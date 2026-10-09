#!/usr/bin/env bash
# Repeat the feature-signal-v1 collection on a fresh Ubuntu 24.04 VM (see benchmarks/collect/CLOUD.md).
# Usage on the VM: bash cloud_run.sh [MINUTES...]   (default: 60, plus the 5/10/30 runs for a cross-machine repeat)
# The VM powers itself off after 5 hours no matter what, as a spend cap.
set -euo pipefail
sudo shutdown -h +300 "feature-signal-v1 spend cap" || true
minutes=("$@"); [ ${#minutes[@]} -gt 0 ] || minutes=(5 10 30 60)
sudo apt-get update -qq && sudo apt-get install -y -qq git python3-venv unrar >/dev/null
work=$HOME/feature-signal && mkdir -p "$work" && cd "$work"
[ -d predictive-multi-level-message-queue ] || git clone https://github.com/divyanshusingh2903/predictive-multi-level-message-queue
cd predictive-multi-level-message-queue
python3 -m venv "$work/venv" && "$work/venv/bin/pip" install -q -r analysis/requirements-analysis.txt
if [ ! -d "$work/opencv" ]; then
  git clone --depth 1 --filter=blob:none --sparse https://github.com/opencv/opencv "$work/opencv"
  git -C "$work/opencv" fetch --depth 1 origin 41ef839c7d03231dc40c026d28e1ba80494f506d
  git -C "$work/opencv" checkout -q 41ef839c7d03231dc40c026d28e1ba80494f506d
  git -C "$work/opencv" sparse-checkout set samples/data
fi
if [ ! -f "$work/azure.txt" ]; then
  git clone --depth 1 --filter=blob:none --no-checkout https://github.com/Azure/AzurePublicDataset "$work/azure"
  git -C "$work/azure" checkout HEAD -- data/AzureFunctionsInvocationTraceForTwoWeeksJan2021.rar
  (cd "$work" && unrar x -o+ azure/data/AzureFunctionsInvocationTraceForTwoWeeksJan2021.rar >/dev/null)
  mv "$work/AzureFunctionsInvocationTraceForTwoWeeksJan2021.txt" "$work/azure.txt"
fi
echo "d56368ef194baa8d418304bd2f87cca67668ced0d117bd89ad4ef3cf836457d2  $work/azure.txt" | sha256sum -c -
[ -d "$work/corpus" ] || "$work/venv/bin/python" benchmarks/collect/prepare_corpus.py \
  "$work/opencv/samples/data" "$(python3 -c 'import sysconfig;print(sysconfig.get_paths()["stdlib"])')" "$work/azure.txt" "$work/corpus"
benchmarks/collect/run_collection.sh "$work/venv/bin/python" "$work/corpus" "$work/results" "${minutes[@]}"
"$work/venv/bin/python" -m analysis.feature_signal benchmarks/configs/feature-signal-v1.json "$work"/results/*.csv \
  --out "$work/results/analysis.json"
tar czf "$work/feature-signal-$(hostname)-$(date -u +%Y%m%dT%H%MZ).tar.gz" -C "$work" results corpus/manifest.json
echo "Done. Copy $work/feature-signal-*.tar.gz off the VM, then delete the VM."
