#!/bin/bash
# =============================================================================
# DPDK 运行环境准备：大页内存、用户态驱动、网卡接管
#
# 系统重启后这些设置都会丢失，运行一次即可恢复：
#   sudo scripts/setup_dpdk_env.sh up        # 配置大页 + 加载驱动 + 接管网卡
#   sudo scripts/setup_dpdk_env.sh hugepages # 只配置大页（功能测试只需要这一步）
#   sudo scripts/setup_dpdk_env.sh status    # 查看当前状态
#   sudo scripts/setup_dpdk_env.sh down      # 网卡还给内核驱动
#   sudo scripts/setup_dpdk_env.sh install   # 安装 systemd 服务，开机自动执行 up
#   sudo scripts/setup_dpdk_env.sh uninstall # 卸载 systemd 服务
#
# 参数可以用环境变量覆盖，或写到 /etc/l4lb/dpdk-env.conf（KEY=VALUE 格式）：
#   DPDK_NIC        要接管的网卡：接口名(ens160) 或 PCI 地址(0000:03:00.0)
#   DPDK_DRIVER     用户态驱动，默认 auto：
#                     有 IOMMU -> vfio-pci（有 DMA 隔离，推荐）
#                     无 IOMMU -> uio_pci_generic（虚拟机常见情况）
#                   也可以显式指定 vfio-pci | uio_pci_generic | igb_uio
#   ALLOW_NOIOMMU   设为 1 时，无 IOMMU 也允许使用 vfio-pci（会打开内核的
#                   enable_unsafe_noiommu_mode，关闭 DMA 隔离，仅限测试机）
#   IGB_UIO_KO      DPDK_DRIVER=igb_uio 时 igb_uio.ko 的路径
#   HUGEPAGES       2MB 大页数量（每个 NUMA 节点），默认 1024（= 2GB）
#   HUGE_MOUNT      hugetlbfs 挂载点，默认 /dev/hugepages
#   DPDK_BIN_DIR    dpdk-devbind.py 所在目录
#
# 安全检查：带 IP 地址、承载默认路由或当前 SSH 会话的网卡一律拒绝接管，
# 避免把管理网口交给 DPDK 导致失联。
# =============================================================================
set -euo pipefail

CONF_FILE=${CONF_FILE:-/etc/l4lb/dpdk-env.conf}
# shellcheck disable=SC1090
[ -f "$CONF_FILE" ] && . "$CONF_FILE"

DPDK_NIC=${DPDK_NIC:-ens160}
DPDK_DRIVER=${DPDK_DRIVER:-auto}
ALLOW_NOIOMMU=${ALLOW_NOIOMMU:-0}
IGB_UIO_KO=${IGB_UIO_KO:-}
HUGEPAGES=${HUGEPAGES:-1024}
HUGE_MOUNT=${HUGE_MOUNT:-/dev/hugepages}
DPDK_BIN_DIR=${DPDK_BIN_DIR:-/root/dpvs/dpvs/dpdk-24.11/dpdklib/bin}
STATE_DIR=/run/l4lb
SERVICE=/etc/systemd/system/l4lb-dpdk-env.service

log()  { echo "[dpdk-env] $*"; }
die()  { echo "[dpdk-env] ERROR: $*" >&2; exit 1; }

[ "$(id -u)" -eq 0 ] || die "must run as root"

devbind() {
  local tool="$DPDK_BIN_DIR/dpdk-devbind.py"
  [ -x "$tool" ] || tool=$(command -v dpdk-devbind.py || true)
  [ -n "$tool" ] || die "dpdk-devbind.py not found, set DPDK_BIN_DIR"
  python3 "$tool" "$@"
}

# 接口名或 PCI 地址 -> 完整 PCI 地址
resolve_pci() {
  local nic=$1
  if [[ $nic =~ ^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$ ]]; then
    echo "$nic"
  elif [[ $nic =~ ^[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]$ ]]; then
    echo "0000:$nic"
  elif [ -e "/sys/class/net/$nic/device" ]; then
    basename "$(readlink -f "/sys/class/net/$nic/device")"
  elif [ -f "$STATE_DIR/$nic.pci" ]; then
    cat "$STATE_DIR/$nic.pci"   # 已被接管，内核接口已消失
  else
    die "cannot resolve NIC '$nic' (not an interface or PCI address)"
  fi
}

