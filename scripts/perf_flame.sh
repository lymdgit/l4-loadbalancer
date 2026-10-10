#!/bin/bash
# =============================================================================
# 采集 l4lb 的 on-CPU 火焰图
#
#   ./scripts/perf_flame.sh 30              # 采集运行中的 l4lb 30 秒
#   ./scripts/perf_flame.sh 30 -p 12345    # 指定 pid
#   FREQ=999 ./scripts/perf_flame.sh 10    # 提高采样频率（默认 99Hz）
#
# 依赖：perf（系统自带）、FlameGraph（自动 git clone 到 $FLAME_DIR）
# 产物：flame-<时间戳>.svg，用浏览器打开
# =============================================================================
set -euo pipefail

DUR=30
PID=""
FREQ=${FREQ:-99}
FLAME_DIR=${FLAME_DIR:-$HOME/FlameGraph}
OUT=flame-$(date +%Y%m%d-%H%M%S)

while [ $# -gt 0 ]; do
  case "$1" in
  -p) PID=$2; shift 2 ;;
  -h|--help) sed -n '3,/^# ====/p' "$0" | sed '$d'; exit 0 ;;
  *) DUR=$1; shift ;;
  esac
done

command -v perf >/dev/null || { echo "[flame] error: perf not found" >&2; exit 1; }

if [ -z "$PID" ]; then
  PID=$(pgrep -x l4lb | head -1)
  [ -n "$PID" ] || { echo "[flame] error: l4lb 未在运行，请先启动或用 -p 指定 pid" >&2; exit 1; }
fi

# FlameGraph 绘图脚本（纯 perl，无其他依赖）；无外网时可手动下载后放入 $FLAME_DIR
if [ ! -x "$FLAME_DIR/flamegraph.pl" ]; then
  echo "[flame] cloning FlameGraph -> $FLAME_DIR"
  git clone --depth 1 https://github.com/brendangregg/FlameGraph "$FLAME_DIR"
fi

echo "[flame] sampling pid=$PID freq=${FREQ}Hz dur=${DUR}s（frame-pointer 回溯）"
perf record -F "$FREQ" -g --call-graph fp -p "$PID" -- sleep "$DUR"

echo "[flame] generating $OUT.svg"
perf script -i perf.data > "$OUT.stacks"
"$FLAME_DIR/stackcollapse-perf.pl" "$OUT.stacks" > "$OUT.folded"
"$FLAME_DIR/flamegraph.pl" --title "l4lb on-CPU flame" "$OUT.folded" > "$OUT.svg"

rm -f "$OUT.stacks" "$OUT.folded"
echo "[flame] done: $OUT.svg（perf.data 已保留，可用 perf report 交互式分析）"
