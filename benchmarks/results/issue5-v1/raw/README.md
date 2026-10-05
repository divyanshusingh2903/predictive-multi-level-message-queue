# Raw online-comparison evidence

> **The `*.tar.gz` archives are not tracked in git** (about 103 MB). Only this
> guide, `inventory.json` and `SHA256SUMS` are versioned, so the archives can be
> verified against the recorded hashes wherever they are stored. Tests that need
> the archives skip when they are absent.

`inventory.json` gives SHA-256 hashes and exact compressed byte counts;
`SHA256SUMS` supports command-line verification:

```bash
sha256sum -c SHA256SUMS
```

| Archive | Root and purpose |
|---|---|
| `collection-v1.tar.gz` | `issue5-runs-v1/`: all 60 initial traces, runner settings, submissions, runtimes, observations, sealed feedback and summaries |
| `replacement-*.tar.gz` | Same-name roots: all matching replacement attempts and incidental unused repeats, including the two lossy replacement attempts |
| `dataset-v1.tar.gz` | `dataset/`: validated audit/attempt/message views, train/evaluation streams and provenance manifest for the 60 admitted clean trials |
| `comparison-v1.tar.gz` | `issue5-comparison-v1-final/`: complete reports, original ingress forecasts and score-before-learning evidence, profiling and delay sensitivity |

Archive headers have normalized owner and modification time, and gzip timestamps
are fixed. No archive is a broker-delivery backup or a reusable production model.
Model state is rebuilt independently for every run.

To replay the retained dataset without collecting messages again, extract from
this archive directory into an existing scratch directory:

```bash
tar -xzf dataset-v1.tar.gz -C /tmp/opencode
```

Then, from the repository root, use the pinned environment:

```bash
.venv/bin/python -m ml_engine.compare --dataset /tmp/opencode/dataset \
  --config ml_engine/configs/online-comparison-v1.json \
  --output /tmp/opencode/issue5-replayed-report
```

Use a fresh extraction/output location. Numeric replay is
deterministic for the same dataset/configuration and pinned environment. Resource
timings/RSS are new host measurements and need not match the retained values.
