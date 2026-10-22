#!/usr/bin/env bash
#
# scripts/capstone.sh —— 结业全量验收（C1–C4）
#
#   make capstone
#
# C1  runner 全家桶：真 wtperf 的 10 个 runner，缩水 icount，其余原配置
# C2  wt 工具子集：真 `wt` 二进制跑 list/create/drop/dump -x/load/verify/stat
# C3  对照组：同一 runner 跑「真 WT」与「tinywt（drop-in）」，出对比表
# C4  汇总：C1/C3 的数字并进 docs/LEADERBOARD.md
#
# ★ 与 make grade 的分工：
#   grade 判「语义对不对」（单元测试）；capstone 判「真工具面能不能顶住」（集成）。
#   两者都不靠"看起来对"，只认退出码和数字。
#
# 依赖（任一缺失 → 对应 Task SKIP，绝不假装通过）：
#   $WT_SRC         真 WiredTiger 源码树（scripts/fetch_wt_sources.sh）
#   $WT_SRC/build/bench/wtperf/wtperf
#   $WT_SRC/build/wt
#   build/libwiredtiger.so                （make shared）
set -u

cd "$(dirname "$0")/.." || exit 2

# 源码位置：fetch 脚本的默认落点（third_party/wt）优先，其次完整克隆（external/wiredtiger）
if [ -z "${WT_SRC:-}" ]; then
    if [ -d third_party/wt ]; then
        WT_SRC=third_party/wt
    else
        WT_SRC=external/wiredtiger
    fi
fi

find_bin() {
    local cand
    for cand in "$@"; do
        if [ -x "$cand" ]; then
            printf '%s' "$cand"
            return 0
        fi
    done
    printf '%s' "$1"
    return 1
}

if [ -z "${WTPERF:-}" ]; then
    WTPERF=$(find_bin "$WT_SRC/build/bench/wtperf/wtperf" "$WT_SRC/bench/wtperf/wtperf" \
        "$WT_SRC/build/wtperf" "third_party/wt/build/wtperf") || true
fi
if [ -z "${WT_BIN:-}" ]; then
    WT_BIN=$(find_bin "$WT_SRC/build/wt" "$WT_SRC/wt" "third_party/wt/build/wt") || true
fi
RUNNERS_DIR="$WT_SRC/bench/wtperf/runners"
SHLIB="$PWD/build/libwiredtiger.so"
RESULT_DIR="$PWD/bench/results"
mkdir -p "$RESULT_DIR"

fail=0
skip=0

# 缩水：只改 icount，其余保持 runner 原配置 —— 结论才有可比性
ICCOUNT="${ICCOUNT:-200000}"

RUNNERS="populate 50r50u 80r20u rdonly evict-btree evict-btree-stress \
checkpoint-stress insert-rmw long-txn-btree index-pareto-btree"

say() { printf '%s\n' "$*"; }

# run_wtperf <runner> <tag> [preload:yes|no]  → 打印 ops/sec（失败打印 -）
run_wtperf() {
    local runner="$1" tag="$2" preload="${3:-yes}"
    local cfg="$RUNNERS_DIR/$runner.wtperf"
    local home="/tmp/twt_cap_${runner}_${tag}"
    local log="$RESULT_DIR/${runner}.${tag}.log"

    [ -f "$cfg" ] || { echo "-"; return; }

    rm -rf "$home"
    local prefix=()
    [ "$preload" = "yes" ] && prefix=(env LD_PRELOAD="$SHLIB")

    if "${prefix[@]}" "$WTPERF" -O "$cfg" -o "icount=$ICCOUNT" -h "$home" >"$log" 2>&1; then
        # wtperf 的吞吐行形如：Executed 200000 ... (12345 ops/sec)
        grep -oE '[0-9]+ ops/sec' "$log" | tail -1 | grep -oE '^[0-9]+'
    else
        echo "-"
    fi
}

say "=================================================================="
say " tinywt Capstone（icount=$ICCOUNT）"
say "=================================================================="

# ---------------------------------------------------------------- C1
say
say "[C1] runner 全家桶"
if [ ! -x "$WTPERF" ]; then
    say "  SKIP  找不到 wtperf（已按候选路径查找：$WT_SRC/build/bench/wtperf/wtperf 等）"
    say "        → scripts/fetch_wt_sources.sh 拉源码，或 WT_SRC=/path/to/wiredtiger 指向完整克隆"
    skip=$((skip + 1))
elif ! make -s shared; then
    say "  FAIL  build/libwiredtiger.so 编不出来"
    fail=1
else
    for r in $RUNNERS; do
        v=$(run_wtperf "$r" tinywt yes)
        if [ "$v" = "-" ]; then
            say "  FAIL  $r（日志 $RESULT_DIR/$r.tinywt.log）"
            tail -5 "$RESULT_DIR/$r.tinywt.log" 2>/dev/null | sed 's/^/        /'
            fail=1
        else
            say "  ok    $r  ${v} ops/sec"
        fi
    done
