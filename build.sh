#!/bin/bash
# =============================================================================
# 一键编译 l4lb
#
#   ./build.sh               # 编译（RelWithDebInfo），产物 build/l4lb
#   ./build.sh test          # 编译后运行单元测试（ctest）
#   ./build.sh clean         # 删除 build 目录后重新编译
#   ./build.sh asan          # ASan + UBSan 版本，输出到 build-asan/
#   ./build.sh debug         # Debug 版本，输出到 build-debug/
#   ./build.sh help
#
# 可以组合：./build.sh clean test
#
# 环境变量：
#   DPDK_PREFIX   DPDK 安装目录（含 lib64/pkgconfig 或 lib/pkgconfig）；
#                 不设置时依次尝试已有的 PKG_CONFIG_PATH、下面的候选目录
#   JOBS          并行编译数，默认 CPU 核数
#   CMAKE_ARGS    额外传给 cmake 的参数，如 "-DL4LB_NATIVE=OFF -DBUILD_TESTS=OFF"
# =============================================================================
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
JOBS=${JOBS:-$(nproc)}
CMAKE_ARGS=${CMAKE_ARGS:-}

# 本机 DPDK 不在系统默认路径（见 CMakeLists.txt），这里列出常见的安装位置
DPDK_CANDIDATES=(
  "${DPDK_PREFIX:-}"
  /root/dpvs/dpvs/dpdk-24.11/dpdklib
  /usr/local
  /opt/dpdk
)

log() { echo "[build] $*"; }
die() { echo "[build] error: $*" >&2; exit 1; }

usage() {
  # 打印文件头的注释块（第 2 行到第一行 "# ====" 之前）
  sed -n '3,/^# =====/p' "$0" | sed '$d'
  exit "${1:-0}"
}

# ---- 找到 libdpdk.pc ----
find_dpdk() {
  if pkg-config --exists libdpdk 2>/dev/null; then
    return 0
  fi
  local p d
  for p in "${DPDK_CANDIDATES[@]}"; do
    [ -n "$p" ] || continue
    for d in "$p/lib64/pkgconfig" "$p/lib/pkgconfig" "$p/lib/x86_64-linux-gnu/pkgconfig"; do
      if [ -f "$d/libdpdk.pc" ]; then
        export PKG_CONFIG_PATH="$d${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
        return 0
      fi
    done
  done
  die "libdpdk not found (tried PKG_CONFIG_PATH and: ${DPDK_CANDIDATES[*]}).
       Set DPDK_PREFIX=<dpdk install dir>, the directory that contains lib64/pkgconfig/libdpdk.pc"
}

# ---- 参数 ----
BUILD_DIR="$ROOT/build"
BUILD_TYPE=RelWithDebInfo
EXTRA=()
DO_CLEAN=0
DO_TEST=0

for arg in "$@"; do
  case "$arg" in
  clean) DO_CLEAN=1 ;;
  test)  DO_TEST=1 ;;
  asan)  BUILD_DIR="$ROOT/build-asan"; EXTRA+=(-DL4LB_SANITIZE=ON) ;;
  debug) BUILD_DIR="$ROOT/build-debug"; BUILD_TYPE=Debug ;;
  help|-h|--help) usage ;;
  *) echo "[build] unknown argument: $arg" >&2; usage 1 ;;
  esac
done

command -v cmake >/dev/null || die "cmake not found"
command -v pkg-config >/dev/null || die "pkg-config not found"
find_dpdk
log "DPDK $(pkg-config --modversion libdpdk) ($(pkg-config --variable=prefix libdpdk))"

if [ "$DO_CLEAN" = 1 ] && [ -d "$BUILD_DIR" ]; then
  log "removing $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

# ---- 配置 + 编译 ----
# shellcheck disable=SC2086  # CMAKE_ARGS 需要按空格拆分
cmake -S "$ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  "${EXTRA[@]}" $CMAKE_ARGS > "$BUILD_DIR.cmake.log" 2>&1 || {
  cat "$BUILD_DIR.cmake.log"
  die "cmake configure failed"
}
grep "l4lb:" "$BUILD_DIR.cmake.log" | sed 's/^-- /[build] /' || true
rm -f "$BUILD_DIR.cmake.log"

log "compiling with $JOBS jobs -> $BUILD_DIR"
start=$(date +%s)
cmake --build "$BUILD_DIR" -j "$JOBS"
log "done in $(( $(date +%s) - start ))s: $BUILD_DIR/l4lb"

# ---- 单元测试 ----
if [ "$DO_TEST" = 1 ]; then
  log "running unit tests"
  (cd "$BUILD_DIR" && ctest --output-on-failure)
fi
