#!/usr/bin/env bash
# Replay a real trace through the real broker: every arm at every load, repeated (docs/v4-replay-preregistration.md).
# Usage: run_replay.sh debug|registered CONFIG.json RESULTS_DIR PREP_DIR TRACE.csv
#   debug       unregistered runs; ROWS, SECONDS_LIMIT, LOADS, ARMS and REPEATS may shorten or narrow them
#   registered  exactly the frozen config: it must have `frozen_on`, the tree must be clean, the trace must match the
#               config's converted_sha256, and the overrides above are refused
# Layout: RESULTS_DIR/<arm>/<load>/rep-<n>/{messages.tsv,run.json}; analyse with python -m benchmarks.replay_report.
#   A run that does not count (benchmarks/replay_report.py check) is kept as rep-<n>.failed and run once more, as the
#   pre-registration says, so both are reported. The overhead probe, if the config has one, goes to RESULTS_DIR/_probe.
# Environment:
#   IMAGE          benchmark image (default harbinger-bench); REPLAY_BIN runs that binary directly instead (no Docker)
#   BROKER_SSH     run the broker on another host over ssh (e.g. ubuntu@10.0.0.5); unset, with BROKER_HOST unset too,
#                  the broker is embedded in the client process (one host)
#   BROKER_HOST    address the clients use for the broker (default 127.0.0.1 when BROKER_SSH is set)
#   BROKER_WORKDIR directory on the broker host for its inputs and broker.json (default /tmp/harbinger-replay)
#   SSH            ssh command (default ssh)
#   SHARD          i/n: run only the cells whose position in the matrix (repeat, load, arm, then the probe) is congruent to
#                  i modulo n, so n machine pairs can split one registered matrix; copy the result trees together afterwards.
#                  Allowed in registered mode, because every cell still runs exactly as frozen.
# Arm order is rotated by repeat and load so that slow drift does not always land on the same arm. `arms_by_load` in the
# config narrows the arms at some loads; ARMS (a debug override) takes precedence over it.
set -euo pipefail
mode=$1; config=$2; results=$3; prep=$4; trace=$5
[ "$mode" = debug ] || [ "$mode" = registered ] || { echo "mode must be debug or registered" >&2; exit 2; }
image=${IMAGE:-harbinger-bench}
workdir=${BROKER_WORKDIR:-/tmp/harbinger-replay}
split=false; [ -n "${BROKER_SSH:-}" ] && split=true
host=${BROKER_HOST:-127.0.0.1}
field() { python3 -c "import json;v=json.load(open('$config'))$1;print(' '.join(map(str,v)) if isinstance(v,list) else v)"; }
has() { python3 -c "import json,sys;sys.exit('$1' not in json.load(open('$config')))"; }

if [ "$mode" = registered ]; then
  for v in ROWS SECONDS_LIMIT LOADS ARMS REPEATS; do  # SHARD is not an override
    [ -z "${!v:-}" ] || { echo "$v is a debug override; registered runs use the config as frozen" >&2; exit 2; }
  done
  python3 -c "import json,sys;sys.exit(json.load(open('$config'))['frozen_on'] is None)" || { echo "$config is not frozen" >&2; exit 2; }
  [ -z "$(git status --porcelain)" ] || { echo "working tree is not clean; registered runs start from the frozen commit" >&2; exit 2; }
  want=$(field "['trace']['converted_sha256']"); have=$(sha256sum "$trace" | cut -d' ' -f1)
  [ "$want" = "$have" ] || { echo "trace sha256 $have is not the registered $want" >&2; exit 2; }
fi
read -r -a loads <<< "${LOADS:-$(field "['utilizations']")}"
read -r -a arms <<< "${ARMS:-$(field "['arms']")}"
# A config may give some loads fewer arms (`arms_by_load`), so costly cells run only the arms a gate needs.
arms_for() {  # arms_for LOAD -> the arms to run at that load
  if [ -n "${ARMS:-}" ] || ! has arms_by_load; then echo "${arms[*]}"; return; fi
  python3 -c "import json;c=json.load(open('$config'));print(' '.join(c['arms_by_load'].get('$1', c['arms'])))"
}
repeats=${REPEATS:-$(field "['repeats']")}
shard_i=0; shard_n=1
if [ -n "${SHARD:-}" ]; then
  shard_i=${SHARD%/*}; shard_n=${SHARD#*/}
  { [[ $shard_i =~ ^[0-9]+$ ]] && [[ $shard_n =~ ^[1-9][0-9]*$ ]] && [ "$shard_i" -lt "$shard_n" ]; } || { echo "SHARD is i/n with 0 <= i < n" >&2; exit 2; }
