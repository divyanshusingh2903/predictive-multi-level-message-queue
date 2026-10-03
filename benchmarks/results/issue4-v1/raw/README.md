# Raw issue #4 experiment history

These compressed snapshots preserve the complete generated artifact directories,
including manifests, traces, submissions, dispatch/outcome observations, measured
handler runtimes, per-message accounting, sealed feedback, and dataset audit and
label streams. `../calibration/` and `../baselines/` provide readable summaries
without requiring extraction. Invalid/lossy trials remain in the history.

## Snapshots

| Archive | Contents and role |
|---|---|
| `calibration-final-v1.tar.gz` | Final 20-trial capacity calibration used to freeze arrival rates |
| `baselines-v1.tar.gz` | Complete frozen 750-trial baseline matrix, with raw replay and feedback artifacts |
| `baseline-dataset-final-v1.tar.gz` | Final static-feedback export, including all evaluation ingress and delayed-label accounting |
| `clean-static-dataset-v1.tar.gz` | Verified clean individual-run export: 120 train and 80 evaluation labels |
| `development-calibration-v1.tar.gz` | Earlier 15-trial calibration before the final generator/timing contract was frozen |
| `development-smoke.tar.gz` | Initial baseline and expiry smoke trials |
| `development-dataset.tar.gz` | Initial smoke dataset export |
| `development-baseline-dataset-v1.tar.gz` | Earlier full-matrix export, before preserving label-missing evaluation ingress |

Development snapshots document intermediate behavior and are not substitutes for
the final frozen evidence. Their input traces are retained even where the current
generator/exporter has changed. Manifests record their original versions and
coverage; an export with `valid_for_learning=false` remains incomplete evidence.

## Verify and extract

Run checksum verification from this directory:

```bash
sha256sum -c SHA256SUMS
```

On macOS, use `shasum -a 256 -c SHA256SUMS` instead. Each archive contains one
top-level directory named after its original run/export. For example:

```bash
mkdir -p /tmp/harbinger-history
tar -xzf baselines-v1.tar.gz -C /tmp/harbinger-history
tar -xzf baseline-dataset-final-v1.tar.gz -C /tmp/harbinger-history
```

Archived runner configurations retain their original output paths for provenance;
regenerate a fresh run with the documented experiment CLI rather than treating an
old `runner.conf` as a portable replay command. Feedback is telemetry, not broker
delivery state. These files cannot restore an in-memory queue or consumer leases.

No predictive model or activation result is represented by these archives.
