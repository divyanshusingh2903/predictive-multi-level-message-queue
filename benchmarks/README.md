# Phase 1 cleanup measurements

Build explicitly; this target is excluded by default and has no CI timing gate:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DHARBINGER_BUILD_BENCHMARKS=ON
cmake --build build --target harbinger_cleanup_benchmark --parallel
./build/benchmarks/harbinger_cleanup_benchmark queue 100000 50000 4 young
./build/benchmarks/harbinger_cleanup_benchmark queue 100000 50000 4 mixed
./build/benchmarks/harbinger_cleanup_benchmark queue 100000 50000 4 dense
./build/benchmarks/harbinger_cleanup_benchmark queue 100000 50000 4 off
./build/benchmarks/harbinger_cleanup_benchmark delivery 1000 65536 64 4
```

Queue workers dequeue and replace work to maintain backlog; reported latency is
one dequeue/placement pair. `young` uses a one-hour threshold, `mixed` backdates
one placement in 100, and `dense` backdates every placement. Backdated placements
use front restoration, exercising timestamp-order exceptions. Due populations
change as aging promotes work; these are workload labels, not measured fixed
due ratios. The minimum observed depth and final depth show whether backlog was
maintained. Aging-disabled runs provide a contention comparison. Small runs may
finish before an aging interval: use enough iterations to observe several scans.

Delivery mode uses an embedded loopback broker, a single producer and the chosen
number of consumers. Payloads are generated before timing with seed 42. Latencies
measure from Submit start to handler entry, including queue wait; throughput is
confirmed Ack outcomes divided by elapsed time. Handlers add no simulated delay.
The requested application header count excludes producer/sequence metadata.
Client registration/setup and shutdown are outside the measured interval.

Compare identical Release builds, compiler/dependency versions, machine load,
parameters, and several repetitions before and after changes. Try queue backlogs
1k/10k/100k and delivery payloads 0/1KiB/64KiB/1MiB, headers 0/8/64, workers 1/4.
Record all results, including regressions; wall-clock speed is not a correctness
assertion. `/usr/bin/time -v` can give coarse whole-process memory usage. This
harness does not isolate allocations or lock hold times and is not a substitute
for Phase 2/3 scheduler comparisons or production trace benchmarks.

## Local comparison for issue #14

Measured on Linux x86-64 with GCC 15.2.0, Release (`-O3`), the same Conda
gRPC/Protobuf installation, and three sequential repetitions per scenario.
Baseline used broker/queue code at `efb3638` plus this harness; comparison used
the #14 working-tree changes. Queue parameters were `100000 50000 4 PATTERN`;
delivery parameters were `1000 65536 64 4`. Queue depth stayed between 99,996
and 100,000 in every run. Values below are medians of the three run summaries,
not percentiles computed across pooled samples.

| Scenario | Baseline outcomes/s | After outcomes/s | Baseline P50 / P99 (µs) | After P50 / P99 (µs) |
|---|---:|---:|---:|---:|
| Queue, young | 605,301 | 681,836 | 3.425 / 24.004 | 4.076 / 24.043 |
| Queue, mixed | 576,206 | 560,933 | 3.647 / 24.320 | 3.621 / 24.292 |
| Queue, dense | 540,243 | 593,032 | 3.476 / 25.528 | 4.575 / 26.329 |
| Queue, aging off | 722,300 | 695,556 | 3.508 / 24.675 | 3.916 / 23.214 |
| Loopback delivery | 2,387.63 | 2,409.94 | 630.733 / 840.420 | 600.403 / 808.892 |

Throughput ranges across baseline/after runs, respectively: young
569,796–623,375 / 633,352–701,007; mixed 570,875–587,053 /
520,136–564,870; dense 535,223–551,906 / 578,198–596,742; aging-off
702,788–730,346 / 680,699–720,768; delivery 2,386.57–2,416.81 /
2,384.94–2,423.56.

These short, unpinned runs are evidence of workload-dependent trade-offs, not
statistically established speedups. In particular, mixed throughput regressed,
dense P50/P99 increased, and the aging-off control also varied. Young-level
skips reduce scanning work by construction (covered by deterministic visit-count
tests), but do not guarantee lower latency on every workload. Delivery throughput
was essentially unchanged; its measured latency improved modestly. Use longer,
repeated runs on a controlled machine before making capacity claims.