fi
extra=(); [ -z "${ROWS:-}" ] || extra+=(--rows "$ROWS"); [ -z "${SECONDS_LIMIT:-}" ] || extra+=(--seconds "$SECONDS_LIMIT")

mkdir -p "$results" "$prep"
results=$(cd "$results" && pwd); prep=$(cd "$prep" && pwd)
config_abs=$(cd "$(dirname "$config")" && pwd)/$(basename "$config"); trace_abs=$(cd "$(dirname "$trace")" && pwd)/$(basename "$trace")
log() { echo "$(date -u +%FT%TZ) $*" | tee -a "$results/progress.log"; }

# Run harbinger_replay here. With Docker, inputs are mounted at /cfg, /trace, /prep and results at /out; the caller
# writes paths in the container's terms through these variables.
if [ -n "${REPLAY_BIN:-}" ]; then
  C_CFG=$config_abs; C_TRACE=$trace_abs; C_PREP=$prep; C_OUT=$results
  here() { "$REPLAY_BIN" "$@"; }
else
  C_CFG=/cfg/$(basename "$config_abs"); C_TRACE=/trace/$(basename "$trace_abs"); C_PREP=/prep; C_OUT=/out
  here() { docker run --rm --network host --user "$(id -u):$(id -g)" -v "$(dirname "$config_abs")":/cfg:ro \
    -v "$(dirname "$trace_abs")":/trace:ro -v "$prep":/prep -v "$results":/out "$image" harbinger_replay "$@"; }
fi

{
  echo "commit $(git rev-parse HEAD)$( [ -z "$(git status --porcelain)" ] || echo ' (dirty tree)')"
  echo "mode $mode"; echo "config $(basename "$config") sha256 $(sha256sum "$config" | cut -d' ' -f1)"
  echo "trace $(basename "$trace") sha256 $(sha256sum "$trace" | cut -d' ' -f1)"
  echo "loads ${loads[*]}"; echo "arms ${arms[*]}"; has arms_by_load && echo "arms_by_load $(python3 -c "import json;print(json.dumps(json.load(open('$config'))['arms_by_load']))")"; echo "repeats $repeats"; echo "split $split"; echo "shard $shard_i/$shard_n"
  [ -z "${REPLAY_BIN:-}" ] && echo "image $(docker image inspect "$image" --format '{{.Id}}')"
  echo "started $(date -u +%FT%TZ)"
} > "$results/manifest.txt"
[ ! -f "$HOME/host-manifest.txt" ] || cp "$HOME/host-manifest.txt" "$results/client-host.txt"

[ -f "$prep/plan.json" ] || { log "prepare: warm snapshots and plan -> $prep"; here prepare "$C_TRACE" "$C_CFG" "$C_PREP"; }

# ── The broker side ─────────────────────────────────────────────────────────
on_broker() { if $split; then ${SSH:-ssh} "$BROKER_SSH" "$1"; else bash -c "$1"; fi; }
binput=$workdir/inputs
if $split; then
  # The broker host needs the config and the prepared plan and snapshots; never the trace.
  on_broker "rm -rf $binput && mkdir -p $binput"
  tar -C "$prep" -c . | on_broker "tar -x -C $binput"
  cat "$config_abs" | on_broker "cat > $binput/config.json"
  on_broker "cat $binput/plan.json" | cmp -s - "$prep/plan.json" || { echo "plan.json differs on the broker host" >&2; exit 2; }
fi
if $split && [ -z "${REPLAY_BIN:-}" ]; then
  # Both hosts must run the same image, or the comparison is between two builds.
  broker_image=$(on_broker "docker image inspect $image --format '{{.Id}}'")
  [ "$broker_image" = "$(docker image inspect "$image" --format '{{.Id}}')" ] || { echo "the broker host's image differs from this host's" >&2; exit 2; }
  echo "broker_image $broker_image" >> "$results/manifest.txt"
  on_broker 'cat "$HOME/host-manifest.txt"' > "$results/broker-host.txt" 2>/dev/null || true
fi
b_run() {  # the broker command, as seen by the broker host
  if [ -n "${REPLAY_BIN:-}" ]; then echo "$REPLAY_BIN run --role broker"; else
    echo "docker run -d --name hb-replay-broker --network host --user \$(id -u):\$(id -g) -v $binput:/in:ro -v $workdir:$workdir $image harbinger_replay run --role broker"; fi
}
stop_broker() {
  if [ -n "${REPLAY_BIN:-}" ]; then on_broker "[ -f $workdir/broker.pid ] && kill -TERM \$(cat $workdir/broker.pid) 2>/dev/null; rm -f $workdir/broker.pid; true"
  else on_broker "docker rm -f hb-replay-broker >/dev/null 2>&1 || true"; fi
}
wait_port() {
  for _ in $(seq 1 120); do (exec 3<>"/dev/tcp/$host/$1") 2>/dev/null && return 0; sleep 1; done
  echo "broker did not open $host:$1" >&2; return 1
}
trap stop_broker EXIT

