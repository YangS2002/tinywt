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
#   $WT_SRC         真 WiredTiger 源码树（make wt-tools）
#   $WT_SRC/build/bench/wtperf/wtperf
#   $WT_SRC/build/wt
#   build/libwiredtiger.so                （make shared）
set -u

cd "$(dirname "$0")/.." || exit 2
# shellcheck source=scripts/lib_wt.sh
. "$PWD/scripts/lib_wt.sh"

# 源码根只有一个来源：lib_wt.sh 的 wt_src_root（无效树不算数）
WT_SRC=$(wt_src_root) || true

if [ -z "${WTPERF:-}" ]; then
    WTPERF=$(wt_find_bin "$WT_SRC/build/bench/wtperf/wtperf" "$WT_SRC/bench/wtperf/wtperf" \
        "$WT_SRC/build/wtperf") || true
fi
if [ -z "${WT_BIN:-}" ]; then
    WT_BIN=$(wt_find_bin "$WT_SRC/build/wt" "$WT_SRC/wt") || true
fi
RUNNERS_DIR="$WT_SRC/bench/wtperf/runners"
SHLIB="$PWD/build/libwiredtiger.so"
RESULT_DIR="$PWD/bench/results"
mkdir -p "$RESULT_DIR"

fail=0
skip=0

# 缩水：只改 icount，其余保持 runner 原配置 —— 结论才有可比性
ICCOUNT="${ICCOUNT:-200000}"

# 每个 runner / 每条 wt 命令的超时（秒）。
# ★ 必须有：空桩的 __wt_thread_create 返回错误但没建线程时，wtperf 的主线程
#   会永远等 worker 出现 —— 不是"慢"，是死挂。没有超时，整条 capstone 就吊死。
#   超时了就如实报"超时"，并提示多半是空桩而不是性能问题。
WT_TIMEOUT="${WT_TIMEOUT:-300}"

RUNNERS="populate 50r50u 80r20u rdonly evict-btree evict-btree-stress \
checkpoint-stress insert-rmw long-txn-btree index-pareto-btree"

say() { printf '%s\n' "$*"; }

