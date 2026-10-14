# Aging and size-bin production flows on AWS: pre-registration (`v4-cloud-reruns`) — DRAFT, NOT FROZEN

**Status: draft.** It becomes a pre-registration only when the commit that freezes it exists, before any registered
run. After freezing, changing anything requires a new version, and these results stay published.

| | |
|---|---|
| Issue | TBD (create before freezing) |
| Configurations | [`v4-cloud-aging.json`](../benchmarks/configs/v4-cloud-aging.json) (E5), [`v4-cloud-sizebins.json`](../benchmarks/configs/v4-cloud-sizebins.json) (E6) |
| Replicates | [`v3-aging-prodflow-1`](v3-aging-preregistration.md) and [`v3-sizebins-1`](v3-sizebins-preregistration.md) |
| Analysis | [`benchmarks/prodflow_report.py`](../benchmarks/prodflow_report.py), unchanged |
| Runner | `benchmarks/cloud/run_all.sh`, experiments `e5` and `e6` |

## Why this experiment

Both experiments were run on a laptop (first in a container, then in the quiet-machine re-run). Every other
production-flow result in the paper has an AWS replication (`v3-cloud` E1). These two do not. This closes that gap with
the **same frozen configurations**, so that the paper's production-flow results all come from, or are replicated on,
the same cloud hardware.

## Question

Do the verdicts of `v3-aging-prodflow-1` and `v3-sizebins-1` replicate on AWS hardware?

Nothing about the experiments changes: arms, seeds, workload, criteria, bars and the analysis are those of the frozen
configurations. The analysed content of the two new configs is identical to the originals (checked when the files were
made); only the version, environment and pointer fields differ.

## Setup

As `v3-cloud-e1`: a `c8i.2xlarge` (8 vCPU, 16 GiB) in `us-east-1`, alone, Ubuntu 24.04, with the broker embedded in
`harbinger_prodflow` over loopback, run by `benchmarks/run_prodflow.sh` inside the `harbinger-bench` image built from the
freeze commit. The two experiments run one after the other on the same host.

| | Arms | Seeds | Runs | Expected time |
|---|---|---|---|---|
| E5 aging | `fifo`, `predictive`, `predictive_weighted` | 11, 22, 33, 44, 55 | 15 | about 1.3 h |
| E6 size bins | `fifo`, `predictive`, `predictive_sized`, `oracle` | 11, 22, 33, 44, 55 | 20 | about 1.7 h |

Arm order is rotated per seed by the runner, as before.

## What was seen before freezing

The local results of both experiments are published, including the quiet-machine re-run, and are what this experiment
replicates:

- **E5 (aging).** The gate did **not** pass locally: all-message P99 under weights and pausing aging was 1.125 times
  that of predictive routing, over its 1.1 limit. The other gate criteria passed (long-class wait 1.14 against 2.0, the
  v2 gain kept at −56.7%, throughput level), and mean latency was 41.9% below predictive routing's.
- **E6 (size bins).** The gate did **not** pass locally: mean latency under size-binned keys was 0.7% *above*
  predictive routing's, against a bar of 3% below it. The guardrails passed. Resize improved by 2.8% (bar 10%), and the
  invoice class was 8.7% worse.

No run of either experiment on AWS has been made. A one-seed debug pass (seed 7, which is not registered, 60 seconds per
arm) will check that the runner works; its latencies are not read.

## Criteria

**Replication, per experiment.** All of the following are evaluated on the AWS results, with the frozen configs'
criteria and the frozen bars:

| ID | Rule |
|---|---|
| RP1 | the gate outcome (passed or not passed) equals the local quiet-machine re-run's: not passed for both E5 and E6 |
| RP2 | for each criterion, its pass or fail equals the local re-run's |
| RP3 | (reported, not gated) each criterion's point estimate against the local re-run's, with both intervals |

The original criteria and bars are not re-evaluated or loosened. A criterion that differs from the local result is
reported as such.

## What happens either way

- If both gates replicate, the paper reports the AWS result beside the local one and states that cloud hardware did
  not change a verdict.
- If a verdict changes, the paper reports both and does not choose between them. A verdict that depends on the machine
  is a finding about the design's sensitivity to load, as the quiet-machine re-run already showed for effect sizes.
- If the pre-registered gate passes on AWS where it did not locally, that is reported as a replication failure of the
  local result and is not treated as a pass of the design.

## Required changes before freezing (done, uncommitted)

1. `benchmarks/configs/v4-cloud-aging.json` and `v4-cloud-sizebins.json`, copies of the frozen configs with `frozen_on`
   still `null`.
2. `benchmarks/cloud/run_all.sh` runs them as `e5` and `e6`, refuses unfrozen configs in registered mode, and has
   debug overrides for seed 7.
3. At freeze: set `frozen_on` in both configs and remove the DRAFT notices.

## Threats

- One region, one instance type, one host; effect sizes depend on CPU speed, as the paper's re-run shows.
- The synthetic production flow, as before.
- The two experiments share a host and run in sequence, so a drift in the host's conditions over the three hours
  would affect E6 more than E5. The manifest records the host's conditions.
