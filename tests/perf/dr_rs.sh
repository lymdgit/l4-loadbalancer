#!/bin/bash
# =============================================================================
# DR 模式的 RS 配置（在 RS 上运行，docs/压测方案.md 第十节）
#
#   sudo ./dr_rs.sh up       # lo 上绑 VIP，关闭 VIP 的 ARP 响应
#   sudo ./dr_rs.sh down     # 还原（删除 VIP，恢复 arp 参数）
#   sudo ./dr_rs.sh status   # 查看当前状态
#
# DR 模式下 LB 只改目的 MAC，包的目的 IP 仍是 VIP，RS 必须认为 VIP 是本机地址
# 才会接收；同时 RS 不能应答 VIP 的 ARP，否则客户端会绕过 LB 直接发给 RS。
#   arp_ignore=1   只应答目的地址配置在收包网卡上的 ARP 请求（VIP 在 lo 上，不应答）
#   arp_announce=2 发 ARP 请求时只用出口网卡上的地址作为源地址（不暴露 VIP）
#
# 可调参数：VIP=192.168.154.130  IFACE=（默认取默认路由的网卡）
# 修改前的 arp 参数保存在 $STATE，down 时恢复；这些都是运行时设置，重启后失效。
# =============================================================================
set -euo pipefail

VIP=${VIP:-192.168.154.130}
IFACE=${IFACE:-$(ip route show default | awk '{for (i = 1; i < NF; i++) if ($i == "dev") {print $(i + 1); exit}}')}
STATE=/var/tmp/l4lb-dr-rs.state

log() { echo "[dr_rs] $*"; }
die() { echo "[dr_rs] error: $*" >&2; exit 1; }

KEYS=(net.ipv4.conf.all.arp_ignore net.ipv4.conf.all.arp_announce
      net.ipv4.conf.lo.arp_ignore net.ipv4.conf.lo.arp_announce)
[ -n "$IFACE" ] && KEYS+=("net.ipv4.conf.$IFACE.arp_ignore" "net.ipv4.conf.$IFACE.arp_announce")

target() { # key -> 目标值
  case "$1" in
  *arp_ignore) echo 1 ;;
  *arp_announce) echo 2 ;;
  esac
}

has_vip() { ip -4 addr show dev lo | grep -q " $VIP/32 "; }

up() {
  # 先改 arp 参数再加 VIP：顺序反了的话，加 VIP 时 RS 可能先对外应答一次 ARP，
  # 客户端把 VIP 解析成 RS 的 MAC，之后的流量就绕过 LB
  if [ ! -f "$STATE" ]; then
    local k
    for k in "${KEYS[@]}"; do
      printf '%s=%s\n' "$k" "$(sysctl -n "$k")" >> "$STATE"
    done
  fi
  local k
  for k in "${KEYS[@]}"; do
    sysctl -q -w "$k=$(target "$k")"
    log "sysctl $k = $(target "$k")"
  done
  if has_vip; then
    log "VIP $VIP already on lo"
  else
    ip addr add "$VIP/32" dev lo
    log "added VIP $VIP/32 on lo"
  fi
  echo
  status
  echo
  log "on the client, clear any stale ARP entry first:  ip neigh flush $VIP"
  log "undo with: $0 down"
}

down() {
  if has_vip; then
    ip addr del "$VIP/32" dev lo
    log "removed VIP $VIP/32 from lo"
  fi
  if [ -f "$STATE" ]; then
    local line
    while IFS= read -r line; do
      sysctl -q -w "$line" && log "restored $line"
    done < "$STATE"
    rm -f "$STATE"
  else
    log "no saved arp settings, left as is"
  fi
}

status() {
  log "uplink $IFACE, VIP $VIP"
  if has_vip; then log "lo: $VIP/32 present"; else log "lo: $VIP not configured"; fi
  local k ok=1
  for k in "${KEYS[@]}"; do
    local v
    v=$(sysctl -n "$k")
    printf '[dr_rs]   %-40s %s (DR wants %s)\n' "$k" "$v" "$(target "$k")"
  done
  # arp_ignore / arp_announce 对某个网卡取 all 和该网卡里的较大值
  [ "$(sysctl -n net.ipv4.conf.all.arp_ignore)" -ge 1 ] || ok=0
  [ "$(sysctl -n net.ipv4.conf.all.arp_announce)" -ge 2 ] || ok=0
  if has_vip && [ "$ok" = 1 ]; then log "DR ready"; else log "DR NOT ready"; fi
}

[ "$(id -u)" = 0 ] || die "run as root"
case "${1:-}" in
up) up ;;
down) down ;;
status) status ;;
*) sed -n '2,17p' "$0"; exit 1 ;;
esac
