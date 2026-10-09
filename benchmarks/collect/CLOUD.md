# Repeating the #24 collection on a cloud VM

The local runs in this repository used a shared 4-vCPU container. A cloud repeat adds a second machine and the
60-minute runs that `feature-signal-v1` leaves out locally. Nothing here needs credentials in the repository.

## Plan and spend cap

| Item | Choice |
|---|---|
| Machine | 4 dedicated vCPUs, 16 GB, Ubuntu 24.04: AWS `c6i.xlarge`, GCP `c2d-standard-4` or Azure `F4s_v2` |
| Disk | 30 GB standard SSD (corpus ~0.8 GB, trace ~0.3 GB, results < 100 MB) |
| Runtime | about 3 h for 5/10/30/60-minute runs, two seeds each (setup ~10 min) |
| Expected cost | roughly USD 0.20 per hour of compute plus disk; under USD 2 for the whole job |
| Hard cap | the script schedules `shutdown -h +300` (5 h) when it starts; also set a USD 10 budget alert on the account |
| Teardown | delete the VM and its disk after copying the archive; nothing needs to persist |

The 60-minute seeds are not in the frozen config (which covers 5/10/30). Add `"60": [601, 602]` to a **new**
config version (for example `feature-signal-v1-cloud`) before running, and keep the local v1 results unchanged.

## Steps

```bash
# 1. Create the VM in your cloud console (or CLI) with the machine above; SSH in.
# 2. Fetch and run the kit (it installs dependencies, downloads and verifies the corpora, collects, analyses):
curl -fsSLO https://raw.githubusercontent.com/divyanshusingh2903/predictive-multi-level-message-queue/main/benchmarks/collect/cloud_run.sh
bash cloud_run.sh            # or: bash cloud_run.sh 60  for only the long runs
# 3. Copy the archive back, e.g.:  scp vm:feature-signal/feature-signal-*.tar.gz .
# 4. Delete the VM and its disk.
```

Bring the archive back and run `python -m analysis.feature_signal` over its CSVs (the archive already contains
`analysis.json`). Compare with the local result in `benchmarks/results/feature-signal-v1/`.
