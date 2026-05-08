#!/usr/bin/env bash
set -euo pipefail

# Tune a Linux real-server host running RS_Src/epoll_server/simple_server_mt.
#
# NAT mode:
#   sudo IFACE=ens33 CPU_MASK=f MODE=nat ./scripts/tune_real_server.sh
#
# DR mode requires the VIP on loopback and ARP suppression:
#   sudo IFACE=ens33 CPU_MASK=f MODE=dr VIP=192.168.72.160 ./scripts/tune_real_server.sh

IFACE="${IFACE:-ens33}"
CPU_MASK="${CPU_MASK:-f}"
MODE="${MODE:-nat}"
VIP="${VIP:-}"
NOFILE="${NOFILE:-1000000}"
RPS_FLOW_ENTRIES="${RPS_FLOW_ENTRIES:-32768}"
RPS_FLOW_CNT="${RPS_FLOW_CNT:-32768}"

require_root() {
  if [[ "${EUID}" -ne 0 ]]; then
    echo "Please run as root: sudo IFACE=${IFACE} MODE=${MODE} $0" >&2
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

echo "== real server tuning on ${IFACE}, mode=${MODE} =="

set_sysctl fs.nr_open 2000000
set_sysctl fs.file-max 2000000

# Server listen/accept pressure.
set_sysctl net.core.somaxconn 65535
set_sysctl net.ipv4.tcp_max_syn_backlog 65535
set_sysctl net.core.netdev_max_backlog 250000
set_sysctl net.ipv4.tcp_synack_retries 3
set_sysctl net.ipv4.tcp_syncookies 1

# Connection churn and keep-alive test stability.
set_sysctl net.ipv4.ip_local_port_range "1024 65535"
set_sysctl net.ipv4.tcp_tw_reuse 1
set_sysctl net.ipv4.tcp_fin_timeout 15
set_sysctl net.ipv4.tcp_max_tw_buckets 2000000
set_sysctl net.ipv4.tcp_timestamps 1

# Buffer ceilings.
set_sysctl net.core.rmem_max 134217728
set_sysctl net.core.wmem_max 134217728
set_sysctl net.core.rmem_default 262144
set_sysctl net.core.wmem_default 262144
set_sysctl net.ipv4.tcp_rmem "4096 87380 134217728"
set_sysctl net.ipv4.tcp_wmem "4096 65536 134217728"

# Spread kernel RX softirq for ordinary NIC receive path.
set_sysctl net.core.rps_sock_flow_entries "${RPS_FLOW_ENTRIES}"
for q in /sys/class/net/"${IFACE}"/queues/rx-*; do
  [[ -d "${q}" ]] || continue
  write_if_exists "${q}/rps_cpus" "${CPU_MASK}"
  write_if_exists "${q}/rps_flow_cnt" "${RPS_FLOW_CNT}"
done

if [[ "${MODE}" == "dr" ]]; then
  if [[ -z "${VIP}" ]]; then
    echo "MODE=dr requires VIP, for example: VIP=192.168.72.160" >&2
    exit 1
  fi

  if ! ip addr show lo | grep -q "${VIP}/32"; then
    ip addr add "${VIP}/32" dev lo
  fi

  # Do not let the RS answer ARP for the VIP; only the LB should own it on L2.
  set_sysctl net.ipv4.conf.all.arp_ignore 1
  set_sysctl net.ipv4.conf.all.arp_announce 2
  set_sysctl net.ipv4.conf.default.arp_ignore 1
  set_sysctl net.ipv4.conf.default.arp_announce 2
  set_sysctl net.ipv4.conf.lo.arp_ignore 1
  set_sysctl net.ipv4.conf.lo.arp_announce 2
  set_sysctl "net.ipv4.conf.${IFACE}.arp_ignore" 1
  set_sysctl "net.ipv4.conf.${IFACE}.arp_announce" 2
fi

ulimit -n "${NOFILE}" || true

echo "Current nofile soft limit in this script: $(ulimit -n)"
echo "Run before starting RS too: ulimit -n ${NOFILE}"
echo "done"
