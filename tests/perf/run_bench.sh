#!/bin/bash
# =============================================================================
# l4lb 性能测试（在压测客户端机器上运行，对应《项目重构.md》第四部分）
#
# 用法：
#   tests/perf/run_bench.sh <目标 URL> [标签]
#   例：tests/perf/run_bench.sh http://192.168.154.130/ fullnat-4core
#       tests/perf/run_bench.sh http://192.168.154.133/ direct      # 直连 RS 作基线
#
# 依赖：wrk（必需），wrk2（可选，测固定速率下的尾延迟）
# 可调参数（环境变量）：
#   DURATION=30  THREADS=4  CONNS="100 1000 2000"  RATES="20000 50000 80000"
#   REPEAT=3     LB_HOST=root@192.168.154.142（可选，压测前后和每项之间抓取 LB 统计）
#   LB_DIR=/root/l4-loadbalancer（LB 上的仓库路径）
#
# 结果写到 tests/perf/results/<时间>-<标签>/，summary.csv 汇总每一项的均值；
# 设置 LB_HOST 时每项还会记录 LB 的转发 PPS / bps（lb_delta_*.txt）。
# 同一套参数分别测：直连 RS、DR、FULLNAT，以及不同核数，便于对比。
# =============================================================================
set -euo pipefail

URL=${1:?usage: run_bench.sh <url> [label]}
LABEL=${2:-run}
DURATION=${DURATION:-30}
THREADS=${THREADS:-4}
CONNS=${CONNS:-"100 1000 2000"}
RATES=${RATES:-"20000 50000 80000"}
REPEAT=${REPEAT:-3}
LB_HOST=${LB_HOST:-}
LB_DIR=${LB_DIR:-/root/l4-loadbalancer}

command -v wrk >/dev/null || { echo "wrk not found"; exit 1; }
OUT="$(dirname "$0")/results/$(date +%Y%m%d-%H%M%S)-$LABEL"
mkdir -p "$OUT"
SUMMARY="$OUT/summary.csv"
# lb_pps / lb_bps：LB 转发的包速率和比特率（fwd in + out，压测前后 counters 相减），
# 需要 LB_HOST；见 docs/pps方案.md
echo "test,param,run,requests_per_sec,latency_avg_ms,latency_p99_ms,lb_pps,lb_bps,errors" > "$SUMMARY"

# ---- 环境记录（测试规范：结果必须可复现）----
{
  echo "date: $(date -Is)"
  echo "url: $URL  label: $LABEL"
  echo "client: $(hostname) $(uname -r)"
  echo "cpu: $(lscpu | awk -F: '/Model name/ {print $2; exit}' | xargs) x $(nproc)"
  echo "duration=$DURATION threads=$THREADS conns=[$CONNS] rates=[$RATES] repeat=$REPEAT"
  wrk --version 2>&1 | head -1 || true
} > "$OUT/env.txt"

LBCTL="python3 $LB_DIR/scripts/l4lbctl.py"

lb_stats() {
  [ -n "$LB_HOST" ] || return 0
  ssh -o BatchMode=yes "$LB_HOST" \
    "$LBCTL stats -v; $LBCTL stats -r; $LBCTL rate" \
    > "$OUT/lb_stats_$1.txt" 2>&1 || true
}

# 压测前在 LB 上保存一份计数（放在 LB 本地，避免来回拷贝）
lb_counters_begin() {
  [ -n "$LB_HOST" ] || return 0
  ssh -o BatchMode=yes "$LB_HOST" "$LBCTL counters > /tmp/l4lb_bench_before.txt" \
    2>/dev/null || true
}

# 压测后：与压测前的计数相减，输出 "pps,bps"，完整报告存到 $1
lb_counters_end() {
  [ -n "$LB_HOST" ] || { echo ","; return 0; }
  ssh -o BatchMode=yes "$LB_HOST" "$LBCTL delta /tmp/l4lb_bench_before.txt" \
    > "$1" 2>&1 || true
  ssh -o BatchMode=yes "$LB_HOST" "$LBCTL delta /tmp/l4lb_bench_before.txt --csv" \
    2>/dev/null | awk -F, 'NF > 2 {printf "%s,%s", $2, $3; ok = 1} END {if (!ok) printf ","}'
  echo
}

