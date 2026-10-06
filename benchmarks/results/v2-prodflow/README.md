# v2 production-flow results (`v2-prodflow-1`)

Pre-registered in [docs/v2-preregistration.md](../../../docs/v2-preregistration.md); summarized in
[docs/phase2-report.md](../../../docs/phase2-report.md).

- `report.md`, `summary.json`: output of `python -m benchmarks.prodflow_report benchmarks/configs/v2-prodflow.json <raw dir>`.
- `runs/seed-<seed>-<arm>.json`: each run's `run.json` (counts, calibration, oracle boundaries, broker and predictor counters).
- `SHA256SUMS`: hashes of every raw file (`raw/seed-*/<arm>/{messages.tsv,run.json}`, 128 MB, not committed).
- `matrix-log.txt`: the runner log, including container restarts (interrupted arms were re-run from scratch) and the
  one arm (seed 11 oracle) re-run because it overlapped a local build.

Reproduce: build with `-DHARBINGER_BUILD_BENCHMARKS=ON`, then
`benchmarks/run_prodflow.sh build/benchmarks/harbinger_prodflow <raw dir> 11 22 33 44 55` (about 3 hours on 4 vCPUs) and
run the report command above. Absolute numbers depend on the machine.
