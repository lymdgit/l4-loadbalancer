#!/bin/bash
# =============================================================================
# 单机实验：本机起 RS、起 LB、从本机打流量（拓扑见 tests/perf/lab.conf）
#
#   sudo tests/perf/lab.sh rs-start     # 编译工具、放行防火墙端口、启动两个 RS
#   sudo tests/perf/lab.sh lb           # 前台启动 LB（Ctrl+C 停止）
#   sudo tests/perf/lab.sh curl         # 发几个请求，看分发到哪个 RS
#   sudo tests/perf/lab.sh bench        # 长连接 QPS + 短连接 CPS
#   sudo tests/perf/lab.sh status       # RS 进程、LB 统计
#   sudo tests/perf/lab.sh rs-stop      # 停止 RS、撤销防火墙端口
#
# 可调参数：LB_CORES=1-4  RS_CORES=5-7  CLIENT_CORES=8-11  DURATION=10
# =============================================================================
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
TOOLS=$ROOT/tests/perf/tools
BIN=$ROOT/build/lab
CONF=$ROOT/tests/perf/lab.conf
VIP=192.168.154.130
RS_PORTS=(8081 8082)
RS_NAMES=(a b)
LB_CORES=${LB_CORES:-1-4}
RS_CORES=${RS_CORES:-5-7}
CLIENT_CORES=${CLIENT_CORES:-8-11}
DURATION=${DURATION:-10}
LOGDIR=/tmp/l4lb-lab

log() { echo "[lab] $*"; }

build_tools() {
  mkdir -p "$BIN" "$LOGDIR"
  for t in rs_server loadgen; do
    if [ ! -x "$BIN/$t" ] || [ "$TOOLS/$t.c" -nt "$BIN/$t" ]; then
      gcc -O2 -Wall -pthread -o "$BIN/$t" "$TOOLS/$t.c"
    fi
  done
}

firewall() { # add|remove
  command -v firewall-cmd >/dev/null && systemctl is-active -q firewalld || return 0
  for p in "${RS_PORTS[@]}"; do
    # 只改运行时规则，firewalld reload 或重启后自动恢复
    firewall-cmd -q --"$1"-port="$p"/tcp || true
  done
  log "firewalld runtime: $1 ports ${RS_PORTS[*]}/tcp"
}

rs_start() {
  build_tools
  firewall add
  for i in "${!RS_PORTS[@]}"; do
    local p=${RS_PORTS[$i]} n=${RS_NAMES[$i]}
    if pgrep -f "rs_server $p " >/dev/null; then
      log "rs $n already running on :$p"
      continue
    fi
    nohup taskset -c "$RS_CORES" "$BIN/rs_server" "$p" 2 "$n" \
      > "$LOGDIR/rs_$n.log" 2>&1 &
    log "rs $n started on :$p (log $LOGDIR/rs_$n.log)"
  done
  sleep 0.3
  for p in "${RS_PORTS[@]}"; do
    curl -s -m 2 "http://127.0.0.1:$p/" | sed "s/^/[lab]   :$p -> /"
  done
}

rs_stop() {
  pkill -f "$BIN/rs_server" || true
  firewall remove
  log "rs stopped"
}

lb() {
  [ -x "$ROOT/build/l4lb" ] || { log "build l4lb first (cmake --build build)"; exit 1; }
  "$ROOT/build/l4lb" -- --lb-config "$CONF" --check-config >/dev/null
  log "starting l4lb on lcores $LB_CORES (Ctrl+C to stop)"
  exec "$ROOT/build/l4lb" -l "$LB_CORES" -- --lb-config "$CONF"
}

do_curl() {
  for i in $(seq 1 6); do
    printf "[lab] request %d: " "$i"
    curl -s -m 2 -H "Connection: close" "http://$VIP/" || echo "FAILED"
  done
}

bench() {
  build_tools
  local lg="taskset -c $CLIENT_CORES $BIN/loadgen -h $VIP -p 80 -d $DURATION"
  log "1) keep-alive QPS"
  $lg -t 2 -c 50
  log "2) short connections CPS"
  $lg -t 2 -c 50 -s
  status
}

status() {
  pgrep -af "$BIN/rs_server" | sed 's/^/[lab] /' || log "no rs running"
  if [ -S /run/l4lb.sock ]; then
    python3 "$ROOT/scripts/l4lbctl.py" stats
    python3 "$ROOT/scripts/l4lbctl.py" services
  else
    log "l4lb is not running"
  fi
}

case "${1:-}" in
rs-start) rs_start ;;
rs-stop)  rs_stop ;;
lb)       lb ;;
curl)     do_curl ;;
bench)    bench ;;
status)   status ;;
*) sed -n '2,15p' "$0"; exit 1 ;;
esac
