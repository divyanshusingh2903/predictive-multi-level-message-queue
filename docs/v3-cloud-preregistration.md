# Cloud runs: pre-registration (issue #26, `v3-cloud`)

**Frozen 2026-10-12, together with [`v3-brokers`](v3-brokers-preregistration.md), after the AWS debug pass and before
any registered run.**

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

## What was seen before freezing

- **The local results these runs repeat:**
  - `v2-prodflow-1`: gate passed; `predictive` −28% mean vs FIFO.
  - `v3-sizebins-1` and `v3-aging-prodflow-1`: `predictive` −25% replicated.
  - In `v3-aging-prodflow-1`, the flash sale built backlogs of 16–23 s under FIFO in a 5-minute run.
- **Local test of the cloud driver** (2026-10-12, one laptop, both "hosts" local, seed 7, debug durations):
  - Every run of E1–E4 completed every message.
  - From the 10-minute E2 runs, only the drain was read: 35 s after the last arrival for `fifo`, 34 s for
    `predictive`, with the longest queue wait about 57 s in both. No other latency was examined.
- **AWS debug pass** (2026-10-12, 22:25–23:00 UTC; the registered hosts; seed 7, debug durations; commit
  `f42f21d`):
  - **Hosts and image:** client `c8i.2xlarge`, broker `c8i.xlarge`, both Xeon 6975P-C, kernel 7.0.0-1014-aws, us-east-1a,
    one placement group. Identical image `sha256:65821c67…` and package versions on both.
  - **Completeness:** all 12 production-flow runs (E1 2, E2 2, E3 8) completed every message, with no worker errors
    and every Harbinger run merged. All 12 E4 runs lost nothing.
  - **CPU speed:** the client's kernel calibration (resize 6.6 ms/megapixel, export 17.5 ms/chunk) is about 2× faster
    than the local quiet re-run and about 20% faster than the original v2 run, so the same workload is a lighter load
    here.
  - **E2 drain:** from the 10-minute E2 runs, only the drain was read: 0.6 s after the last arrival for both arms, with
    the longest queue wait 47 s. The 600 s drain stays. No other E1 or E2 latency was examined.

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

## Decisions settled before freezing (2026-10-12)

1. **E2 repetitions:** two, as #26 asks.
2. **E2 drain:** 600 s. The debug pass drained in 0.6 s.
3. **C1 bar:** −15% in every repetition, the same bar as v2.
4. **Running order:** all four experiments run sequentially on one pair (E4, E3, E2, then E1), about 15 h.

## Amendment 1 (2026-10-13, 03:30 UTC): E1 on its own host

Decided and written after E4 and E3 had finished and E2 had started, before the first E1 run. Committed at about
05:30 UTC, while E2 and E1 were running. Up to the commit, no result of any experiment had been read except message
counts: every E3, E2 and E1 run so far had completed every message, with no DLQ and no submit errors.

- **Why:** the account's vCPU quota rose from 16 to 32 during the runs. E1 needs no broker, so it can run alongside E2.
  This ends the registered set about 4.5 h sooner.
- **Change to decision 4:** E4, E3 and E2 run sequentially on the registered pair. E1 runs on a third host, started
  at 03:37:59 UTC from the same frozen commit `2da0e3a` with `EXPERIMENTS=e1`, into `registered-2da0e3a-e1`.
- **The E1 host** (`i-0e19834bb6489c1a0`) matches the pair's client host: `c8i.2xlarge`, Xeon 6975P-C, 8 vCPU,
  kernel 7.0.0-1014-aws, the same AMI and Docker version, us-east-1a, the same placement group.
- **Image:** rebuilt on the E1 host from `2da0e3a` (`sha256:2b02939e…`), not copied from the pair
  (`sha256:8952ed0d…`), because copying would load the pair's hosts during E2. Same Dockerfile, pinned base image and
  package versions.
- **The pair does not run E1.** Before E2 ends, its E1 config is made unreadable on the client host, so `run_all.sh`
  stops at the E1 step before running anything. Its results are then archived by hand.
- **Unchanged:** every config, criterion, seed and analysis.
