#!/usr/bin/env bash
# Production-flow matrix with the broker on its own host (#37, #47; E2/E3 in #46). For every seed, every arm in the
# config runs once, arm order rotated per seed as in run_prodflow.sh, with a fresh broker container per run:
#   Harbinger arms: harbinger_prodflow --role broker on the broker host, --role client here, then merge_split_run.py;
#   rabbitmq_* / redis_* arms: a RabbitMQ or Redis container on the broker host, the client here writes run.json.
# Usage: run_prodflow_split.sh CONFIG.json RESULTS_DIR [SEED...]   (default: the config's seeds)
#   IMAGE          benchmark image (default harbinger-bench; see benchmarks/docker)
#   BROKER_SSH     run broker containers on another host over ssh (e.g. ubuntu@10.0.0.5); default: this host
#   BROKER_HOST    address the clients use for the broker (default 127.0.0.1)
#   BROKER_WORKDIR directory on the broker host for Harbinger's broker.json (default /tmp/harbinger-broker-runs)
#   SSH            ssh command (default ssh)
# Results: RESULTS_DIR/seed-<seed>/<arm>/{messages.tsv,run.json}; analyse with python -m benchmarks.prodflow_report.
set -euo pipefail
config=$1; results=$2; shift 2
image=${IMAGE:-harbinger-bench}
host=${BROKER_HOST:-127.0.0.1}
workdir=${BROKER_WORKDIR:-/tmp/harbinger-broker-runs}
field() { python3 -c "import json;v=json.load(open('$config'))['$1'];print(' '.join(map(str,v)) if isinstance(v,list) else v)"; }
read -r -a arms <<< "$(field arms)"
seconds=$(field seconds); workers=$(field workers); scale=$(field rate_scale); drain=$(field drain_s)
seeds=("$@"); [ ${#seeds[@]} -gt 0 ] || read -r -a seeds <<< "$(field seeds)"
# One shell command on the broker host.
on_broker() { if [ -n "${BROKER_SSH:-}" ]; then ${SSH:-ssh} "$BROKER_SSH" "$1"; else bash -c "$1"; fi; }
stop_broker() { on_broker "docker rm -f hb-broker >/dev/null 2>&1 || true"; }
wait_port() {
  for _ in $(seq 1 120); do (exec 3<>"/dev/tcp/$host/$1") 2>/dev/null && return 0; sleep 1; done
  echo "broker did not open $host:$1" >&2; return 1
}
trap stop_broker EXIT
mkdir -p "$results"
on_broker "docker run --rm $image versions" > "$results/broker-versions.txt"
docker run --rm "$image" versions > "$results/client-versions.txt"
for seed in "${seeds[@]}"; do
  offset=$(( seed % ${#arms[@]} ))
  for i in "${!arms[@]}"; do
    arm=${arms[$(( (i + offset) % ${#arms[@]} ))]}
    out="$results/seed-$seed/$arm"
    [ -e "$out/run.json" ] && { echo "skip $out"; continue; }
    rm -rf "$out"; mkdir -p "$(dirname "$out")"
    echo "$(date -u +%FT%TZ) seed=$seed arm=$arm"
    stop_broker
    case $arm in
      rabbitmq_*) port=5672; on_broker "docker run -d --name hb-broker --network host $image rabbitmq >/dev/null" ;;
      redis_*) port=6379; on_broker "docker run -d --name hb-broker --network host $image redis >/dev/null" ;;
      *) port=50051; remote="$workdir/seed-$seed-$arm-$(date -u +%s)"
         on_broker "mkdir -p $workdir && docker run -d --name hb-broker --network host --user \$(id -u):\$(id -g) \
           -v $workdir:$workdir $image harbinger_prodflow --role broker --arm $arm --listen 0.0.0.0:$port \
           --out $remote >/dev/null" ;;
    esac
    wait_port "$port"
    case $arm in rabbitmq_*) sleep 3 ;; esac  # RabbitMQ accepts connections slightly before it is fully booted
    role=(--role client); case $arm in rabbitmq_*|redis_*) role=() ;; esac
    status=0
    docker run --rm --network host --user "$(id -u):$(id -g)" -v "$(cd "$(dirname "$out")" && pwd)":/out "$image" \
      harbinger_prodflow "${role[@]}" --broker "$host:$port" --arm "$arm" --seed "$seed" --seconds "$seconds" \
      --workers "$workers" --rate-scale "$scale" --drain-s "$drain" --out "/out/$arm" || status=$?
    case $arm in
      rabbitmq_*|redis_*) stop_broker ;;
      *) # SIGTERM makes the broker write broker.json; wait for it to exit before reading the file.
         on_broker "docker stop -t 30 hb-broker >/dev/null && docker wait hb-broker >/dev/null 2>&1; true"
         if on_broker "cat $remote/broker.json" > "$out/broker.json" && [ -s "$out/broker.json" ]; then
           python3 -m benchmarks.merge_split_run "$out" >/dev/null || echo "merge failed for $out"
         else
           echo "no broker.json for seed $seed arm $arm"; rm -f "$out/broker.json"
         fi
         stop_broker ;;
    esac
    [ "$status" -eq 0 ] || echo "arm $arm seed $seed client exited $status"
  done
done
