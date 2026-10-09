#!/bin/bash
# =============================================================================
# 压测前的系统调优（docs/压测方案.md）：在 client、LB、RS 三台虚拟机上各运行一次
#
#   sudo ./tune.sh client apply     # 压测客户端（wrk）
#   sudo ./tune.sh rs apply         # 后端 nginx
#   sudo ./tune.sh lb apply         # l4lb 所在机器
#   sudo ./tune.sh <角色> show      # 只查看当前值，不修改
#   sudo ./tune.sh <角色> revert    # 恢复 apply 之前的值
#
# - sysctl 只改运行时的值，重启后自动恢复；修改前的值保存在 $STATE_DIR，revert 用
# - 文件句柄上限写入 /etc/security/limits.d/90-l4lb-bench.conf，对之后新登录的
#   shell 生效；当前 shell 需要手动执行脚本最后提示的 ulimit 命令
# - RS 上如果 firewalld / ufw 在运行，放行 RS_PORTS（默认 80 8080）
#
# 可调参数：RS_PORTS="80 8080"  NOFILE=1048576
# =============================================================================
set -euo pipefail

ROLE=${1:-}
ACTION=${2:-apply}
RS_PORTS=${RS_PORTS:-"80 8080"}
NOFILE=${NOFILE:-1048576}
STATE_DIR=/var/tmp/l4lb-tune
LIMITS_FILE=/etc/security/limits.d/90-l4lb-bench.conf

log() { echo "[tune] $*"; }
die() { echo "[tune] error: $*" >&2; exit 1; }

usage() {
  sed -n '2,17p' "$0"
  exit 1
}

# ---- 各角色的 sysctl ----------------------------------------------------------
# 所有角色：句柄总数、网卡收包队列
COMMON_SYSCTL=(
  "fs.file-max=2097152"
  "fs.nr_open=2097152"
  "net.core.netdev_max_backlog=65535"
  "net.core.somaxconn=65535"
)

# 客户端：wrk 主动发起大量连接，本地端口和 TIME_WAIT 是主要限制
CLIENT_SYSCTL=(
  "net.ipv4.ip_local_port_range=1024 65000"
  "net.ipv4.tcp_tw_reuse=1"             # 主动关闭方可以复用 TIME_WAIT 端口
  "net.ipv4.tcp_fin_timeout=15"
  "net.ipv4.tcp_max_tw_buckets=2000000"
  "net.ipv4.tcp_max_syn_backlog=65535"
)

# RS：nginx 被动接受连接，短连接时 TIME_WAIT 在 nginx 这一侧（服务端先关闭）
RS_SYSCTL=(
  "net.ipv4.tcp_max_syn_backlog=65535"
  "net.ipv4.tcp_max_tw_buckets=2000000"
  "net.ipv4.tcp_fin_timeout=15"
  "net.ipv4.ip_local_port_range=1024 65000"
)

# conntrack（firewalld 等加载了 nf_conntrack 时）：短连接压测会把默认的表打满，
# 内核开始丢 SYN，dmesg 里出现 "nf_conntrack: table full"
CONNTRACK_SYSCTL=(
  "net.netfilter.nf_conntrack_max=1048576"
  "net.netfilter.nf_conntrack_tcp_timeout_time_wait=30"
)

sysctl_list() {
  local -n out=$1
  out=("${COMMON_SYSCTL[@]}")
  case "$ROLE" in
  client) out+=("${CLIENT_SYSCTL[@]}") ;;
  rs)     out+=("${RS_SYSCTL[@]}") ;;
  lb)     ;; # 数据面走 DPDK，不经过内核协议栈；ens33 只承载 SSH 和控制面
  esac
  if [ "$ROLE" != lb ] && [ -e /proc/sys/net/netfilter/nf_conntrack_max ]; then
    out+=("${CONNTRACK_SYSCTL[@]}")
  fi
}

key_of() { echo "${1%%=*}"; }
val_of() { echo "${1#*=}"; }

# ---- apply / revert / show ---------------------------------------------------
apply_sysctl() {
  local items item key val
  sysctl_list items
  mkdir -p "$STATE_DIR"
  local backup="$STATE_DIR/sysctl.$ROLE"
  # 只在第一次 apply 时备份，重复 apply 不会把调优后的值当成原始值
  local first=0
  [ -f "$backup" ] || first=1
  for item in "${items[@]}"; do
    key=$(key_of "$item")
    val=$(val_of "$item")
    [ -e "/proc/sys/${key//./\/}" ] || { log "skip $key (not available)"; continue; }
    if [ "$first" = 1 ]; then
      printf '%s=%s\n' "$key" "$(sysctl -n "$key" | tr '\t' ' ')" >> "$backup"
    fi
    sysctl -q -w "$key=$val"
    log "sysctl $key = $val"
  done
}

revert_sysctl() {
  local backup="$STATE_DIR/sysctl.$ROLE" line
  [ -f "$backup" ] || { log "no sysctl backup for $ROLE, nothing to revert"; return; }
  while IFS= read -r line; do
    sysctl -q -w "$line" && log "restored $line"
  done < "$backup"
  rm -f "$backup"
}

