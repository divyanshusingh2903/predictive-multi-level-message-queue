#!/usr/bin/env bash
# Run the pre-registered production-flow matrix: every arm for each given seed, arm order rotated per seed so slow
# machine drift does not always land on the same arm. Usage: run_prodflow.sh BINARY RESULTS_DIR SEED [SEED...]
# PRODFLOW_CONFIG selects the frozen config (default v2-prodflow.json).
set -euo pipefail
binary=$1; results=$2; shift 2
config=${PRODFLOW_CONFIG:-benchmarks/configs/v2-prodflow.json}
read -r -a arms < <(python3 -c "import json;print(' '.join(json.load(open('$config'))['arms']))")
seconds=$(python3 -c "import json;print(json.load(open('$config'))['seconds'])")
workers=$(python3 -c "import json;print(json.load(open('$config'))['workers'])")
scale=$(python3 -c "import json;print(json.load(open('$config'))['rate_scale'])")
drain=$(python3 -c "import json;print(json.load(open('$config'))['drain_s'])")
for seed in "$@"; do
  offset=$(( seed % ${#arms[@]} ))
  for i in "${!arms[@]}"; do
    arm=${arms[$(( (i + offset) % ${#arms[@]} ))]}
    out="$results/seed-$seed/$arm"
    [ -e "$out/run.json" ] && { echo "skip $out"; continue; }
    rm -rf "$out"
    echo "$(date -u +%FT%TZ) seed=$seed arm=$arm"
    "$binary" --arm "$arm" --seed "$seed" --seconds "$seconds" --workers "$workers" \
      --rate-scale "$scale" --drain-s "$drain" --out "$out" || echo "arm $arm seed $seed exited $?"
  done
done
