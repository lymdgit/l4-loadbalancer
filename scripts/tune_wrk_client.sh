#!/usr/bin/env bash
set -euo pipefail

# Tune a Linux wrk client host.
#
# Usage:
#   sudo IFACE=ens33 CPU_MASK=f ./scripts/tune_wrk_client.sh
#   ulimit -n 1000000
#   wrk -t8 -c1000 -d30s http://192.168.72.160/
#
# Notes:
# - ulimit is process-local. Run it in the same shell that starts wrk, or
#   launch wrk from this script after adding your command at the bottom.
# - RPS/RFS only applies to kernel networking hosts, not DPDK-bound ports.

IFACE="${IFACE:-ens33}"
CPU_MASK="${CPU_MASK:-f}"
NOFILE="${NOFILE:-1000000}"
RPS_FLOW_ENTRIES="${RPS_FLOW_ENTRIES:-32768}"
RPS_FLOW_CNT="${RPS_FLOW_CNT:-32768}"

require_root() {
  if [[ "${EUID}" -ne 0 ]]; then
    echo "Please run as root: sudo IFACE=${IFACE} $0" >&2
    exit 1
  fi
}

set_sysctl() {
  local key="$1"
  local value="$2"
  if [[ -e "/proc/sys/${key//./\/}" ]]; then
    sysctl -w "${key}=${value}"
  else
    echo "skip missing sysctl: ${key}" >&2
  fi
}

write_if_exists() {
  local path="$1"
  local value="$2"
  if [[ -e "${path}" ]]; then
    echo "${value}" > "${path}"
  else
    echo "skip missing path: ${path}" >&2
  fi
}

require_root

echo "== wrk client tuning on ${IFACE} =="

set_sysctl fs.nr_open 2000000
set_sysctl fs.file-max 2000000

# Client-side connection churn: enough ephemeral ports and safer TIME_WAIT reuse.
set_sysctl net.ipv4.ip_local_port_range "1024 65535"
set_sysctl net.ipv4.tcp_tw_reuse 1
set_sysctl net.ipv4.tcp_fin_timeout 15
set_sysctl net.ipv4.tcp_max_tw_buckets 2000000
set_sysctl net.ipv4.tcp_timestamps 1

# Buffer ceilings for high-throughput responses.
set_sysctl net.core.rmem_max 134217728
set_sysctl net.core.wmem_max 134217728
set_sysctl net.core.rmem_default 262144
set_sysctl net.core.wmem_default 262144
set_sysctl net.ipv4.tcp_rmem "4096 87380 134217728"
set_sysctl net.ipv4.tcp_wmem "4096 65536 134217728"

# RX backlog/RPS helps ordinary Linux NIC receive path when response softirq is skewed.
set_sysctl net.core.netdev_max_backlog 250000
set_sysctl net.core.rps_sock_flow_entries "${RPS_FLOW_ENTRIES}"
for q in /sys/class/net/"${IFACE}"/queues/rx-*; do
  [[ -d "${q}" ]] || continue
  write_if_exists "${q}/rps_cpus" "${CPU_MASK}"
  write_if_exists "${q}/rps_flow_cnt" "${RPS_FLOW_CNT}"
done

ulimit -n "${NOFILE}" || true

echo "Current nofile soft limit in this script: $(ulimit -n)"
echo "Run in your wrk shell too: ulimit -n ${NOFILE}"
echo "done"