apply_limits() {
  # nofile 不能超过 fs.nr_open，否则新登录（包括 ssh）会因为 setrlimit 失败被拒绝；
  # 重启后 nr_open 回到默认值（通常 1048576），所以这里用当前和默认值里较小的那个比较
  local nr_open
  nr_open=$(sysctl -n fs.nr_open)
  [ "$nr_open" -gt 1048576 ] && nr_open=1048576
  [ "$NOFILE" -le "$nr_open" ] || die "NOFILE=$NOFILE > fs.nr_open default $nr_open, would lock out logins"
  cat > "$LIMITS_FILE" <<EOF
# l4lb 压测（tests/perf/tune.sh），revert 时删除
*    soft nofile $NOFILE
*    hard nofile $NOFILE
root soft nofile $NOFILE
root hard nofile $NOFILE
EOF
  log "nofile limit $NOFILE written to $LIMITS_FILE (new logins)"
}

revert_limits() {
  [ -f "$LIMITS_FILE" ] && rm -f "$LIMITS_FILE" && log "removed $LIMITS_FILE"
  return 0
}

# RS 的防火墙：只放行端口，记录下来，revert 时撤销
apply_firewall() {
  [ "$ROLE" = rs ] || return 0
  local p rec="$STATE_DIR/firewall.$ROLE"
  : > "$rec"
  if command -v firewall-cmd >/dev/null && systemctl is-active -q firewalld; then
    for p in $RS_PORTS; do
      if ! firewall-cmd -q --query-port="$p/tcp"; then
        firewall-cmd -q --add-port="$p/tcp"   # 运行时规则，reload/重启后失效
        echo "firewalld $p" >> "$rec"
      fi
      log "firewalld: port $p/tcp open"
    done
  elif command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
    for p in $RS_PORTS; do
      if ! ufw status | grep -qE "^$p/tcp +ALLOW"; then
        ufw allow "$p/tcp" >/dev/null
        echo "ufw $p" >> "$rec"
      fi
      log "ufw: port $p/tcp open"
    done
  else
    log "no active firewall (firewalld/ufw), nothing to open"
  fi
}

revert_firewall() {
  local rec="$STATE_DIR/firewall.$ROLE" kind p
  [ -f "$rec" ] || return 0
  while read -r kind p; do
    case "$kind" in
    firewalld) firewall-cmd -q --remove-port="$p/tcp" && log "firewalld: closed $p/tcp" ;;
    ufw)       ufw delete allow "$p/tcp" >/dev/null && log "ufw: closed $p/tcp" ;;
    esac
  done < "$rec"
  rm -f "$rec"
}

show() {
  local items item key
  sysctl_list items
  log "role $ROLE, $(nproc) vCPU, $(uname -r)"
  for item in "${items[@]}"; do
    key=$(key_of "$item")
    [ -e "/proc/sys/${key//./\/}" ] || continue
    printf '[tune]   %-48s %-18s (target %s)\n' "$key" \
      "$(sysctl -n "$key" | tr '\t' ' ')" "$(val_of "$item")"
  done
  log "nofile: this shell soft $(ulimit -Sn) hard $(ulimit -Hn)"
  if [ -e /proc/sys/net/netfilter/nf_conntrack_count ]; then
    log "conntrack: $(cat /proc/sys/net/netfilter/nf_conntrack_count) /" \
        "$(cat /proc/sys/net/netfilter/nf_conntrack_max) entries"
  fi
  case "$ROLE" in
  lb) show_lb ;;
  rs) show_rs ;;
  esac
}

# LB：检查 DPDK 运行环境，不修改
show_lb() {
  local free total
  total=$(awk '/HugePages_Total/ {print $2}' /proc/meminfo)
  free=$(awk '/HugePages_Free/ {print $2}' /proc/meminfo)
  log "hugepages: $free free / $total total"
  [ "${total:-0}" -gt 0 ] || log "WARNING: no hugepages, run: DPDK_NIC=ens160 scripts/setup_dpdk_env.sh up"
  if command -v dpdk-devbind.py >/dev/null; then
    dpdk-devbind.py -s | sed -n '/DPDK-compatible/,/^$/p' | sed 's/^/[tune]   /'
  else
    local dev
    for dev in /sys/bus/pci/drivers/{vfio-pci,uio_pci_generic,igb_uio}/0000:*; do
      [ -e "$dev" ] && log "DPDK NIC: $(basename "$dev") ($(basename "$(dirname "$dev")"))"
    done
  fi
  if pgrep -x l4lb >/dev/null; then log "l4lb is running (pid $(pgrep -x l4lb))"; fi
  return 0
}

show_rs() {
  local p
  for p in $RS_PORTS; do
    if command -v ss >/dev/null && ss -ltn "sport = :$p" | grep -q LISTEN; then
      log "port $p: listening"
    else
      log "WARNING: nothing listening on port $p"
    fi
  done
}

# ---- main --------------------------------------------------------------------
case "$ROLE" in client|rs|lb) ;; *) usage ;; esac
[ "$(id -u)" = 0 ] || die "run as root"

case "$ACTION" in
apply)
  apply_sysctl
  apply_limits
  apply_firewall
  echo
  show
  echo
  log "done. In the shell that starts wrk / nginx / l4lb, also run:"
  log "    ulimit -n $NOFILE"
  log "undo with: $0 $ROLE revert (sysctl changes also reset on reboot)"
  ;;
revert)
  revert_sysctl
  revert_limits
  revert_firewall
  ;;
show) show ;;
*) usage ;;
esac
