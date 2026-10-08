# Size-binned keys under load: pre-registration (issue #39, `v3-sizebins-1`)

**Frozen 2026-10-08, before any `v3-sizebins-1` run.** Configuration:
[`benchmarks/configs/v3-sizebins.json`](../benchmarks/configs/v3-sizebins.json). Analysis:
[`benchmarks/prodflow_report.py`](../benchmarks/prodflow_report.py). One 30-second smoke run of the new arm (seed 7,
not a registered seed) was made only to check that bins form and nothing is lost; its latency was not examined.
Changing anything below after results exist requires a new version, and these results stay published.

## Question

Does adding the opt-in log2 size bin (#35, `routing.key.size_source`) to the key improve latency in the broker under
load, beyond the job-type keys that passed `v2-prodflow-1`? The only evidence so far is offline (`feature-signal-v1`:
median error factor ~1.95 → 1.2 for size-driven jobs).

## Workload

The `v2-prodflow-1` production-flow benchmark, unchanged except that producers now send a `size_hint` header with the
input size a real producer knows at submission, for the three job types whose cost follows it:

| Job type | `size_hint` | Cost spread |
|---|---|---|
| `resize_image` | pixels: 8% high resolution (~9 MP) vs ~0.8 MP, **rising to 55% after the mid-run shift** | ~11× |
| `generate_invoice` | line items, 1–400 (lognormal, median 12) | wide |
| `export_report` | 250k-row chunks, 40–120 | ~3×, always long |

Every arm sends the header (identical traffic); only `predictive_sized` routes on it. Same seeds (11, 22, 33, 44,
55), 300 s of arrivals, 4 workers, rate scale 1.8, 180 s drain, aging 5000/500 ms, as in v2.

## Arms

- `fifo`: one level.
- `predictive`: producer-scoped `job_type` keys, as in v2.
- `predictive_sized`: the same, plus `size_source: header`, `size_header: size_hint`. A cold bin uses its un-binned
  parent key's prediction when the parent is warm.
- `oracle`: tier from the true planned cost at fixed terciles (a reference, not a ceiling; see the Phase 2 report).

Arm order is rotated per seed by the runner, as in v2.

## Criteria

Paired per-seed values, bootstrap 95% interval (10,000 resamples, seed 2026), as in v2.

**Gate:**

| Criterion | Pass if |
|---|---|
| P1 `predictive_sized` mean latency vs `predictive` | point ≤ −3% and interval below 0 |
| G1 all-message P99 ratio vs `predictive` | interval upper bound ≤ 1.10 |
| G2 mean latency of the six job types without a size hint, vs `predictive` | interval upper bound ≤ +5% |
| G3 no lost work | every message completed, 0 DLQ, 0 submit errors |
| G4 export max wait vs FIFO | ratio upper bound ≤ 2.0 and none over 60 s |
| G5 throughput vs FIFO | ratio lower bound ≥ 0.98 |

**Secondary (reported, not part of the gate):**

- S1 `resize_image` mean latency vs `predictive` (≤ −10%);
- S2 `resize_image` mean latency in the shifted phase vs `predictive` (≤ −10%);
- S3 `generate_invoice` mean latency vs `predictive` (≤ −10%);
- S4 `predictive_sized` vs FIFO mean (≤ −15%);
- S5 `predictive` vs FIFO mean (≤ −15%), checking that the v2 result replicates.
- Also reported: tier agreement, mean |log2 error|, key count and parent fallbacks for both predictive arms.

## Expectation, stated before running

Size-driven jobs are about 21% of messages (`resize_image` ~7%, `generate_invoice` ~14%, `export_report` ~0.3%; job
shares from the smoke run's generated traffic, which follows the same phase mix).
Bins only change routing where a key's bins fall in different tiers or where binning makes a high-spread key
predictable. The overall gain may well be under 3%, and P1 may fail; a null or small result will be published as such.
The resize shift is where a gain is most likely: the un-binned key's median moves when the mix changes, while each
bin's estimate does not.

## Limits

One 4-vCPU machine; one synthetic scenario written by us; simulated network latency; the oracle is approximate.