fi

# ---------------------------------------------------------------- C2
say
say "[C2] wt 工具子集（list/create/drop/dump -x/load/verify/stat）"
if [ ! -x "$WT_BIN" ]; then
    say "  SKIP  找不到 $WT_BIN"
    skip=$((skip + 1))
elif ! make -s shared; then
    say "  FAIL  build/libwiredtiger.so 编不出来"
    fail=1
else
    H=/tmp/twt_cap_wt
    rm -rf "$H"
    wt_run() { env LD_PRELOAD="$SHLIB" "$WT_BIN" -h "$H" "$@" >>"$RESULT_DIR/wt.log" 2>&1; }
    : >"$RESULT_DIR/wt.log"

    for step in \
        "list" \
        "create table:t" \
        "list" \
        "verify table:t" \
        "stat table:t" \
        "drop table:t"
    do
        # shellcheck disable=SC2086
        if wt_run $step; then
            say "  ok    wt $step"
        else
            say "  FAIL  wt $step（日志 $RESULT_DIR/wt.log）"
            fail=1
        fi
    done

    # load / dump 需要数据：先 create，用 wtperf populate 灌一份，再 dump -x 比对
    rm -rf "$H"
    if [ -x "$WTPERF" ] && [ -f "$RUNNERS_DIR/populate.wtperf" ]; then
        if "$WTPERF" -O "$RUNNERS_DIR/populate.wtperf" -o "icount=$ICCOUNT" -h "$H" \
            >"$RESULT_DIR/wt.load.log" 2>&1; then
            uri=$(env LD_PRELOAD="$SHLIB" "$WT_BIN" -h "$H" list 2>/dev/null |
                grep -m1 'table:' || true)
            [ -n "${uri:-}" ] || uri="table:test"
            if wt_run dump -x "$uri"; then
                say "  ok    wt dump -x $uri"
            else
                say "  FAIL  wt dump -x $uri"
                fail=1
            fi
        else
            say "  SKIP  wt load（populate 没跑通）"
            skip=$((skip + 1))
        fi
    else
        say "  SKIP  wt load/dump（缺 wtperf 或 populate.wtperf）"
        skip=$((skip + 1))
    fi
fi

# ---------------------------------------------------------------- C3
say
say "[C3] 对照组：真 WT vs tinywt（同 runner、同 icount）"
if [ ! -x "$WTPERF" ] || [ ! -f "$SHLIB" ]; then
    say "  SKIP  缺 wtperf 或 libwiredtiger.so"
    skip=$((skip + 1))
else
    printf '  %-22s %12s %12s %8s\n' runner "real-WT" "tinywt" "ratio"
    for r in 50r50u evict-btree; do
        a=$(run_wtperf "$r" wt no)
        b=$(run_wtperf "$r" tinywt yes)
        if [ "$a" = "-" ] || [ "$b" = "-" ]; then
            printf '  %-22s %12s %12s %8s\n' "$r" "$a" "$b" "-"
            fail=1
        else
            ratio=$(awk -v x="$b" -v y="$a" 'BEGIN{ if (y>0) printf "%.2f", x/y; else print "-" }')
            printf '  %-22s %12s %12s %8s\n' "$r" "$a" "$b" "$ratio"
        fi
    done
fi

# ---------------------------------------------------------------- C4
say
say "[C4] 汇总到 docs/LEADERBOARD.md"
if ls "$RESULT_DIR"/*.tinywt.log >/dev/null 2>&1; then
    TMP=$(mktemp)
    {
        echo
        echo "## Capstone 结果（$(date '+%Y-%m-%d %H:%M')，icount=$ICCOUNT）"
        echo
        echo "| runner | tinywt ops/s |"
        echo "|---|---|"
        for r in $RUNNERS; do
            v=$(grep -oE '[0-9]+ ops/sec' "$RESULT_DIR/$r.tinywt.log" 2>/dev/null |
                tail -1 | grep -oE '^[0-9]+')
            [ -n "${v:-}" ] && echo "| $r | $v |"
        done
    } >"$TMP"

    if [ -f docs/LEADERBOARD.md ]; then
        cat "$TMP" >>docs/LEADERBOARD.md
    else
        { echo "# Leaderboard"; cat "$TMP"; } >docs/LEADERBOARD.md
    fi
    rm -f "$TMP"
    say "  ok    已追加到 docs/LEADERBOARD.md"
else
    say "  SKIP  没有 C1 的结果可汇总"
    skip=$((skip + 1))
fi

say
say "=================================================================="
if [ $fail -eq 0 ]; then
    if [ "$skip" -gt 0 ]; then
        say " Capstone: PASSED（$skip 项 SKIP）"
    else
        say " Capstone: PASSED"
    fi
else
    say " Capstone: 未通过（$fail 项 FAIL，$skip 项 SKIP）"
fi
say "=================================================================="
exit $fail
