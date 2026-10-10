#!/bin/bash
# =============================================================================
# l4lb 单核转发能力测试（docs/压测方案.md 第十一节）
#
# 在本机启动 l4lb（net_memif server）和发包器 memif_gen（net_memif client），
# 两者通过共享内存直接交换报文，不经过网卡、虚拟交换机和内核协议栈。
# 测的是 l4lb 代码本身每秒能处理多少个包。
#
#   sudo tests/perf/memif_bench.sh                     # 默认：1 个 worker，按 RATES 逐档测
#   sudo WORKERS=2 tests/perf/memif_bench.sh           # 2 个 worker
#   sudo RATES="0" DURATION=20 tests/perf/memif_bench.sh   # 只测尽力发送
#
# 可调参数：
#   WORKERS=1           转发核数（pipeline：额外 1 个收包核）
#   LB_LCORES           l4lb 的 lcore，默认 1 到 1+WORKERS
#   GEN_LCORES=8-10     发包器的 lcore（main + 发包 + 收包）
#   RATES               发包速率（pps，0 = 尽力），默认 "500000 1000000 2000000 3000000 0"
#   DURATION=10         每档秒数
#   FLOWS=10000         并发流数（会话数）
#   SIZE=60             帧长（加 FCS 为 64 字节）
#   REFLECT=1           1：回射（入站 + 回程，每包 2 次转发）；0：只测入站
#
# 不碰物理网卡；但会占用大页，跑之前请停掉其他 l4lb（控制 socket 用独立路径）。
# 结果表格同时写到 tests/perf/results/<时间>-memif/summary.txt。
# =============================================================================
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WORKERS=${WORKERS:-1}
LB_LCORES=${LB_LCORES:-1-$((1 + WORKERS))}
GEN_LCORES=${GEN_LCORES:-8-10}
RATES=${RATES:-"500000 1000000 2000000 3000000 0"}
DURATION=${DURATION:-10}
FLOWS=${FLOWS:-10000}
SIZE=${SIZE:-60}
REFLECT=${REFLECT:-1}

WORK=$(mktemp -d /tmp/l4lb-memif.XXXXXX)
SOCK=$WORK/memif.sock
CTL=$WORK/ctl.sock
OUT=$ROOT/tests/perf/results/$(date +%Y%m%d-%H%M%S)-memif
LBPID=

log() { echo "[memif] $*"; }
die() { echo "[memif] error: $*" >&2; exit 1; }

