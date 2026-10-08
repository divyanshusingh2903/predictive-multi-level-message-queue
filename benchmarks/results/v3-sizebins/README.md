# Size-binned keys under load (`v3-sizebins-1`, issue #39)

Pre-registered in [docs/v3-sizebins-preregistration.md](../../../docs/v3-sizebins-preregistration.md) (frozen in commit
`3c4762c` before any registered run).

**Result: the gate did NOT pass.** Size-binned keys lowered mean latency by 1.5% versus job-type keys, but the 95%
interval includes no change, and the point estimate is short of the pre-registered −3%. Every guardrail passed.

| Criterion | Point | 95% CI | Result |
|---|---|---|---|
| P1 `predictive_sized` mean vs `predictive` (≤ −3%, interval below 0) | −1.5% | [−5.1%, +1.7%] | fail |
| G1 all-message P99 ratio vs `predictive` (≤ 1.10) | 1.00× | [0.99, 1.02] | pass |
| G2 classes without a size hint vs `predictive` (≤ +5%) | −2.3% | [−5.8%, +0.6%] | pass |
| G3 no lost work | all 20 runs complete, 0 DLQ, 0 submit errors | | pass |
| G4 export max wait vs FIFO (≤ 2×, none over 60 s) | 1.02× | [0.97, 1.04] | pass |
| G5 throughput vs FIFO (≥ 0.98) | 1.00× | [0.99, 1.00] | pass |
| S1 `resize_image` mean vs `predictive` (≤ −10%) | −4.4% | [−8.1%, −0.8%] | fail |
| S2 `resize_image` shifted-phase mean vs `predictive` (≤ −10%) | −5.1% | [−14.3%, +2.9%] | fail |
| S3 `generate_invoice` mean vs `predictive` (≤ −10%) | **+11.1%** | [+3.5%, +21.0%] | fail |
| S4 `predictive_sized` mean vs FIFO (≤ −15%) | −26.2% | [−31.7%, −22.4%] | pass |
| S5 `predictive` mean vs FIFO (≤ −15%; v2 replicates) | −25.1% | [−29.0%, −21.7%] | pass |

Full tables: [`report.md`](report.md).

## What the bins did

The predictor itself got better with bins: mean |log2 error| dropped from 0.84 to 0.66 octaves (median across seeds).
It used 28 keys instead of 9, and about 260 lookups per run were answered by a cold bin's parent key. Tier agreement
barely moved (0.80 → 0.81), and routing changed only modestly.

Exploratory breakdown by planned cost (`exploratory_by_cost.py`, **not pre-registered**; latency means, median across
seeds):

| Class, planned-cost third | `predictive` | `predictive_sized` | Change |
|---|---|---|---|
| `generate_invoice`, cheapest | 4,410 ms | 4,418 ms | +0% |
| `generate_invoice`, middle | 3,990 ms | 4,239 ms | +6% |
| `generate_invoice`, costliest | 4,201 ms | 5,357 ms | **+28%** |
| `resize_image`, cheapest | 10,252 ms | 9,007 ms | **−12%** |
| `resize_image`, middle | 10,431 ms | 10,301 ms | −1% |
| `resize_image`, costliest | 9,300 ms | 9,549 ms | +3% |

This is shortest-expected-first scheduling working as intended. Large invoices were recognised as longer and moved
down; small resizes were recognised as shorter and moved up. The net gain is small because:

- **Invoices are cheap.** The median planned cost is about 5 ms, and invoices use 2.7% of worker time. Demoting the
  large ones cost them more than it saved everyone else.
- **Size-driven jobs are a minority.** Resizes use about 9.5% of worker time and exports about 9.4%. Most of the
  benefit available from shortest-expected-first scheduling was already captured by the job-type keys (`predictive`
  vs FIFO −25%, oracle −28.5% on medians).

## Conclusion

Size-binned keys stay **opt-in**, as shipped in #35. On this workload they make the predictor more accurate but
change latency by an amount we could not distinguish from zero. They are most likely to help when a single job type
spans tiers *and* carries a large share of the work; this scenario has no such job type. That was not tested here and
would need a new pre-registered workload.

## Files

- `report.md`, `summary.json`: output of `python -m benchmarks.prodflow_report benchmarks/configs/v3-sizebins.json <raw dir>`.
- `runs/seed-<seed>-<arm>.json`: each run's `run.json` (counts, calibration, oracle boundaries, broker and predictor counters).
- `SHA256SUMS`: hashes of every raw file (`raw/seed-*/<arm>/{messages.tsv,run.json}`, 74 MB, not committed).
- `matrix-log.txt`: the runner log. All 20 runs completed on the first attempt, with no restarts.
- `exploratory_by_cost.py`: the exploratory breakdown above.

Reproduce: build with `-DHARBINGER_BUILD_BENCHMARKS=ON`, then
`PRODFLOW_CONFIG=benchmarks/configs/v3-sizebins.json benchmarks/run_prodflow.sh build/benchmarks/harbinger_prodflow <raw dir> 11 22 33 44 55`
(about 1 h 45 min on 4 vCPUs), and run the report command above. Absolute numbers depend on the machine.
