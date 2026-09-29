#include <harbinger.grpc.pb.h>
#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>

extern char** environ;
using namespace std::chrono_literals;

namespace {
class ServerChild {
public:
    explicit ServerChild(std::string address = "127.0.0.1:0") {
        int pipes[2];
        if (pipe(pipes) != 0) throw std::runtime_error("pipe failed");
        fcntl(pipes[0], F_SETFD, FD_CLOEXEC);
        fcntl(pipes[1], F_SETFD, FD_CLOEXEC);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipes[0]);
        posix_spawn_file_actions_addclose(&actions, pipes[1]);
        std::string executable = SERVER_EXECUTABLE;
        char* args[] = {executable.data(), address.data(), nullptr};
        const int result = posix_spawn(&pid_, executable.c_str(), &actions, nullptr, args, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(pipes[1]);
        output_ = pipes[0];
        if (result != 0) {
            close(output_);
            throw std::runtime_error("spawn failed");
        }
    }
    ~ServerChild() {
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        }
        close(output_);
    }
    int ready_port() {
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto marker = output.find("Ready port=");
            if (marker != std::string::npos && output.find('\n', marker) != std::string::npos)
                return std::stoi(output.substr(marker + 11));
            pollfd fd{output_, POLLIN, 0};
            if (poll(&fd, 1, 100) > 0) {
                char buffer[4096];
                const auto count = read(output_, buffer, sizeof(buffer));
                if (count <= 0) break;
                output.append(buffer, static_cast<std::size_t>(count));
            }
        }
        return 0;
    }
    void signal(int value) { ASSERT_EQ(kill(pid_, value), 0); }
    int wait() {
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        while (std::chrono::steady_clock::now() < deadline) {
            int status = 0;
            if (waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            std::this_thread::sleep_for(10ms);
        }
        return -1;
    }
    std::string output;
private:
    pid_t pid_{-1};
    int output_{-1};
};
}

TEST(ServerShutdown, SigintAndSigtermExitNormally) {
    for (const int signal : {SIGINT, SIGTERM}) {
        ServerChild child;
        ASSERT_GT(child.ready_port(), 0) << child.output;
        child.signal(signal);
        EXPECT_EQ(child.wait(), 0) << child.output;
    }
}

TEST(ServerShutdown, SigtermDuringPull) {
    ServerChild child;
    const auto port = child.ready_port();
    ASSERT_GT(port, 0) << child.output;
    auto stub = harbinger_rpc::Broker::NewStub(grpc::CreateChannel(
        "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    grpc::ClientContext registration;
    registration.set_deadline(std::chrono::system_clock::now() + 3s);
    harbinger_rpc::RegisterConsumerRequest req;
    harbinger_rpc::RegisterConsumerResponse resp;
    ASSERT_TRUE(stub->RegisterConsumer(&registration, req, &resp).ok());
    auto pull = std::async(std::launch::async, [&] {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + 7s);
        harbinger_rpc::PullRequest request;
        request.set_consumer_id(resp.consumer_id());
        request.set_timeout_ms(5000);
        harbinger_rpc::PullResponse response;
        return stub->Pull(&ctx, request, &response);
    });
    EXPECT_EQ(pull.wait_for(200ms), std::future_status::timeout);
    child.signal(SIGTERM);
    EXPECT_EQ(child.wait(), 0);
    EXPECT_EQ(pull.wait_for(1s), std::future_status::ready);
}

TEST(ServerShutdown, BindFailureExitsNonzero) {
    const int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(socket_fd, 0);
    struct SocketGuard { int fd; ~SocketGuard() { close(fd); } } guard{socket_fd};
    fcntl(socket_fd, F_SETFD, FD_CLOEXEC);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(bind(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(listen(socket_fd, 1), 0);
    socklen_t length = sizeof(address);
    ASSERT_EQ(getsockname(socket_fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
    ServerChild child{"127.0.0.1:" + std::to_string(ntohs(address.sin_port))};
    EXPECT_EQ(child.ready_port(), 0);
    EXPECT_EQ(child.wait(), EXIT_FAILURE) << child.output;
}