cleanup() {
  if [ -n "$LBPID" ] && kill -0 "$LBPID" 2>/dev/null; then
    kill -INT "$LBPID"
    wait "$LBPID" 2>/dev/null || true
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

[ "$(id -u)" = 0 ] || die "run as root"
[ -x "$ROOT/build/l4lb" ] || die "build l4lb first: ./build.sh"

# ---- 编译发包器 ----
GEN=$ROOT/build/lab/memif_gen
SRC=$ROOT/tests/perf/tools/memif_gen.c
if [ ! -x "$GEN" ] || [ "$SRC" -nt "$GEN" ]; then
  if ! pkg-config --exists libdpdk 2>/dev/null; then
    for d in /root/dpvs/dpvs/dpdk-24.11/dpdklib/lib64/pkgconfig /usr/local/lib64/pkgconfig \
             /opt/dpdk/lib64/pkgconfig; do
      [ -f "$d/libdpdk.pc" ] && export PKG_CONFIG_PATH=$d && break
    done
  fi
  libdir=$(pkg-config --variable=libdir libdpdk)
  mkdir -p "$(dirname "$GEN")"
  log "building memif_gen"
  gcc -O2 -Wall -march=native -include rte_config.h -o "$GEN" "$SRC" \
    $(pkg-config --cflags --libs libdpdk) -Wl,-rpath,"$libdir"
fi

# ---- l4lb 配置：UDP 服务，静态 RS MAC，不做健康检查 ----
cat > "$WORK/lb.conf" <<EOF
[global]
mode = nat
dataplane = pipeline
log_dir = $WORK/log
log_stderr = off
stats_interval = 3600
udp_timeout = 30
[network]
# 客户端 10.1.x.x 与 VIP 同网段，回程用首包源 MAC，不需要 ARP
netmask = 255.0.0.0
local_ips = 10.0.0.2, 10.0.0.3, 10.0.0.4, 10.0.0.5
[healthcheck]
enabled = false
[control]
socket = $CTL
[service.udp]
vip = 10.0.0.1
port = 9
proto = udp
scheduler = maglev
server1 = 10.0.0.11:9:1:02:00:00:00:00:11
server2 = 10.0.0.12:9:1:02:00:00:00:00:12
EOF

log "starting l4lb on lcores $LB_LCORES ($WORKERS worker(s))"
"$ROOT/build/l4lb" -l "$LB_LCORES" --file-prefix l4lb-memif -m 1024 --no-pci \
  --vdev="net_memif0,role=server,socket=$SOCK,mac=02:00:00:00:00:01" \
  -- --lb-config "$WORK/lb.conf" > "$WORK/lb.out" 2>&1 &
LBPID=$!
for _ in $(seq 1 60); do
  [ -S "$CTL" ] && grep -q "is running" "$WORK/log/l4lb.log" 2>/dev/null && break
  kill -0 "$LBPID" 2>/dev/null || { cat "$WORK/lb.out" "$WORK"/log/*.log 2>/dev/null | tail -20; die "l4lb exited"; }
  sleep 0.5
done
[ -S "$CTL" ] || die "l4lb did not start"

CTLCMD=(python3 "$ROOT/scripts/l4lbctl.py" -s "$CTL")
mkdir -p "$OUT"
{
  echo "date: $(date -Is)  commit: $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null)$(git -C "$ROOT" diff --quiet 2>/dev/null || echo +dirty)"
  echo "cpu: $(LC_ALL=C lscpu | awk -F: '/^Model name/ {print $2; exit}' | xargs), $(nproc) vCPU"
  echo "l4lb: pipeline, $WORKERS worker(s), lcores $LB_LCORES; gen lcores $GEN_LCORES"
  echo "traffic: UDP ${SIZE}B+FCS, $FLOWS flows, reflect=$REFLECT, ${DURATION}s per rate"
  echo "columns: gen_tx = client pkts sent/s, gen_back = client pkts that made the full round trip/s,"
  echo "         fwd_* = l4lb forwarded pkts/s, busy = share of time spent on packets,"
  echo "         lb_loss = packets l4lb dropped (rx_ring_full / tx_full) / packets it handled"
  echo
  printf '%-8s %9s %9s %9s %9s %10s %9s %13s %9s\n' \
    target gen_tx gen_back fwd_in fwd_out fwd_total rcv_busy wk_busy lb_loss
} | tee "$OUT/summary.txt"

# memif 两端队列数要对应：l4lb TX = worker + 1（master 线程一个），RX = TX 向上取 2 的幂
LB_TXQ=$((WORKERS + 1))
LB_RXQ=1
while [ "$LB_RXQ" -lt "$LB_TXQ" ]; do LB_RXQ=$((LB_RXQ * 2)); done
GENARGS=(--flows "$FLOWS" --size "$SIZE" --duration "$DURATION" --rxq "$LB_TXQ" --txq "$LB_RXQ")
[ "$REFLECT" = 1 ] || GENARGS+=(--no-reflect)

for r in $RATES; do
  "${CTLCMD[@]}" counters > "$WORK/before.txt"
  "$GEN" -l "$GEN_LCORES" --file-prefix l4lb-memif-gen -m 512 --no-pci \
    --vdev="net_memif0,role=client,socket=$SOCK,mac=02:00:00:00:00:99" -- "${GENARGS[@]}" --rate "$r" \
    > "$WORK/gen.out" 2>&1 || { cat "$WORK/gen.out"; die "memif_gen failed"; }
  "${CTLCMD[@]}" delta "$WORK/before.txt" > "$OUT/delta_$r.txt"
  gen_tx=$(sed -n 's/.*tx_pps=\([0-9]*\).*/\1/p' "$WORK/gen.out")
  gen_back=$(sed -n 's/.*client_pps=\([0-9]*\).*/\1/p' "$WORK/gen.out")
  cp "$WORK/gen.out" "$OUT/gen_$r.txt"
  # pps 用发包器的发送时长做分母：delta 的区间还包含发包器启动和退出的时间
  gen_sec=$(sed -n 's/.* sec=\([0-9.]*\).*/\1/p' "$WORK/gen.out")
  awk -v target="$r" -v gtx="$gen_tx" -v gback="$gen_back" -v sec="$gen_sec" '
    function n(x) { gsub(",", "", x); return x + 0 }
    function h(v) { return v >= 1e6 ? sprintf("%.2fM", v / 1e6) : sprintf("%.0fk", v / 1e3) }
    /^fwd in/    { fin = n($3) }
    /^fwd out/   { fout = n($3) }
    /^busy:/ {
      line = $0
      if (match(line, /receiver [0-9.]+%/)) { s = substr(line, RSTART, RLENGTH); split(s, a, " "); rb = a[2] }
      wb = ""
      while (match(line, /worker[0-9]+ [0-9.]+%/)) {
        s = substr(line, RSTART, RLENGTH); split(s, a, " "); wb = wb (wb ? "/" : "") a[2]
        line = substr(line, RSTART + RLENGTH)
      }
    }
    /^drops/ { d = $2 }
    END {
      t = target == 0 ? "max" : sprintf("%.2fM", target / 1e6)
      tot = fin + fout
      loss = tot > 0 ? 100 * d / (tot + d) : 0
      printf "%-8s %9s %9s %9s %9s %10s %9s %13s %8.3f%%\n", t, h(gtx), h(gback), h(fin / sec),
             h(fout / sec), h(tot / sec), rb, wb, loss
    }' "$OUT/delta_$r.txt" | tee -a "$OUT/summary.txt"
  sleep 1
done

echo | tee -a "$OUT/summary.txt"
log "per-rate reports: $OUT/delta_*.txt"
log "summary: $OUT/summary.txt"