current_driver() {
  local link="/sys/bus/pci/devices/$1/driver"
  [ -e "$link" ] && basename "$(readlink -f "$link")" || echo "none"
}

# 网卡在内核里的接口名（已被 DPDK 接管则为空）
kernel_ifname() {
  ls "/sys/bus/pci/devices/$1/net" 2>/dev/null | head -1 || true
}

# ---------------------------------------------------------------------------
# 安全检查：不能接管管理网口
# ---------------------------------------------------------------------------
check_safe_to_bind() {
  local ifname=$1
  [ -z "$ifname" ] && return 0
  if ip -4 -o addr show dev "$ifname" | grep -q inet; then
    die "$ifname has an IPv4 address; refusing to bind (flush it first if this is intended)"
  fi
  if ip route show default | grep -qw "dev $ifname"; then
    die "$ifname carries the default route; refusing to bind"
  fi
  if [ -n "${SSH_CONNECTION:-}" ]; then
    local ssh_local
    ssh_local=$(echo "$SSH_CONNECTION" | awk '{print $3}')
    if ip -o addr show dev "$ifname" | grep -qw "$ssh_local"; then
      die "current SSH session runs over $ifname; refusing to bind"
    fi
  fi
}

# ---------------------------------------------------------------------------
# 大页
# ---------------------------------------------------------------------------
setup_hugepages() {
  local nodes
  nodes=$(ls -d /sys/devices/system/node/node[0-9]* 2>/dev/null || true)
  if [ -n "$nodes" ]; then
    for n in $nodes; do
      echo "$HUGEPAGES" > "$n/hugepages/hugepages-2048kB/nr_hugepages"
    done
  else
    echo "$HUGEPAGES" > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
  fi
  local total
  total=$(awk '/HugePages_Total/ {print $2}' /proc/meminfo)
  [ "$total" -ge "$HUGEPAGES" ] || \
    log "WARNING: only $total hugepages allocated (wanted $HUGEPAGES per node); memory may be fragmented"

  if ! mount | grep -q "on $HUGE_MOUNT type hugetlbfs"; then
    mkdir -p "$HUGE_MOUNT"
    mount -t hugetlbfs -o pagesize=2M nodev "$HUGE_MOUNT"
  fi
  log "hugepages: $total x 2MB, mounted at $HUGE_MOUNT"
}

# ---------------------------------------------------------------------------
# 驱动
# ---------------------------------------------------------------------------
has_iommu() { [ -n "$(ls -A /sys/kernel/iommu_groups 2>/dev/null)" ]; }

load_driver() {
  if [ "$DPDK_DRIVER" = auto ]; then
    if has_iommu; then DPDK_DRIVER=vfio-pci; else DPDK_DRIVER=uio_pci_generic; fi
    log "DPDK_DRIVER=auto -> $DPDK_DRIVER"
  fi
  case "$DPDK_DRIVER" in
  vfio-pci)
    if ! has_iommu; then
      [ "$ALLOW_NOIOMMU" = 1 ] || die "no IOMMU: vfio-pci needs ALLOW_NOIOMMU=1 (disables DMA isolation), or use DPDK_DRIVER=uio_pci_generic"
      modprobe vfio enable_unsafe_noiommu_mode=1
      echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
      log "WARNING: vfio no-IOMMU mode enabled (no DMA isolation)"
    fi
    modprobe vfio-pci
    ;;
  uio_pci_generic)
    modprobe uio
    modprobe uio_pci_generic
    ;;
  igb_uio)
    modprobe uio
    lsmod | grep -q '^igb_uio' || {
      [ -f "$IGB_UIO_KO" ] || die "set IGB_UIO_KO to the path of igb_uio.ko"
      insmod "$IGB_UIO_KO"
    }
    ;;
  *) die "unknown DPDK_DRIVER '$DPDK_DRIVER'" ;;
  esac
  log "driver $DPDK_DRIVER loaded"
}

