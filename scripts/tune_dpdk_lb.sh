#!/usr/bin/env bash
set -euo pipefail

# Tune the DPDK load-balancer host.
#
# Usage:
#   sudo DPDK_IFACE=ens33 SYS_IFACE=ens32 CPU_MASK_SYSTEM=1 HUGE_2M=1024 ./scripts/tune_dpdk_lb.sh
#
# Important:
# - RPS/RFS and TCP backlog sysctls do not tune packets received by a DPDK-bound
#   data-plane NIC. DPDK polls RX/TX queues directly.
# - Keep Linux interrupts away from the lcores used by the DPDK app.

DPDK_IFACE="${DPDK_IFACE:-ens33}"
SYS_IFACE="${SYS_IFACE:-}"
CPU_MASK_SYSTEM="${CPU_MASK_SYSTEM:-1}"
HUGE_2M="${HUGE_2M:-1024}"
NOFILE="${NOFILE:-1000000}"
DISABLE_IRQBALANCE="${DISABLE_IRQBALANCE:-1}"

require_root() {
  if [[ "${EUID}" -ne 0 ]]; then
    echo "Please run as root: sudo DPDK_IFACE=${DPDK_IFACE} $0" >&2
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

echo "== DPDK LB host tuning, data iface=${DPDK_IFACE} =="

set_sysctl fs.nr_open 2000000
set_sysctl fs.file-max 2000000

# Useful only for control-plane sockets/logging; DPDK data plane bypasses TCP.
set_sysctl net.core.rmem_max 134217728
set_sysctl net.core.wmem_max 134217728
set_sysctl net.core.netdev_max_backlog 250000

# Hugepages for DPDK mbuf/hash allocations.
if [[ -d /sys/kernel/mm/hugepages/hugepages-2048kB ]]; then
  echo "${HUGE_2M}" > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
fi
mkdir -p /mnt/huge
if ! mountpoint -q /mnt/huge; then
  mount -t hugetlbfs nodev /mnt/huge || true
fi

# Keep CPUs deterministic.
if command -v cpupower >/dev/null 2>&1; then
  cpupower frequency-set -g performance || true
else
  for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [[ -e "${gov}" ]] || continue
    echo performance > "${gov}" || true
  done
fi

swapoff -a || true

if [[ "${DISABLE_IRQBALANCE}" == "1" ]] && command -v systemctl >/dev/null 2>&1; then
  systemctl stop irqbalance 2>/dev/null || true
fi

# Move non-DPDK NIC interrupts to housekeeping CPUs. Do not expect this to tune
# a NIC after it is bound to vfio-pci/igb_uio because it no longer uses kernel RX.
if [[ -n "${SYS_IFACE}" ]]; then
  for irq in $(grep -i "${SYS_IFACE}" /proc/interrupts | awk -F: '{print $1}' | tr -d ' '); do
    write_if_exists "/proc/irq/${irq}/smp_affinity" "${CPU_MASK_SYSTEM}"
  done
fi

if [[ -e "/sys/class/net/${DPDK_IFACE}/device/numa_node" ]]; then
  echo "DPDK iface NUMA node: $(cat /sys/class/net/${DPDK_IFACE}/device/numa_node)"
fi

ulimit -n "${NOFILE}" || true

cat <<EOF
done

Checklist before running l4lb:
  1. Bind ${DPDK_IFACE} to your DPDK driver.
  2. Start l4lb with lcores that do not overlap busy Linux IRQ CPUs.
  3. Use one RX/TX queue per worker where possible, and confirm RSS is enabled.
  4. Keep --log info/error for benchmark runs; avoid debug logging.
EOF
