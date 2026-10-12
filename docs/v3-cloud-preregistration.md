# Cloud runs: pre-registration (issue #26, `v3-cloud`) — DRAFT

**DRAFT, not frozen.** It is frozen together with [`v3-brokers`](v3-brokers-preregistration.md) after the AWS debug
pass ([runbook](../benchmarks/cloud/README.md), step 4), by the commit that removes this notice and sets `frozen_on`
in both configs, before any registered run.

| | |
|---|---|
| Configurations | [`v3-cloud-e1.json`](../benchmarks/configs/v3-cloud-e1.json), [`v3-cloud-e2.json`](../benchmarks/configs/v3-cloud-e2.json) |
| Analysis | [`benchmarks/prodflow_report.py`](../benchmarks/prodflow_report.py) |
| Plan | #46 |

Changing anything below after results exist requires a new version, and those results stay published.

## Questions

- **E1.** Does the `v2-prodflow-1` result replicate on cloud hardware? It was measured on a local 4-vCPU container.
- **E2.** Over a 30-minute run, with the broker on its own host across a real network and a mid-run change in the
  job mix, does predictive routing keep its gain, recover after the change, and cost nothing in shadow mode?

The workload is synthetic, and the paper must say so (#26).

## Environment

- **AWS us-east-1**, Ubuntu 24.04. Both hosts run the `harbinger-bench` image built from the freeze commit
  ([runbook](../benchmarks/cloud/README.md)).
- **E1** runs on the client host alone (`c8i.2xlarge`, 8 vCPU), with the broker embedded in the benchmark over
  loopback, exactly as in v2 (`benchmarks/run_prodflow.sh` inside the image).
- **E2** runs the broker on a `c8i.xlarge` (4 vCPU) and the producers and workers on the `c8i.2xlarge`, in one
  placement group (`benchmarks/run_prodflow_split.sh`). Every latency timestamp is taken on the client host.
- Each run records instance type, CPU model, kernel, package versions, image IDs and the commit.

## E1: replication (`v3-cloud-e1-1`)

**Setup:** `v2-prodflow-1` unchanged:
- 300 s of arrivals, 4 workers, rate scale 1.8, 180 s drain;
- seeds 11, 22, 33, 44 and 55, arm order rotated per seed;
- the same 7 arms: `fifo`, `static_tuned`, `static_misconfigured`, `shadow`, `predictive`, `predictive_producer`,
  `oracle`.

**Gate:** v2's own gate, with the same thresholds:

| Criterion | Pass if |
|---|---|
| P1 `predictive` mean vs `fifo` | point ≤ −15% and interval below 0 |
| P2 short-class P50 vs `fifo` | point ≤ −30% and interval below 0 |
| G1 all-message P99 ratio vs `fifo` | interval upper bound ≤ 1.25 |
| G2 export max wait ratio vs `fifo` | interval upper bound ≤ 2.0, and no run over 60 s |
| G3 no lost work | every message completed, 0 DLQ, 0 submit errors |
| G4 throughput ratio vs `fifo` | interval lower bound ≥ 0.98 |

**Secondary:** v2's S1–S4.
- S1: within +10% of `static_tuned`.
- S2: beats `static_misconfigured`.
- S3: shadow overhead within ±5%, lookups ≤ 20 µs.
- S4: producer-only keys at least 5% better than FIFO.

**Load depends on the CPU.** Each job's work is fixed in units (rows, pixels, line items), so a faster CPU finishes
it sooner and the same arrival rate is a lighter load. The per-run calibration in `run.json` records this. Effect
sizes are compared with v2 descriptively; only the gate above decides replication.

## E2: 30-minute run (`v3-cloud-e2-1`)

**Timeline.** The production-flow timeline scales with the run length:

| Phase | Minutes |
|---|---|
| steady | 0–6 |
| ramp | 6–9 |
| flash sale | 9–15 |
| recovery | 15–21 |
| **shifted** | **21–30** |

In the shifted phase, high-resolution image uploads rise from 8% to 55%, and one webhook partner slows (slow share 12%
→ 35%). This is the mid-run change #26 asks for.

**Setup:**
- 4 workers, rate scale 1.8, drain up to 600 s.
- Seeds 101 and 202, two repetitions, never used before.
- Arms: `fifo`, `static_tuned`, `shadow` and `predictive`.

**No gate.** Two repetitions are too few for bootstrap intervals. Instead each check below must hold **in every
repetition** (`every_seed`):

| Check | Pass if, in every repetition |
|---|---|
| C1 `predictive` mean vs `fifo` | ≤ −15% |
| C2 short-class P50 vs `fifo` | ≤ −30% |
| C3 all-message P99 vs `fifo` | ≤ +25% |
| C4 export max wait vs `fifo` | ≤ 2× |
| C5 shifted-phase mean (minutes 21–30) vs `fifo` | ≤ −15% (the gain survives the change) |
| C6 `predictive` vs `static_tuned`, mean | ≤ +10% |
| C7 `shadow` vs `fifo`, mean | within ±5% (shadow costs nothing) |
| C8 throughput vs `fifo` | within ±2% |
| G3 no lost work, every arm | every message completed, 0 DLQ, 0 submit errors |

**Reported alongside:**
- mean latency per phase for every arm (median across the two repetitions);
- the predictor's drift alerts, tier agreement and keys;
- the per-class means in the shifted phase.

## What was seen before drafting

- **The local results these runs repeat:**
  - `v2-prodflow-1`: gate passed; `predictive` −28% mean vs FIFO.
  - `v3-sizebins-1` and `v3-aging-prodflow-1`: `predictive` −25% replicated.
  - In `v3-aging-prodflow-1`, the flash sale built backlogs of 16–23 s under FIFO in a 5-minute run.
- **Local test of the cloud driver** (2026-10-12, one laptop, both "hosts" local, seed 7, debug durations):
  - Every run of E1–E4 completed every message.
  - From the 10-minute E2 runs, only the drain was read: 35 s after the last arrival for `fifo`, 34 s for
    `predictive`, with the longest queue wait about 57 s in both. No other latency was examined.
- **No cloud run yet.** The AWS debug pass (seed 7, short durations) will be listed here before freezing.

## Expectations, stated before running

- **E1.** The v2 gate replicates. Effects may be somewhat smaller if the c8i CPUs make jobs shorter and the load
  lighter.
- **E2.** In the 30-minute timeline the flash sale lasts 6 minutes, not 60 s, so FIFO backlogs should be several
  times longer than in v2.
  - That favours predictive routing on mean latency and short-job latency (C1, C2).
  - It is the regime where today's aging pulls everything up and erodes the gain (`v3-aging-1`). C1 may therefore
    pass by less than in v2, or fail.
  - **C5:** after the change, `resize_image` gets heavier and should move down within a few hundred observations.
  - **C7:** shadow costs nothing.

A failed check will be published as such.

## Limits

- Synthetic workload, one cloud provider and region, two instance types.
- E2 has only two repetitions, so its checks are per-repetition, without intervals.
- The network is one placement group: low latency, not a WAN.

## Open decisions before freezing

1. **E2 repetitions**: two (about 5 h), or three (about 7.5 h) so a single odd run cannot pass every check? Default:
   two, as #26 asks.
2. **E2 drain**: 600 s, raised before freezing if the 10-minute debug runs show FIFO barely draining. Default: 600 s.
3. **C1 bar**: −15% in every repetition, the same bar as v2? Default: yes.