# resolve_runner <name> → 打印真实配置路径
#
# ★ WT 仓库里的 runner 命名**不统一**：10 个里有 6 个就叫 <name>.wtperf，
#   另外 4 个（populate / 50r50u / 80r20u / rdonly）带规模前缀 500m-btree-。
#   旧脚本直接拼 "$name.wtperf"，这 4 个永远找不到，然后静默返回 "-"，
#   最后表现为"runner 全 FAIL"，看起来像 tinywt 的问题，其实是路径问题。
resolve_runner() {
    local name="$1" c
    for c in "$RUNNERS_DIR/$name.wtperf" \
        "$RUNNERS_DIR/500m-btree-$name.wtperf" \
        "$RUNNERS_DIR"/*-"$name".wtperf; do
        if [ -f "$c" ]; then
            printf '%s' "$c"
            return 0
        fi
    done
    return 1
}

# run_wtperf <runner> <tag> [preload:yes|no]  → 打印 ops/sec（失败打印 -）
run_wtperf() {
    local runner="$1" tag="$2" preload="${3:-yes}"
    local cfg
    local home="/tmp/twt_cap_${runner}_${tag}"
    local log="$RESULT_DIR/${runner}.${tag}.log"

    if ! cfg=$(resolve_runner "$runner"); then
        echo "-"
        return
    fi

    rm -rf "$home"
    local prefix=()
    [ "$preload" = "yes" ] && prefix=(env LD_PRELOAD="$SHLIB")

    local rc=0
    timeout "$WT_TIMEOUT" "${prefix[@]}" "$WTPERF" -O "$cfg" -o "icount=$ICCOUNT" -h "$home" \
        >"$log" 2>&1 || rc=$?

    if [ $rc -eq 0 ]; then
        # wtperf 的吞吐行形如：Executed 200000 ... (12345 ops/sec)
        grep -oE '[0-9]+ ops/sec' "$log" | tail -1 | grep -oE '^[0-9]+'
    else
        if [ $rc -eq 124 ]; then
            echo "!! 超时 ${WT_TIMEOUT}s：多半是某个 __wt_* 空桩让主线程在等 worker（不是慢）" >>"$log"
        fi
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
    wt_howto
    say "        → 然后 make shared（产出 build/libwiredtiger.so）"
    skip=$((skip + 1))
elif ! make -s shared; then
    say "  FAIL  build/libwiredtiger.so 编不出来"
    fail=$((fail + 1))
else
    for r in $RUNNERS; do
        v=$(run_wtperf "$r" tinywt yes)
        if [ "$v" = "-" ]; then
            say "  FAIL  $r（日志 $RESULT_DIR/$r.tinywt.log）"
            tail -5 "$RESULT_DIR/$r.tinywt.log" 2>/dev/null | sed 's/^/        /'
            fail=$((fail + 1))
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
    wt_howto
    skip=$((skip + 1))
elif ! make -s shared; then
    say "  FAIL  build/libwiredtiger.so 编不出来"
    fail=$((fail + 1))
else
    H=/tmp/twt_cap_wt
    rm -rf "$H"
    : >"$RESULT_DIR/wt.log"

    # ★ 只判退出码是不够的：真 wt 遇到解析失败会**打印 usage 然后 exit 0**。
    #   所以每步都要看输出内容，否则"什么都没做"会被记成 ok。
    # ★ 不能用 `... | tee`：管道的退出码是 tee 的，会把真工具的失败吞掉。
    wt_out() {
        local tmp rc
        tmp=$(mktemp)
        timeout "$WT_TIMEOUT" env LD_PRELOAD="$SHLIB" "$WT_BIN" -h "$H" "$@" >"$tmp" 2>&1
        rc=$?
        cat "$tmp" >>"$RESULT_DIR/wt.log"
        cat "$tmp"
        rm -f "$tmp"
        return $rc
    }

    # 真 wt 解析失败时打印的是 usage（含 global_options: 段）且 exit 0。
    # 这是本项目最容易出现的假通过：__wt_getopt 还没实现时，每条子命令都会走到这里。
    is_usage() { printf '%s\n' "$1" | grep -q '^global_options:'; }

    step() { # step <说明> <命令...>
        local what="$1"
        shift
        local out rc
        out=$(wt_out "$@")
        rc=$?
        if [ $rc -ne 0 ]; then
            say "  FAIL  wt $what（exit=$rc）"
            printf '%s\n' "$out" | tail -3 | sed 's/^/        /'
            fail=$((fail + 1))
            return 1
        fi
        if is_usage "$out"; then
            say "  FAIL  wt $what 打印的是 usage（命令没被识别，多半 __wt_getopt 还是空桩）"
            fail=$((fail + 1))
            return 1
        fi
        say "  ok    wt $what"
        return 0
    }

    step "list（空库）" list

    step "create table:t" create table:t

    # create 之后必须真的能列出来 —— 这是"写进去了"唯一的证据
    if out=$(wt_out list); then
        if is_usage "$out"; then
            say "  FAIL  wt list 打印的是 usage"
            fail=$((fail + 1))
        elif printf '%s\n' "$out" | grep -q '^table:t$'; then
            say "  ok    wt list 能看到 table:t"
        else
            say "  FAIL  wt list 里没有 table:t（create 没落地）"
            printf '%s\n' "$out" | head -5 | sed 's/^/        /'
            fail=$((fail + 1))
        fi
    else
        say "  FAIL  wt list（create 之后）"
        fail=$((fail + 1))
    fi

    step "verify table:t" verify table:t

    # stat 必须打出非空的统计行；空输出说明命令被当成 usage 吞了
    if out=$(wt_out stat table:t); then
        if printf '%s\n' "$out" | grep -qE '^[^ ]+:[ ]'; then
            say "  ok    wt stat table:t（有统计输出）"
        else
            say "  FAIL  wt stat table:t 没有统计输出"
            fail=$((fail + 1))
        fi
    else
        say "  FAIL  wt stat table:t"
        fail=$((fail + 1))
    fi

    step "drop table:t" drop table:t

    # drop 之后必须消失，否则 drop 是个空操作却 exit 0
    if out=$(wt_out list); then
        if printf '%s\n' "$out" | grep -q '^table:t$'; then
            say "  FAIL  wt list 在 drop 之后仍能看到 table:t"
            fail=$((fail + 1))
        else
            say "  ok    wt list 在 drop 之后看不到 table:t"
        fi
    else
        say "  FAIL  wt list（drop 之后）"
        fail=$((fail + 1))
    fi

    # load / dump 需要数据：先 create，用 wtperf populate 灌一份，再 dump -x 比对
    rm -rf "$H"
    POP_CFG=$(resolve_runner populate) || POP_CFG=""
    if [ -x "$WTPERF" ] && [ -n "${POP_CFG:-}" ]; then
        if "$WTPERF" -O "$POP_CFG" -o "icount=$ICCOUNT" -h "$H" \
            >"$RESULT_DIR/wt.load.log" 2>&1; then
            uri=$(env LD_PRELOAD="$SHLIB" "$WT_BIN" -h "$H" list 2>/dev/null |
                grep -m1 'table:' || true)
            [ -n "${uri:-}" ] || uri="table:test"
            # dump -x 是 C2 里唯一**必须字节级对齐**的地方：
            # 断言输出里有成对的十六进制字节，而不是"退出码 0 就算过"。
            if out=$(wt_out dump -x "$uri") &&
                printf '%s\n' "$out" | grep -qE '^[0-9a-f]{2}( [0-9a-f]{2})+'; then
                say "  ok    wt dump -x $uri（有十六进制转储）"
            else
                say "  FAIL  wt dump -x $uri（没有可解析的十六进制行）"
                fail=$((fail + 1))
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
            fail=$((fail + 1))
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
    ROWS=$(mktemp)
    for r in $RUNNERS; do
        v=$(grep -oE '[0-9]+ ops/sec' "$RESULT_DIR/$r.tinywt.log" 2>/dev/null |
            tail -1 | grep -oE '^[0-9]+')
        [ -n "${v:-}" ] && echo "| $r | $v |" >>"$ROWS"
    done

    # 一条数据都没有就不写。空表比不写更糟：它看起来像"跑过了，只是都是 0"。
    if [ -s "$ROWS" ]; then
        [ -f docs/LEADERBOARD.md ] || echo "# Leaderboard" >docs/LEADERBOARD.md
        {
            echo
            echo "## Capstone 结果（$(date '+%Y-%m-%d %H:%M')，icount=$ICCOUNT）"
            echo
            echo "| runner | tinywt ops/s |"
            echo "|---|---|"
            cat "$ROWS"
        } >>docs/LEADERBOARD.md
        say "  ok    已追加 $(wc -l <"$ROWS") 行到 docs/LEADERBOARD.md"
    else
        say "  SKIP  没有一个 runner 产出 ops/sec —— 不写空表"
        skip=$((skip + 1))
    fi
    rm -f "$ROWS"
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
# 退出码只表达"过 / 没过"，不把计数当退出码（>255 会被截断）
if [ "$fail" -gt 0 ]; then
    exit 1
fi
exit 0
