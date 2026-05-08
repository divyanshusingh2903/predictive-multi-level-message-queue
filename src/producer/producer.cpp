#include "producer/producer.hpp"

#include <grpcpp/grpcpp.h>

#include <stdexcept>

namespace pmlmq {

Producer::Producer(std::string id,
                   std::unique_ptr<pmlmq_rpc::Broker::Stub> stub)
    : id_(std::move(id)), stub_(std::move(stub)) {}

std::shared_ptr<Producer> Producer::connect(const std::string& server_addr) {
    auto channel = grpc::CreateChannel(server_addr,
                                       grpc::InsecureChannelCredentials());
    auto stub = pmlmq_rpc::Broker::NewStub(channel);

    pmlmq_rpc::RegisterProducerRequest  req;
    pmlmq_rpc::RegisterProducerResponse resp;
    grpc::ClientContext ctx;

    const auto status = stub->RegisterProducer(&ctx, req, &resp);
    if (!status.ok()) {
        throw std::runtime_error(
            "Producer::connect failed: " + status.error_message());
    }

    return std::shared_ptr<Producer>(
        new Producer(resp.producer_id(), std::move(stub)));
}

std::string Producer::send(std::vector<uint8_t> payload,
                            std::unordered_map<std::string, std::string> headers) {
    pmlmq_rpc::SubmitRequest req;
    req.set_producer_id(id_);
    req.set_payload(std::string(payload.begin(), payload.end()));
    for (const auto& [k, v] : headers) {
        (*req.mutable_headers())[k] = v;
    }

    pmlmq_rpc::SubmitResponse resp;
    grpc::ClientContext ctx;

    const auto status = stub_->Submit(&ctx, req, &resp);
    if (!status.ok()) {
        throw std::runtime_error("Producer::send failed: " + status.error_message());
    }

    sent_.fetch_add(1, std::memory_order_relaxed);
    return resp.message_id();
}

} // namespace pmlmq
