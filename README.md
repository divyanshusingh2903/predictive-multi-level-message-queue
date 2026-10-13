<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/brand/harbinger-lockup-dark.svg">
    <img alt="Harbinger" src="assets/brand/harbinger-lockup.svg" height="64">
  </picture>
</p>

# Harbinger — Predictive Multi-Level Message Queue

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

*The queue that knows what's coming: it learns each kind of message's cost, then prioritizes.*

Harbinger is a gRPC message broker that estimates each message's processing time from the durations it has already
observed for that kind of message (a per-key, decaying duration histogram, not a trained model) and uses the estimate
for multi-level priority scheduling. Operators do not have to classify heavy and light work by hand. Predictive routing
is opt-in; the default is static priority.

**Project page:** <https://divyanshusingh.site/harbinger> · **Source:** <https://github.com/divyanshusingh2903/predictive-multi-level-message-queue>

## Status

Research prototype, in-memory only. The implementation and the evaluation are complete:

- **Broker:** multi-level priority queues with aging, in-flight tracking, retries, a dead-letter queue, and producer and consumer clients.
- **Predictor:** in-process C++ per-key duration statistics, with shadow and predictive routing modes, snapshots and drift counters.
- **Evaluation:** pre-registered experiments on synthetic workloads, public traces and AWS, including comparisons with RabbitMQ and Redis. Results are in [`benchmarks/results/`](benchmarks/results/).
- **Next:** a preprint describing the design and the evaluation.

Not provided: durable storage, TLS or authentication, backpressure, metrics. See
[broker guarantees and limits](docs/internals.md#delivery-guarantees-and-limits).

## Build

```bash
# Ubuntu
sudo apt install -y libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev
# macOS
brew install grpc protobuf

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Running

```bash
./build/harbinger_server                  # broker on 0.0.0.0:50051
./build/harbinger_server 127.0.0.1:50051  # or pick an address
./build/demo/harbinger_demo               # embedded broker, 2 producers, 2 consumers

./build/tests/harbinger_unit_tests
./build/tests/harbinger_integration_tests
```

Server options and the config file are in [configuration](docs/configuration.md).

## Client API

```cpp
auto producer = harbinger::Producer::connect("127.0.0.1:50051");
std::string id = producer->send({0x01, 0x02}, {{"job_type", "resize"}});

auto consumer = harbinger::Consumer::connect(
    "127.0.0.1:50051",
    [](const harbinger::ReceivedMessage& msg) {
        // process msg.payload / msg.headers
        return harbinger::AckResult::SUCCESS;  // FAILURE retries, then dead-letters
    });
consumer->start();
// ...
consumer->stop();
```

## Documentation

See the [documentation index](docs/README.md): [producers](docs/producers.md), [consumers](docs/consumers.md),
[internals](docs/internals.md), [configuration](docs/configuration.md), the
[duration predictor](docs/duration-predictor.md), and the [benchmark guide](benchmarks/README.md).

## License

[MIT](LICENSE) © 2026 Divyanshu Singh
