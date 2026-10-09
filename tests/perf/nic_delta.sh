#!/bin/bash
# =============================================================================
# 网卡收发包速率（任意 Linux 主机，读 /proc/net/dev，不需要 root）
#
#   ./nic_delta.sh                # 现在开始，Ctrl+C 结束，输出这段时间的 pps / bps
#   ./nic_delta.sh 60             # 统计 60 秒
#   IFACE=ens33 ./nic_delta.sh    # 指定网卡（默认取默认路由的网卡）
#
# DR 压测时在 RS 上运行：回包不经过 LB，LB 上的 counters 只有入向，
# RS 出向（tx）的 pps 就是回程的包速率（docs/压测方案.md 第十节）。
# =============================================================================
set -euo pipefail

IFACE=${IFACE:-$(ip route show default | awk '{for (i = 1; i < NF; i++) if ($i == "dev") {print $(i + 1); exit}}')}
[ -n "$IFACE" ] || { echo "cannot find the default interface, set IFACE"; exit 1; }

snap() { # -> "ns rx_bytes rx_pkts tx_bytes tx_pkts"
  local line
  line=$(grep -E "^ *$IFACE:" /proc/net/dev) || { echo "no such interface $IFACE" >&2; exit 1; }
  line=${line#*:}
  # /proc/net/dev: rx bytes packets errs drop fifo frame compressed multicast | tx bytes packets ...
  echo "$(date +%s%N) $(echo "$line" | awk '{print $1, $2, $9, $10}')"
}

report() {
  read -r t0 rb0 rp0 tb0 tp0 <<< "$1"
  read -r t1 rb1 rp1 tb1 tp1 <<< "$2"
  awk -v dt=$(( t1 - t0 )) -v rb=$(( rb1 - rb0 )) -v rp=$(( rp1 - rp0 )) \
      -v tb=$(( tb1 - tb0 )) -v tp=$(( tp1 - tp0 )) -v ifc="$IFACE" 'BEGIN {
    s = dt / 1e9
    printf "interface %s, interval %.2f s\n", ifc, s
    printf "%-4s %14s %12s %14s\n", "", "packets", "pps", "bps"
    printf "%-4s %14d %12.0f %11.1f Mbps\n", "rx", rp, rp / s, rb * 8 / s / 1e6
    printf "%-4s %14d %12.0f %11.1f Mbps\n", "tx", tp, tp / s, tb * 8 / s / 1e6
  }'
}

A=$(snap)
if [ -n "${1:-}" ]; then
  sleep "$1"
else
  echo "counting on $IFACE... press Ctrl+C to stop" >&2
  trap 'report "$A" "$(snap)"; exit 0' INT
  while true; do sleep 1; done
fi
report "$A" "$(snap)"
