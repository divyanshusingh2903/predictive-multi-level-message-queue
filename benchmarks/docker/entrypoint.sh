#!/usr/bin/env bash
# rabbitmq | redis | versions | any other command (run as given).
set -euo pipefail
case "${1:-}" in
  rabbitmq)
    # Fresh node per container; messages are transient and queues non-durable, so nothing persists between runs.
    exec rabbitmq-server ;;
  redis)
    exec redis-server --bind 0.0.0.0 --protected-mode no --save "" --appendonly no ;;
  versions)
    dpkg-query -W -f '${Package} ${Version}\n' rabbitmq-server redis-server librabbitmq4 libhiredis1.1.0 \
      libgrpc++1.51t64 erlang-base 2>/dev/null || true ;;
  *) exec "$@" ;;
esac