# 解析 wrk --latency 输出：Requests/sec、平均延迟、99%、错误数
parse() {
  local f=$1
  local rps avg p99 err
  rps=$(awk '/Requests\/sec/ {print $2}' "$f")
  avg=$(awk '/^ +Latency/ {print $2; exit}' "$f")
  p99=$(awk '/^ +99(\.000)?%/ {print $2; exit}' "$f")
  err=$(awk '/Non-2xx|Socket errors/ {s=s $0 ";"} END {print s}' "$f" | tr ',' ' ')
  echo "$rps,$(to_ms "$avg"),$(to_ms "$p99"),${err:-0}"
}

to_ms() {
  local v=$1
  case "$v" in
  *us) awk -v x="${v%us}" 'BEGIN {printf "%.3f", x/1000}' ;;
  *ms) echo "${v%ms}" ;;
  *s)  awk -v x="${v%s}" 'BEGIN {printf "%.3f", x*1000}' ;;
  *)   echo "" ;;
  esac
}

run() { # name param run cmd...
  local name=$1 param=$2 i=$3
  shift 3
  local f="$OUT/${name}_${param}_$i.txt"
  echo ">> $name $param run $i"
  lb_counters_begin
  "$@" > "$f" 2>&1 || true
  # 计数差值包含 wrk 启动和退出的几十毫秒，相对 DURATION 可以忽略
  local pps
  pps=$(lb_counters_end "$OUT/lb_delta_${name}_${param}_$i.txt")
  local w
  w=$(parse "$f")
  # parse 输出 rps,avg,p99,errors；把 lb_pps,lb_bps 插在 errors 之前
  echo "$name,$param,$i,${w%,*},$pps,${w##*,}" >> "$SUMMARY"
  lb_stats "${name}_${param}_$i"  # 每项结束时 LB 的计数、速率和忙碌率
}

lb_stats before

# 1. 长连接吞吐（QPS）：不同并发
for c in $CONNS; do
  for i in $(seq "$REPEAT"); do
    run keepalive "c$c" "$i" wrk -t"$THREADS" -c"$c" -d"${DURATION}s" --latency "$URL"
  done
done

# 2. 短连接（CPS）：每个请求新建 TCP 连接，持续时间足够让会话数超过会话表容量的数倍
for c in $CONNS; do
  for i in $(seq "$REPEAT"); do
    run shortconn "c$c" "$i" wrk -t"$THREADS" -c"$c" -d"${DURATION}s" --latency \
      -H "Connection: close" "$URL"
  done
done

# 3. 固定速率下的尾延迟（需要 wrk2，二进制名通常也是 wrk 或 wrk2）
if command -v wrk2 >/dev/null; then
  for r in $RATES; do
    for i in $(seq "$REPEAT"); do
      run fixedrate "r$r" "$i" wrk2 -t"$THREADS" -c1000 -d"${DURATION}s" -R"$r" \
        --latency "$URL"
    done
  done
else
  echo "wrk2 not found, skipping fixed-rate latency tests" | tee -a "$OUT/env.txt"
fi

lb_stats after

# ---- 汇总：每个 test/param 的均值 ----
echo
echo "test,param,avg_rps,avg_latency_ms,avg_p99_ms,avg_lb_pps,avg_lb_bps" | tee "$OUT/mean.csv"
awk -F, 'NR > 1 && $4 != "" {
  k = $1 "," $2; n[k]++; r[k] += $4; l[k] += $5; p[k] += $6; q[k] += $7; b[k] += $8
} END {
  for (k in n) printf "%s,%.0f,%.3f,%.3f,%.0f,%.0f\n", k, r[k]/n[k], l[k]/n[k],
                      p[k]/n[k], q[k]/n[k], b[k]/n[k]
}' "$SUMMARY" | sort | tee -a "$OUT/mean.csv"
echo
echo "results: $OUT"
