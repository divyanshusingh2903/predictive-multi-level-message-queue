# Feature signal beyond the key (`feature-signal-v1`, issue #24)

Pre-registered in [docs/v2-preregistration.md](../../../docs/v2-preregistration.md#addendum-feature-signal-beyond-the-key-issue-24-feature-signal-v1)
before any counted run. Six runs (5, 10 and 30 minutes, two seeds each), 568,592 jobs, real handlers on real inputs,
three worker processes on a shared 4-vCPU container.

## Result: a binned size feature is worthwhile; a richer model is not

Median |log2(predicted / actual)| and tier accuracy on the held-out last 40% of each run (all six runs agree within
±0.03):

| Model | Median error (log2) | ≈ error factor | Tier accuracy |
|---|---|---|---|
| Global median | 2.89 | 7.4× | 35% |
| Per-key median (the broker today) | 0.96 | 1.95× | 68% |
| Per-key × floor(log2(size)) median | 0.27 | 1.2× | 93% |
| Gradient boosting (key + all features) | 0.23 | 1.17× | 93% |

Decision rule outcome: binned feature wins in 6 of 6 runs (5 required); gradient boosting beats the binned model by
≥ 0.25 in 0 of 6. **Verdict: binned size feature worthwhile.**

Per key (30-minute run, seed 301):

| Key | Within-key p99/p50 | Per-key error | Binned error |
|---|---|---|---|
| catalog/resize_image | 46 | 4.02 | 0.16 |
| api/parse_json | 39 | 2.25 | 0.29 |
| reports/sqlite_report | 37 | 1.93 | 0.26 |
| storage/compress_file | 11 | 0.90 | 0.24 |
| search/index_text | 8 | 0.88 | 0.27 |
| storage/checksum | 5 | 0.52 | 0.33 |
| notify/http_call (control) | 3 | 0.48 | 0.48 |

The control key, whose latency does not depend on its payload, gains nothing, as it should. The three keys whose cost
scales with content have p99/p50 spreads of 37–46, so the broker's spread gate (16) sends them to the default tier
today; size bins would make them routable.

## Files

- `analysis.json`: full output of `python -m analysis.feature_signal benchmarks/configs/feature-signal-v1.json raw/*.csv`.
- `corpus-manifest.json`: every corpus file with its kind, size, dimensions or rows, and SHA-256.
- `SHA256SUMS`: hashes of the six raw CSVs (69 MB, not committed). `collection-log.txt`: run log.

Reproduce: `python benchmarks/collect/prepare_corpus.py …` then `benchmarks/collect/run_collection.sh` (see the
script headers), or the cloud kit in [`benchmarks/collect/CLOUD.md`](../../../benchmarks/collect/CLOUD.md).

## Limits

One machine; job mix, corpus and handlers chosen by us; millisecond-scale jobs; the larger photos are real photos
upscaled to camera resolutions. The result shows that size carries most of the remaining signal for content-dependent
jobs like these; it does not say how common such jobs are in a given deployment. A producer must send the size as a
header unless the content itself is the message payload.

> The offline tools used here lived in `ml_engine/` when these results were produced; the folder is now `analysis/` (same code), so manifests record the old paths.
