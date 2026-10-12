#!/usr/bin/env bash
# Prepare a fresh Ubuntu 24.04 EC2 instance for the v3 cloud runs (benchmarks/cloud/README.md): a spend cap, Docker,
# git, and a host manifest. Usage on the instance: bash setup_host.sh [HOURS]   (default 20)
set -euo pipefail
hours=${1:-20}
# Spend cap: the instance powers off after HOURS no matter what. Launched with shutdown behaviour "stop", so the disk
# and any results survive; compute billing stops.
sudo shutdown -h "+$((hours * 60))" "harbinger spend cap" || true
sudo apt-get update -qq
sudo apt-get install -y -qq docker.io git python3 >/dev/null
sudo usermod -aG docker "$USER"
token=$(curl -sf -X PUT http://169.254.169.254/latest/api/token -H "X-aws-ec2-metadata-token-ttl-seconds: 300")
md() { curl -sf -H "X-aws-ec2-metadata-token: $token" "http://169.254.169.254/latest/meta-data/$1"; }
{
  echo "instance_id $(md instance-id)"
  echo "instance_type $(md instance-type)"
  echo "availability_zone $(md placement/availability-zone)"
  echo "ami $(md ami-id)"
  echo "private_ip $(md local-ipv4)"
  echo "kernel $(uname -r)"
  echo "cpu $(lscpu | sed -n 's/^Model name: *//p')"
  echo "nproc $(nproc)"
  echo "memory_kb $(awk '/MemTotal/ {print $2}' /proc/meminfo)"
  echo "docker $(docker --version)"
  echo "spend_cap_hours $hours"
} > "$HOME/host-manifest.txt"
cat "$HOME/host-manifest.txt"
echo "Done. Log out and back in so the docker group applies."
