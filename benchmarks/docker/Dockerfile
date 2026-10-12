# One image for the production-queue comparison (#37, #47): it builds harbinger_prodflow with the RabbitMQ and Redis
# adapters and runs every broker. Ubuntu 24.04 packages give exactly the versions frozen in #37: RabbitMQ 3.12.1,
# Redis 7.0.15, rabbitmq-c 0.11.0 and hiredis 1.2.0. Package versions are recorded by each run, not pinned here,
# because the archive drops superseded builds; the base image is pinned by digest.
#
#   docker build -f benchmarks/docker/Dockerfile -t harbinger-bench .
#   docker run --rm --network host harbinger-bench rabbitmq        # AMQP on 5672
#   docker run --rm --network host harbinger-bench redis           # Redis on 6379
#   docker run --rm --network host harbinger-bench harbinger_prodflow --arm ... (any benchmark command)
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ git libgrpc++-dev libhiredis-dev libprotobuf-dev librabbitmq-dev ninja-build \
        pkg-config protobuf-compiler-grpc python3 rabbitmq-server redis-server zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Brokers as #37 freezes them: no persistence, defaults otherwise. RabbitMQ accepts the guest user from other hosts
# (the client runs on a second instance); both listen on all interfaces of a private benchmark network.
RUN printf 'loopback_users = none\nlisteners.tcp.default = 5672\n' > /etc/rabbitmq/rabbitmq.conf

WORKDIR /src
COPY . .
RUN cmake -S . -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHARBINGER_BUILD_BENCHMARKS=ON \
        -DHARBINGER_BUILD_BROKER_ADAPTERS=ON \
    && cmake --build /build --target harbinger_prodflow harbinger_server \
    && install -m 0755 /build/benchmarks/harbinger_prodflow /build/harbinger_server /usr/local/bin/ \
    && install -m 0755 benchmarks/docker/entrypoint.sh /usr/local/bin/entrypoint.sh

ENTRYPOINT ["entrypoint.sh"]
