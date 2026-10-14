#!/usr/bin/env bash
# Run the v3 cloud experiments from the client host (benchmarks/cloud/README.md). Run from the repository root, inside
# tmux, with the harbinger-bench image built from this commit on both hosts.
# Usage: BROKER_SSH=ubuntu@<broker private IP> BROKER_HOST=<broker private IP> \
#          benchmarks/cloud/run_all.sh debug|registered RESULTS_ROOT
#   debug       short unregistered runs (seed 7) that exercise every path; about 1 hour
#   registered  E4, E3, E2 then E1 exactly as frozen in their configs; about 15 hours (needs a clean, frozen tree)
#   SSH overrides the ssh command, as for the runners.
#   EXPERIMENTS chooses which to run, in order (default "e4 e3 e2 e1"), so several machine pairs can each run a subset
#   in parallel. E1 alone needs no broker host. A subset writes to <mode>-<commit>-<experiments>.
#   e5 and e6 (v4-cloud-reruns) re-run the frozen aging and size-bin production-flow experiments on one host, like E1:
#   EXPERIMENTS="e5 e6" benchmarks/cloud/run_all.sh registered ~/results
set -euo pipefail
mode=$1; root=$2
experiments=${EXPERIMENTS:-e4 e3 e2 e1}
for e in $experiments; do case $e in e1|e2|e3|e4|e5|e6) ;; *) echo "unknown experiment $e" >&2; exit 2 ;; esac; done
want() { [[ " $experiments " == *" $1 "* ]]; }
needs_broker=false; for e in e2 e3 e4; do want $e && needs_broker=true; done
if $needs_broker; then : "${BROKER_SSH:?set BROKER_SSH}" "${BROKER_HOST:?set BROKER_HOST}"; fi
[ "$mode" = debug ] || [ "$mode" = registered ] || { echo "mode must be debug or registered" >&2; exit 2; }
if [ "$mode" = registered ] && [ -n "$(git status --porcelain)" ]; then
  echo "working tree is not clean; registered runs start from the frozen commit" >&2; exit 2
fi
ssh_broker() { ${SSH:-ssh} "$BROKER_SSH" "$1"; }
commit=$(git rev-parse --short HEAD)$( [ -z "$(git status --porcelain)" ] || echo "-dirty")
out="$root/$mode-$commit"
[ "$experiments" = "e4 e3 e2 e1" ] || out="$out-$(echo $experiments | tr ' ' '-')"
mkdir -p "$out"
if [ "$mode" = registered ]; then
  for c in v3-cloud-e1 v3-cloud-e2 v3-brokers-prodflow v3-brokers-cost; do
    python3 -c "import json,sys;sys.exit(json.load(open('benchmarks/configs/$c.json'))['frozen_on'] is None)" \
      || { echo "$c is not frozen" >&2; exit 2; }
  done
fi
if [ "$mode" = registered ]; then
  for pair in "e5 v4-cloud-aging" "e6 v4-cloud-sizebins"; do
    set -- $pair
    want "$1" || continue
    python3 -c "import json,sys;sys.exit(json.load(open('benchmarks/configs/$2.json'))['frozen_on'] is None)" \
      || { echo "$2 is not frozen" >&2; exit 2; }
  done
fi
image_id() { docker image inspect harbinger-bench --format '{{.Id}}'; }
{
  echo "commit $(git rev-parse HEAD)"
  echo "mode $mode"
  echo "experiments $experiments"
  echo "client_image $(image_id)"
  $needs_broker && echo "broker_image $(ssh_broker "docker image inspect harbinger-bench --format '{{.Id}}'")"
  echo "started $(date -u +%FT%TZ)"
} > "$out/manifest.txt"
cp "$HOME/host-manifest.txt" "$out/client-host.txt"
if $needs_broker; then
  ssh_broker 'cat "$HOME/host-manifest.txt"' > "$out/broker-host.txt"
  grep -q "$(image_id)" <(sed -n 's/^broker_image //p' "$out/manifest.txt") || { echo "images differ between hosts" >&2; exit 2; }
fi