# ---------------------------------------------------------------------------
# 网卡接管
# ---------------------------------------------------------------------------
# 加载驱动之前先做检查：网卡存在、不是管理网口
preflight() {
  local pci
  pci=$(resolve_pci "$DPDK_NIC")
  [ -e "/sys/bus/pci/devices/$pci" ] || die "PCI device $pci not found"
  check_safe_to_bind "$(kernel_ifname "$pci")"
}

bind_nic() {
  local pci ifname drv
  pci=$(resolve_pci "$DPDK_NIC")
  drv=$(current_driver "$pci")
  if [ "$drv" = "$DPDK_DRIVER" ]; then
    log "$pci already bound to $DPDK_DRIVER"
    return
  fi
  ifname=$(kernel_ifname "$pci")
  check_safe_to_bind "$ifname"

  mkdir -p "$STATE_DIR"
  echo "$pci" > "$STATE_DIR/${ifname:-$DPDK_NIC}.pci"
  echo "$drv" > "$STATE_DIR/$pci.kernel_driver"

  if [ -n "$ifname" ]; then
    # 让 NetworkManager 不再管理这张网卡，避免它自动拉起接口
    command -v nmcli >/dev/null && nmcli dev set "$ifname" managed no 2>/dev/null || true
    ip link set "$ifname" down
  fi
  devbind --bind="$DPDK_DRIVER" "$pci"
  log "$pci (${ifname:-n/a}, was $drv) bound to $DPDK_DRIVER"
}

unbind_nic() {
  local pci kdrv
  pci=$(resolve_pci "$DPDK_NIC")
  kdrv=$(cat "$STATE_DIR/$pci.kernel_driver" 2>/dev/null || echo "")
  [ -n "$kdrv" ] && [ "$kdrv" != "none" ] || kdrv=$(lspci -s "$pci" -k 2>/dev/null | awk '/Kernel modules/ {print $3}')
  [ -n "$kdrv" ] || die "cannot determine kernel driver for $pci"
  devbind --bind="$kdrv" "$pci"
  sleep 1
  local ifname
  ifname=$(kernel_ifname "$pci")
  if [ -n "$ifname" ]; then
    command -v nmcli >/dev/null && nmcli dev set "$ifname" managed yes 2>/dev/null || true
    ip link set "$ifname" up || true
  fi
  rm -f "$STATE_DIR/$pci.kernel_driver" "$STATE_DIR"/*.pci
  log "$pci returned to kernel driver $kdrv (${ifname:-n/a})"
}

status() {
  grep -E "HugePages_(Total|Free)|Hugepagesize" /proc/meminfo
  mount | grep hugetlbfs || echo "hugetlbfs not mounted"
  lsmod | grep -E "^(vfio_pci|vfio |uio_pci_generic|igb_uio)" || echo "no DPDK driver loaded"
  echo
  devbind --status-dev net
}

install_service() {
  local self
  self=$(readlink -f "$0")
  mkdir -p /etc/l4lb
  if [ ! -f "$CONF_FILE" ]; then
    cat > "$CONF_FILE" <<EOF
# l4lb DPDK 环境配置（由 setup_dpdk_env.sh 读取）
DPDK_NIC=$DPDK_NIC
DPDK_DRIVER=$DPDK_DRIVER
HUGEPAGES=$HUGEPAGES
DPDK_BIN_DIR=$DPDK_BIN_DIR
EOF
  fi
  cat > "$SERVICE" <<EOF
[Unit]
Description=Prepare DPDK environment for l4lb (hugepages, driver, NIC binding)
After=systemd-modules-load.service
Before=network-pre.target
Wants=network-pre.target

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=$self up
ExecStop=$self down

[Install]
WantedBy=multi-user.target
EOF
  systemctl daemon-reload
  systemctl enable l4lb-dpdk-env.service
  log "installed $SERVICE (config: $CONF_FILE); runs at every boot"
}

uninstall_service() {
  systemctl disable l4lb-dpdk-env.service 2>/dev/null || true
  rm -f "$SERVICE"
  systemctl daemon-reload
  log "removed $SERVICE"
}

case "${1:-}" in
up)        preflight; setup_hugepages; load_driver; bind_nic; status ;;
hugepages) setup_hugepages ;;
down)      unbind_nic ;;
status)    status ;;
install)   install_service ;;
uninstall) uninstall_service ;;
*) sed -n '2,30p' "$0"; exit 1 ;;
esac
