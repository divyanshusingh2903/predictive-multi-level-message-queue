#!/usr/bin/env bash
# Broker-cost matrix (#37; E4 in #46): every kind x rate x repetition, a fresh broker container per run, kind order
# rotated per repetition. Usage: run_brokerbench.sh CONFIG.json RESULTS_DIR
#   IMAGE        benchmark image (default harbinger-bench; see benchmarks/docker)
#   BROKER_SSH   run the broker containers on another host over ssh (e.g. ubuntu@10.0.0.5); default: this host
#   BROKER_HOST  address the clients use for the broker (default 127.0.0.1)
#   SSH          ssh command (default ssh)
# Results: RESULTS_DIR/rep-<r>/<kind>/rate-<rate>.json; analyse with python -m benchmarks.brokerbench_report.
set -euo pipefail
config=$1; results=$2
image=${IMAGE:-harbinger-bench}
host=${BROKER_HOST:-127.0.0.1}
field() { python3 -c "import json,sys;v=json.load(open('$config'))['$1'];print(' '.join(map(str,v)) if isinstance(v,list) else v)"; }
read -r -a kinds <<< "$(field kinds)"; read -r -a rates <<< "$(field rates)"
reps=$(field repetitions); seconds=$(field seconds); warmup=$(field warmup_s); drain=$(field drain_s)
producers=$(field producers); consumers=$(field consumers); payload=$(field payload_bytes)
broker() { if [ -n "${BROKER_SSH:-}" ]; then ${SSH:-ssh} "$BROKER_SSH" "$*"; else "$@"; fi; }
port_of() { case $1 in harbinger) echo 50051;; rabbitmq) echo 5672;; redis) echo 6379;; esac; }
start_broker() {
  local kind=$1 port; port=$(port_of "$kind")
  case $kind in
    harbinger) broker docker run -d --rm --name hb-broker --network host "$image" harbinger_server >/dev/null ;;
    *) broker docker run -d --rm --name hb-broker --network host "$image" "$kind" >/dev/null ;;
  esac
  for _ in $(seq 1 120); do (exec 3<>"/dev/tcp/$host/$port") 2>/dev/null && return 0; sleep 1; done
  echo "broker $kind did not open $host:$port" >&2; return 1
}
stop_broker() { broker docker rm -f hb-broker >/dev/null 2>&1 || true; }  # waits for removal, unlike stop + --rm
trap stop_broker EXIT
mkdir -p "$results"
broker docker run --rm "$image" versions > "$results/broker-versions.txt"
docker run --rm "$image" versions > "$results/client-versions.txt"
for rep in $(seq 1 "$reps"); do
  for i in "${!kinds[@]}"; do
    kind=${kinds[$(( (i + rep - 1) % ${#kinds[@]} ))]}
    for rate in "${rates[@]}"; do
      out="$results/rep-$rep/$kind/rate-$rate.json"
      [ -e "$out" ] && { echo "skip $out"; continue; }
      mkdir -p "$(dirname "$out")"
      echo "$(date -u +%FT%TZ) rep=$rep kind=$kind rate=$rate"
      stop_broker; start_broker "$kind"
      sleep 2  # RabbitMQ accepts connections slightly before it is fully booted
      docker run --rm --network host --user "$(id -u):$(id -g)" -v "$(cd "$(dirname "$out")" && pwd)":/out "$image" \
        harbinger_brokerbench --kind "$kind" --broker "$host:$(port_of "$kind")" --rate "$rate" --seconds "$seconds" \
        --warmup-s "$warmup" --drain-s "$drain" --producers "$producers" --consumers "$consumers" \
        --payload-bytes "$payload" --out "/out/rate-$rate.json" || echo "run rep=$rep kind=$kind rate=$rate exited $?"
      stop_broker
    done
  done
done