# A config for this mode: the frozen file itself, or a debug copy with overrides (written under $out/configs).
config() {
  local name=$1; shift
  if [ "$mode" = registered ]; then echo "benchmarks/configs/$name.json"; return; fi
  mkdir -p "$out/configs"
  python3 - "benchmarks/configs/$name.json" "$out/configs/$name.json" "$@" <<'PY'
import json, sys
src, dst, *pairs = sys.argv[1:]
c = json.load(open(src))
for pair in pairs:
    key, value = pair.split("=", 1)
    c[key] = json.loads(value)
c["version"] += "-debug"
json.dump(c, open(dst, "w"), indent=1)
PY
  echo "$out/configs/$name.json"
}
log() { echo "$(date -u +%FT%TZ) $*" | tee -a "$out/progress.log"; }
export BROKER_SSH BROKER_HOST SSH

if want e4; then
  e4=$(config v3-brokers-cost repetitions=1 seconds=10 warmup_s=2)
  log "E4 broker cost: $e4"
  benchmarks/run_brokerbench.sh "$e4" "$out/e4" 2>&1 | tee "$out/e4.log"
  python3 -m benchmarks.brokerbench_report "$e4" "$out/e4" > /dev/null
fi

if want e3; then
  e3=$(config v3-brokers-prodflow seeds='[7]' seconds=60 drain_s=120)
  log "E3 production queues: $e3"
  benchmarks/run_prodflow_split.sh "$e3" "$out/e3" 2>&1 | tee "$out/e3.log"
  python3 -m benchmarks.prodflow_report "$e3" "$out/e3" > /dev/null
fi

if want e2; then
  e2=$(config v3-cloud-e2 seeds='[7]' seconds=600 'arms=["fifo","predictive"]')
  log "E2 long run: $e2"
  benchmarks/run_prodflow_split.sh "$e2" "$out/e2" 2>&1 | tee "$out/e2.log"
  python3 -m benchmarks.prodflow_report "$e2" "$out/e2" > /dev/null
fi

if want e1; then
  e1=$(config v3-cloud-e1 seeds='[7]' seconds=60 'arms=["fifo","predictive"]')
  log "E1 single-host replication: $e1 (broker host idle)"
  seeds=$(python3 -c "import json;print(' '.join(map(str,json.load(open('$e1'))['seeds'])))")
  mkdir -p "$out/e1"
  cp "$e1" "$out/e1/config.json"
  docker run --rm --user "$(id -u):$(id -g)" -v "$out/e1":/results -e PRODFLOW_CONFIG=/results/config.json -w /src \
    harbinger-bench bash benchmarks/run_prodflow.sh harbinger_prodflow /results $seeds 2>&1 | tee "$out/e1.log"
  python3 -m benchmarks.prodflow_report "$e1" "$out/e1" > /dev/null
fi

# One-host production-flow reruns (v4-cloud-reruns): the frozen v3 configs on a c8i.2xlarge alone, embedded broker.
single_host() {  # single_host LABEL CONFIG_NAME SUBDIR [debug overrides...]
  local label=$1 name=$2 sub=$3; shift 3
  local cfg; cfg=$(config "$name" "$@")
  log "$label single-host rerun: $cfg (broker host idle)"
  local seeds; seeds=$(python3 -c "import json;print(' '.join(map(str,json.load(open('$cfg'))['seeds'])))")
  mkdir -p "$out/$sub"
  cp "$cfg" "$out/$sub/config.json"
  docker run --rm --user "$(id -u):$(id -g)" -v "$out/$sub":/results -e PRODFLOW_CONFIG=/results/config.json -w /src \
    harbinger-bench bash benchmarks/run_prodflow.sh harbinger_prodflow /results $seeds 2>&1 | tee "$out/$sub.log"
  python3 -m benchmarks.prodflow_report "$cfg" "$out/$sub" > /dev/null
}
if want e5; then single_host E5 v4-cloud-aging e5 seeds='[7]' seconds=60 'arms=["fifo","predictive_weighted"]'; fi
if want e6; then single_host E6 v4-cloud-sizebins e6 seeds='[7]' seconds=60 'arms=["fifo","predictive_sized"]'; fi

log "done"
echo "finished $(date -u +%FT%TZ)" >> "$out/manifest.txt"
tar czf "$out.tar.gz" -C "$root" "$(basename "$out")"
sha256sum "$out.tar.gz" | tee "$out.tar.gz.sha256"
