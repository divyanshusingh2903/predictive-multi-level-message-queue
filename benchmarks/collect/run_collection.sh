#!/usr/bin/env bash
# Run the pre-registered feature-signal-v1 collection: every (minutes, seed) pair from the config, skipping runs that
# already finished. Usage: run_collection.sh PYTHON CORPUS_DIR OUT_DIR [MINUTES...]
set -euo pipefail
python=$1; corpus=$2; out=$3; shift 3
config=benchmarks/configs/feature-signal-v1.json
mkdir -p "$out"
minutes=("$@")
[ ${#minutes[@]} -gt 0 ] || read -r -a minutes < <(python3 -c "import json;print(' '.join(str(m) for m in json.load(open('$config'))['collection']['runs_minutes']))")
for m in "${minutes[@]}"; do
  for seed in $(python3 -c "import json;print(' '.join(str(s) for s in json.load(open('$config'))['collection']['seeds']['$m']))"); do
    file="$out/m$m-s$seed.csv"
    [ -s "$file.done" ] && { echo "skip $file"; continue; }
    rm -f "$file"
    echo "$(date -u +%FT%TZ) collecting ${m} min seed $seed"
    "$python" benchmarks/collect/collect_jobs.py "$corpus" "$file" --minutes "$m" --seed "$seed" --workers 3
    sha256sum "$file" > "$file.done"
  done
done
