#!/usr/bin/env bash
#
# scripts/doctor.sh —— 环境体检
#
#   make doctor
#
# 目的：把"看起来像实现错了"的环境问题提前打出来。最常见的四类：
#   1. clang-format 没装 → make format 直接失败（这是故意的，但不该临到头才发现）
#   2. 真 WT 没编 / 编了但 wt 解析到真库（capstone 会判 FAIL，但它不会告诉你怎么修）
#   3. /tmp 或仓库所在盘满了 → WAL/页写入随机失败，看起来像并发 bug
#   4. 构建目录是旧的（改过 include/ 却没重编）→ 行为对不上源码
#
# 退出码：0 = 没有 FAIL；1 = 有 FAIL（缺必需依赖）
set -u
cd "$(dirname "$0")/.." || exit 2

fail=0
ok() { printf '  OK    %s\n' "$*"; }
warn() { printf '  WARN  %s\n' "$*"; }
bad() {
    printf '  FAIL  %s\n' "$*"
    fail=$((fail + 1))
}

echo "=================================================================="
echo " tinywt doctor"
echo "=================================================================="

# ---------------------------------------------------------------- 系统
echo
echo "[系统]"
if grep -qi microsoft /proc/version 2>/dev/null; then
    ok "WSL: $(grep -oi 'microsoft.*' /proc/version | head -1)"
else
    ok "Linux: $(uname -sr)"
fi
ok "CPU: $(nproc 2>/dev/null || echo '?') 核"
if [ -r /proc/meminfo ]; then
    mem=$(awk '/MemTotal/ { printf "%.1f GiB", $2 / 1048576 }' /proc/meminfo)
    ok "内存: $mem"
    if [ "$(awk '/MemTotal/ { print $2 }' /proc/meminfo)" -lt 4000000 ]; then
        warn "内存 < 4GiB：cache_size=16G 之类的 wtperf 配置会直接失败"
    fi
fi
df -h . | awk 'NR == 2 { print "  OK    仓库磁盘可用: " $4 " (" $6 ")" }'
df -h /tmp | awk 'NR == 2 { print "  OK    /tmp 可用: " $4 }'

# ---------------------------------------------------------------- 工具链
echo
echo "[工具链]"
for t in cmake make gcc g++ python3; do
    if command -v "$t" >/dev/null 2>&1; then
        v=$("$t" --version 2>/dev/null | head -1)
        ok "$t: $v"
    else
        bad "$t 没装"
    fi
done
if command -v clang-format >/dev/null 2>&1; then
    ok "clang-format: $(clang-format --version)"
else
    warn "clang-format 没装 → make format 会失败；pip install --user clang-format"
fi
if command -v ninja >/dev/null 2>&1; then
    ok "ninja: $(ninja --version)"
else
    warn "ninja 没装（可选；cmake 默认用 make 也能编）"
fi

# ---------------------------------------------------------------- 仓库
echo
echo "[仓库]"
[ -f third_party/wiredtiger/include/wiredtiger.h ] &&
    ok "真 wiredtiger.h 在 third_party/" || bad "缺 third_party/wiredtiger/include/wiredtiger.h"
[ -f CMakeLists.txt ] && [ -f Makefile ] && ok "构建入口齐全" || bad "缺 CMakeLists.txt / Makefile"
if [ -d .git ]; then
    dirty=$(git status --porcelain 2>/dev/null | wc -l)
    if [ "$dirty" -eq 0 ]; then
        ok "git 干净"
    else
        warn "git 有 $dirty 处未提交改动"
    fi
else
    warn "不是 git 仓库（没有版本历史）"
fi

# ---------------------------------------------------------------- 真 WT
echo
echo "[真 WiredTiger 工具链]"
WT_SRC="${WT_SRC:-external/wiredtiger}"
if [ -f "$WT_SRC/bench/wtperf/wtperf.c" ] && [ -d "$WT_SRC/src/include" ]; then
    ok "源码树: $WT_SRC"
    WTPERF="$WT_SRC/build/bench/wtperf/wtperf"
    WT_BIN="$WT_SRC/build/wt"
    [ -x "$WT_BIN" ] && ok "wt: $WT_BIN" || warn "没有 $WT_BIN（make wt-tools）"
    [ -x "$WTPERF" ] && ok "wtperf: $WTPERF" || warn "没有 $WTPERF（make wt-tools）"
    if [ -f "$WT_SRC/build/libwiredtiger.so.11.3.0" ]; then
        ok "真库: $WT_SRC/build/libwiredtiger.so.11.3.0"
    else
        warn "真库没编出来 → capstone / diff-wt / crash REAL=1 都会 SKIP"
    fi
    if [ -x "$WT_BIN" ]; then
        got=$(ldd "$WT_BIN" 2>/dev/null | awk '/libwiredtiger/ { print $3; exit }')
        ok "wt 默认解析到: ${got:-?}（capstone 会用 LD_LIBRARY_PATH 换成我们的库）"
    fi
else
    warn "没有可用的 WT 源码树（make wt-tools 会克隆到 $WT_SRC）"
fi

# ---------------------------------------------------------------- 构建产物
echo
echo "[构建产物]"
for d in build build-asan build-perf; do
    if [ -d "$d" ]; then
        stale=""
        # include/ 或 src/ 比构建产物新 → 提醒重编（不武断判 FAIL）
        if [ -n "$(find include src -newer "$d/CMakeCache.txt" -type f 2>/dev/null | head -1)" ]; then
            stale="（源码比构建目录新，建议 make build）"
        fi
        ok "$d 存在$stale"
    else
        warn "$d 不存在"
    fi
done
if [ -f build/libwiredtiger.so.11.3.0 ]; then
    ok "drop-in 库: build/libwiredtiger.so.11.3.0"
    if [ -x build/bin/wt_client ]; then
        ok "作业客户端: build/bin/wt_client"
    else
        warn "没有 build/bin/wt_client（make build 会编出来）"
    fi
else
    warn "没有 build/libwiredtiger.so.11.3.0（make shared）"
fi

# ---------------------------------------------------------------- 收尾
echo
echo "=================================================================="
if [ "$fail" -gt 0 ]; then
    echo " 体检发现 $fail 项 FAIL —— 先修它们，再怀疑自己的实现。"
    echo "=================================================================="
    exit 1
fi
echo " 没有 FAIL。下一步：make todo P=P0 / make grade / make p0-test"
echo "=================================================================="
exit 0
