#!/usr/bin/env bash
#
# scripts/wt_tools.sh —— 准备真 WT 工具链（wt + wtperf），供 cp1/cp2/capstone 做 drop-in
#
#   make wt-tools
#
# 做三件事：
#   1. 浅克隆固定 tag 的 WiredTiger 到 external/wiredtiger（已有有效树则跳过）
#   2. 打一个幂等补丁：base.cmake 无条件 find_package(Python3 ... REQUIRED)，
#      而机器上没有 python3-dev 就配不过去。我们只要 wt/wtperf，不要 python 绑定，
#      所以把那次探测降级为「可选」。不改系统、不装包。
#   3. cmake 配置 + 只编 wt / wtperf 两个 target
#
# 产物：
#   external/wiredtiger/build/wt                    真 wt 工具
#   external/wiredtiger/build/bench/wtperf/wtperf   真 wtperf
#
# 可用环境变量：
#   WT_TAG         要克隆的分支/标签（默认 mongodb-8.0）
#   WT_SRC         源码位置（默认 external/wiredtiger）
#   WT_CLEAN=1     重新配置（删掉 build/CMakeCache.txt）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

WT_TAG="${WT_TAG:-mongodb-8.0}"
SRC="${WT_SRC:-$ROOT/external/wiredtiger}"
BUILD="$SRC/build"
REPO_URL="${WT_REPO:-https://github.com/wiredtiger/wiredtiger}"
JOBS="$(nproc 2>/dev/null || echo 4)"

say() { printf '%s\n' "$*"; }

# ---------------------------------------------------------------- 1. 源码
if [ -f "$SRC/bench/wtperf/wtperf.c" ] && [ -d "$SRC/src/include" ]; then
    say "[1/3] 源码已就位：$SRC"
else
    say "[1/3] 浅克隆 $WT_TAG → $SRC"
    mkdir -p "$(dirname "$SRC")"
    if [ -e "$SRC" ]; then
        # 目录在但不是有效树（例如旧 fetch 脚本留下的 wiredtiger.in 空壳）。
        # 不删用户文件：挪到一边，让人还能找回来。
        mv "$SRC" "$SRC.stale.$(date +%s)"
        say "      已有目录不是完整源码树，已改名保留"
    fi
    git clone --depth 1 --branch "$WT_TAG" "$REPO_URL" "$SRC"
fi

# ---------------------------------------------------------------- 2. 补丁
# 幂等：改过之后模式不再匹配，重复跑是 no-op。
BASE_CMAKE="$SRC/cmake/configs/base.cmake"
if grep -q '^source_python3_package(python_libs python_version python_executable)$' "$BASE_CMAKE"; then
    say "[2/3] 打补丁：Python3 探测改为可选（无 python3-dev 也能构建 wt/wtperf）"
    sed -i 's|^source_python3_package(python_libs python_version python_executable)$|# tinywt: python 绑定不是构建目标；无 python3-dev 时不要 REQUIRED 失败\nif(TINYWT_WITH_PYTHON)\n  source_python3_package(python_libs python_version python_executable)\nendif()|' "$BASE_CMAKE"
else
    say "[2/3] 补丁已在（或该版本无需补丁）"
fi

# ---------------------------------------------------------------- 3. 构建
if [ "${WT_CLEAN:-0}" = "1" ]; then
    rm -rf "$BUILD"
fi
mkdir -p "$BUILD"

# 配置每次都跑：cmake 自身幂等，且上一次「配置失败」会留下 CMakeCache.txt 却没有
# Makefile —— 用 cache 存在与否当判据会把这种半成品当成功复用。
say "[3/3] cmake 配置 → $BUILD"
cmake -S "$SRC" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_SHARED=1 -DENABLE_STATIC=0 -DENABLE_PYTHON=0 \
    -DHAVE_DIAGNOSTIC=0 \
    -DENABLE_LZ4=0 -DENABLE_SNAPPY=0 -DENABLE_ZLIB=0 -DENABLE_ZSTD=0 -DENABLE_SODIUM=0 \
    >"$BUILD/configure.log" 2>&1 || {
    say "      配置失败，日志末尾："
    tail -20 "$BUILD/configure.log" | sed 's/^/      /'
    exit 1
}

say "      编译 wt / wtperf（-j $JOBS）"
cmake --build "$BUILD" --target wt wtperf -j "$JOBS" \
    >"$BUILD/build.log" 2>&1 || {
    say "      编译失败，日志末尾："
    tail -30 "$BUILD/build.log" | sed 's/^/      /'
    exit 1
}

WTPERF="$BUILD/bench/wtperf/wtperf"
WT_BIN="$BUILD/wt"

say
if [ -x "$WTPERF" ] && [ -x "$WT_BIN" ]; then
    say "完成："
    say "  wt      $WT_BIN"
    say "  wtperf  $WTPERF"
    say "  runners $SRC/bench/wtperf/runners（$(ls "$SRC"/bench/wtperf/runners/*.wtperf 2>/dev/null | wc -l) 个配置）"
    say
    say "下一步：make cp1 / make capstone"
else
    say "构建结束但产物缺失：wtperf=$WTPERF wt=$WT_BIN" >&2
    exit 1
fi