# One attempt at one cell. Writes to $out; returns the client's exit status.
attempt() {  # attempt ARM LOAD OUT SUBDIR
  local arm=$1 load=$2 out=$3 sub=$4 status=0
  if ! $split; then
    here run --arm "$arm" --load "$load" --trace "$C_TRACE" --config "$C_CFG" --prep "$C_PREP" --out "$C_OUT/$sub" "${extra[@]}" || status=$?
    return $status
  fi
  local remote="$workdir/run-$(date -u +%s)" port=50051
  stop_broker
  if [ -n "${REPLAY_BIN:-}" ]; then
    on_broker "mkdir -p $workdir; nohup $REPLAY_BIN run --role broker --arm '$arm' --load $load --config $binput/config.json --prep $binput --out $remote --listen 0.0.0.0:$port > $workdir/broker.log 2>&1 & echo \$! > $workdir/broker.pid"
  else
    on_broker "mkdir -p $workdir && $(b_run) --arm '$arm' --load $load --config /in/config.json --prep /in --out $remote --listen 0.0.0.0:$port >/dev/null"
  fi
  wait_port $port
  here run --role client --broker "$host:$port" --arm "$arm" --load "$load" --trace "$C_TRACE" --config "$C_CFG" --prep "$C_PREP" \
    --out "$C_OUT/$sub" "${extra[@]}" || status=$?
  # SIGTERM makes the broker write broker.json; wait for it before reading.
  if [ -n "${REPLAY_BIN:-}" ]; then
    on_broker "kill -TERM \$(cat $workdir/broker.pid); for _ in \$(seq 1 60); do [ -s $remote/broker.json ] && break; sleep 1; done"
  else
    on_broker "docker stop -t 30 hb-replay-broker >/dev/null && docker wait hb-replay-broker >/dev/null 2>&1; true"
  fi
  if on_broker "cat $remote/broker.json" > "$out/broker.json" && [ -s "$out/broker.json" ]; then
    python3 -m benchmarks.merge_split_run "$out" >/dev/null || echo "merge failed for $out"
  else
    echo "no broker.json for $arm at $load"; rm -f "$out/broker.json"
  fi
  stop_broker
  return $status
}

# One cell: run, check it counts, and if not keep it as .failed and run once more.
cell() {  # cell ARM LOAD NAME [SUBTREE]
  local arm=$1 load=$2 name=$3 tree=${4:-}
  local sub=$tree$arm/$load/$name out="$results/$tree$arm/$load/$name"
  [ -e "$out/run.json" ] && { echo "skip $out"; return; }
  rm -rf "$out"; mkdir -p "$(dirname "$out")"
  log "arm=$arm load=$load $name"
  for try in 1 2; do
    status=0; attempt "$arm" "$load" "$out" "$sub" || status=$?
    [ "$status" -eq 0 ] || log "arm=$arm load=$load $name exited $status"
    if [ -f "$out/run.json" ] && python3 -m benchmarks.replay_report check "$out"; then return; fi
    [ "$try" -eq 1 ] || { log "arm=$arm load=$load $name still does not count; both attempts are kept"; return; }
    log "arm=$arm load=$load $name does not count; keeping it as $name.failed and running once more"
    rm -rf "$out.failed"; [ ! -e "$out" ] || mv "$out" "$out.failed"
  done
}

position=0
mine() { local r=$(( position % shard_n == shard_i )); position=$(( position + 1 )); [ "$r" -eq 1 ]; }
for rep in $(seq 1 "$repeats"); do
  for li in "${!loads[@]}"; do
    offset=$(( rep + li ))
    read -r -a here_arms <<< "$(arms_for "${loads[$li]}")"
    for i in "${!here_arms[@]}"; do
      arm=${here_arms[$(( (i + offset) % ${#here_arms[@]} ))]}
      if mine; then cell "$arm" "${loads[$li]}" "rep-$rep"; fi
    done
  done
done

if has probe && [ -z "${LOADS:-}${ARMS:-}" ]; then
  probe_load=$(field "['probe']['load']"); extra=(--seconds "$(field "['probe']['seconds']")")
  if mine; then
    log "overhead probe: fifo at load $probe_load for $(field "['probe']['seconds']") s"
    cell fifo "$probe_load" rep-1 _probe/
  fi
fi
log "done"
echo "finished $(date -u +%FT%TZ)" >> "$results/manifest.txt"
