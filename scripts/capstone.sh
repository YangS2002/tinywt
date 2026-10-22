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
if WT_SRC=$(wt_src_root); then
    SRC_OK=1
else
    SRC_OK=0
fi

if [ -z "${WTPERF:-}" ]; then
    WTPERF=$(wt_find_bin "$WT_SRC/build/bench/wtperf/wtperf" "$WT_SRC/bench/wtperf/wtperf" \
        "$WT_SRC/build/wtperf") || true
fi
if [ -z "${WT_BIN:-}" ]; then
    WT_BIN=$(wt_find_bin "$WT_SRC/build/wt" "$WT_SRC/wt") || true
fi
RUNNERS_DIR="$WT_SRC/bench/wtperf/runners"
# SHLIB 可以覆盖：唯一正当的用途是 make capstone-selftest —— 指着真库跑一遍，
# 验证"判据本身"在正确实现上是全绿的（一份永远报错的检查比没有检查更浪费）。
SHLIB="${SHLIB:-$PWD/build/libwiredtiger.so}"
RESULT_DIR="$PWD/bench/results"
mkdir -p "$RESULT_DIR"

# 库的接入方式由 wt_setup_dropin 决定（replace / preload），必须在 make shared
# 成功之后才能探测 —— 它要看 build/ 下有没有与 NEEDED 同名的 .so。
DROPIN_DESC="（还没构建 .so）"

fail=0
skip=0

# 缩水：icount + run_time + warmup + compression，四项都是必须改的。
# ★ 不是"想改小一点好跑"，是原配置在这台机器上根本跑不完 / 连不上：
#   run_time=7200  → 一条 runner 两小时
#   warmup=120     → 缩了 run_time 之后预热比跑的时间还长
#   compression=snappy 未编出、且本项目只支持 NONE/RLE → 连接直接失败
#   详见 wt_shrink_opts 的注释。
ICCOUNT="${ICCOUNT:-200000}"
RUN_TIME="${RUN_TIME:-20}"
WARMUP="${WARMUP:-5}"
wt_shrink_opts "$ICCOUNT" "$RUN_TIME" "$WARMUP"

# 每个 runner / 每条 wt 命令的超时（秒）。
# ★ 必须有：空桩的 __wt_thread_create 返回错误但没建线程时，wtperf 的主线程
#   会永远等 worker 出现 —— 不是"慢"，是死挂。没有超时，整条 capstone 就吊死。
#   超时了就如实报"超时"，并提示多半是空桩而不是性能问题。
WT_TIMEOUT="${WT_TIMEOUT:-300}"

# 可覆写：RUNNERS="populate 50r50u" make capstone —— 排查单个 runner 时别跑全家桶。
# 用 ${RUNNERS-默认}（没有冒号）：显式的 RUNNERS= （空串）表示"一个都不跑"，
# capstone-selftest 靠这个跳过 C1。写成 ${RUNNERS:-...} 时空串会被当成没设，
# 自检就会莫名其妙跑起全家桶来。
RUNNERS="${RUNNERS-populate 50r50u 80r20u rdonly evict-btree evict-btree-stress \
checkpoint-stress insert-rmw long-txn-btree index-pareto-btree}"

# resolve_runner / wt_run_wtperf / wt_shrink_opts 都在 lib_wt.sh 里
# （checkpoint.sh 也要用），这里只保留一个薄封装，避免两处各写一份、判法还不一样。
say() { printf '%s\n' "$*"; }

# run_wtperf <runner> <tag> [use_our_lib:yes|no]  → 打印 ops/sec（失败打印 -）
run_wtperf() {
    local runner="$1" tag="$2" use_lib="${3:-yes}"
    local cfg pcfg
    local home="/tmp/twt_cap_${runner}_${tag}"
    local log="$RESULT_DIR/${runner}.${tag}.log"

    if ! cfg=$(resolve_runner "$RUNNERS_DIR" "$runner"); then
        echo "-"
        return
    fi

    wt_new_home "$home"

    # ★ create=false 的 runner 假定 home 里**已经有一份 populate 数据**
    #   （配置注释原话："assumes that its correlating populate already completed"）。
    #   直接跑的结果是 find_table_count: open_cursor failed —— 看着像库坏了，
    #   其实只是没数据。所以先灌一遍，用同一个 home。
    #   缩水参数里 compression 已经是 none，populate 那一步也必须带上，
    #   否则建出来的表用 snappy，后面读的时候照样连不上。
    if wt_runner_needs_populate "$cfg"; then
        if ! pcfg=$(resolve_runner "$RUNNERS_DIR" populate); then
            echo "-"
            return
        fi
        wt_run_wtperf "$WTPERF" "$pcfg" "$home" "$WT_TIMEOUT" \
            "${log%.log}.populate.log" "$use_lib" "${WT_SHRINK[@]}" >/dev/null
    fi

    wt_run_wtperf "$WTPERF" "$cfg" "$home" "$WT_TIMEOUT" "$log" "$use_lib" "${WT_SHRINK[@]}"
}

say "=================================================================="
say " tinywt Capstone（icount=$ICCOUNT）"
say "=================================================================="

# ---------------------------------------------------------------- C1
say
say "[C1] runner 全家桶"
if [ ! -x "$WTPERF" ]; then
    if [ "$SRC_OK" -eq 0 ]; then
        say "  SKIP  没有可用的真 WiredTiger 源码树（WT_SRC=${WT_SRC:-未设置}）"
    else
        say "  SKIP  找不到 wtperf（已按候选路径查找：$WT_SRC/build/bench/wtperf/wtperf 等）"
    fi
    wt_howto
    say "        → 然后 make shared（产出 build/libwiredtiger.so）"
    skip=$((skip + 1))
elif ! make -s shared; then
    say "  FAIL  build/libwiredtiger.so 编不出来"
    fail=$((fail + 1))
else
    # ★ 这里**不能**写 DROPIN_DESC=$(wt_setup_dropin ...)：$( ) 是子 shell，
    #   WT_ENV 跟着一起没了，工具就照 RUNPATH 找真库、飞快全过 —— 假通过。
    wt_setup_dropin "$WTPERF" "$SHLIB"
    say "  库模式：$(wt_dropin_desc)"
    if ! real_lib=$(wt_check_dropin "$WTPERF" "$SHLIB"); then
        say "  FAIL  替换没生效：wtperf 实际链的是 ${real_lib:-?}"
        say "        别往下看结果 —— 那样跑出来的数字是真 WT 的"
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
fi

# ---------------------------------------------------------------- C2
say
say "[C2] wt 工具子集（list/create/drop/dump -x/load/verify/stat）"
if [ ! -x "$WT_BIN" ]; then
    if [ "$SRC_OK" -eq 0 ]; then
        say "  SKIP  没有可用的真 WiredTiger 源码树（WT_SRC=${WT_SRC:-未设置}）"
    else
        say "  SKIP  找不到 wt（已按候选路径查找：$WT_SRC/build/wt）"
    fi
    wt_howto
    skip=$((skip + 1))
elif ! make -s shared; then
    say "  FAIL  build/libwiredtiger.so 编不出来"
    fail=$((fail + 1))
else
    # ★ 这里**不能**写 DROPIN_DESC=$(wt_setup_dropin ...)：$( ) 是子 shell，
    #   WT_ENV 跟着一起没了，wt 就照 RUNPATH 找真库 —— 全绿，但跑的是真 WT。
    wt_setup_dropin "$WT_BIN" "$SHLIB"
    say "  库模式：$(wt_dropin_desc)"
    if ! real_lib=$(wt_check_dropin "$WT_BIN" "$SHLIB"); then
        say "  FAIL  替换没生效：wt 实际链的是 ${real_lib:-?}"
        say "        停在这里 —— 继续跑只会把真 WT 的行为记成你的成绩"
        fail=$((fail + 1))
    else
        H=/tmp/twt_cap_wt
        rm -rf "$H"
        mkdir -p "$H"
        WT_HOME="$H" # 当前操作的 home；load 回路会临时切到 H2
        : >"$RESULT_DIR/wt.log"

        # ★ 只判退出码是不够的：真 wt 遇到解析失败会**打印 usage 然后 exit 0**。
        #   所以每步都要看输出内容，否则"什么都没做"会被记成 ok。
        # ★ 不能用 `... | tee`：管道的退出码是 tee 的，会把真工具的失败吞掉。
        wt_out() {
            local tmp rc
            tmp=$(mktemp)
            timeout "$WT_TIMEOUT" "${WT_ENV[@]}" "$WT_BIN" -h "$WT_HOME" "$@" >"$tmp" 2>&1
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

        # ★ 这里**不**先跑 "list（空库）"。看着像个合理的冒烟步骤，实测真 WT
        #   11.3.0 在空目录上直接 `wt -h <empty> list` 会退出 1：
        #     __posix_open_file: .../WiredTiger.lock: handle-open: open:
        #     No such file or directory   →   WT_TRY_SALVAGE
        #   而 `create` 在同一个空目录上能正常建库。所以"空库能 list"不是真 WT 的
        #   行为，把它当判据会让我们自己的实现永远过不了。第一条命令直接用 create。
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

        # ---------------------------------------------------------------- load / dump
        # ★ 这一段以前是"用 wtperf populate 灌数据，然后 dump -x 看一眼"：
        #   (a) 根本**没跑过 load**（文档却写着 C2 覆盖 load）；
        #   (b) 依赖 wtperf，populate 一失败整段就 SKIP —— C2 被 C1 拖着走；
        #   (c) "dump -x 有十六进制行"只能证明它能打印，证明不了能还原。
        #
        # 现在改成闭环，且不依赖 wtperf：
        #   create(S/S) → write 三条 → dump → load 到全新 home → 再 dump → 逐字节比
        # 判据是**两次 dump 完全相同**。这是 C2 里唯一能证明"dump 与 load 互为逆操作"
        # 的检查，也间接证明了 cell 编码在文本层的自洽。
        H2=/tmp/twt_cap_wt_rt
        RT_A=/tmp/twt_cap_rt_a.dump
        RT_B=/tmp/twt_cap_rt_b.dump
        rm -rf "$H2"
        mkdir -p "$H2"

        RT_OK=1
        step "create -c key_format=S,value_format=S table:rt" \
            create -c "key_format=S,value_format=S" table:rt || RT_OK=0
        for kv in "k1 v1" "k2 v2" "k3 v3"; do
            # shellcheck disable=SC2086
            step "write table:rt $kv" write table:rt $kv || RT_OK=0
        done

        # ★ dump 的头三行是 load 的**硬契约**（external/wiredtiger/src/utilities/util_load.c:253-280）：
        #     第 1 行以 "WiredTiger Dump " 开头（注意有尾部空格，后面紧跟版本）
        #     第 2 行恰好 "Format=print"（-x 时是 "Format=hex"）
        #     第 3 行恰好 "Header"
        #   之后是 uri / config **成对**出现（成奇数条 load 直接判格式错），直到一行 "Data"，
        #   再往下是数据：print 模式下**一行键、一行值**（不是一行一对）。
        #   契约里任何一条不对，load 只会回一句 "invalid format"，不告诉你是哪一行 ——
        #   所以这些必须在这里先判，报错才有指向性。
        dump_contract() {
            local f="$1" n
            case "$(head -1 "$f")" in
            "WiredTiger Dump "*) ;;
            *) printf '第 1 行不是 "WiredTiger Dump ..."（实际：%s）' "$(head -1 "$f")"; return 1 ;;
            esac
            [ "$(sed -n 2p "$f")" = "Format=print" ] ||
                { printf '第 2 行不是 Format=print（实际：%s）' "$(sed -n 2p "$f")"; return 1; }
            [ "$(sed -n 3p "$f")" = "Header" ] ||
                { printf '第 3 行不是 Header（实际：%s）' "$(sed -n 3p "$f")"; return 1; }
            grep -qx 'Data' "$f" || { printf '没有 Data 段'; return 1; }
            grep -q '^k1\\00$' "$f" || { printf '数据段里没有键 k1 的行 k1\\00'; return 1; }
            # Header 段必须是 uri/config 成对（util_load.c:313 的要求，奇数条直接判格式错）
            n=$(awk 'f && !/^Data$/ { c++ } /^Header$/ { f = 1 } END { print c + 0 }' "$f")
            [ $((n % 2)) -eq 0 ] || { printf 'Header 段 %s 行，uri/config 必须成对' "$n"; return 1; }
            # print 模式下三条记录 = 6 行；数不对说明键值没有分行输出
            n=$(awk 'seen { c++ } /^Data$/ { seen = 1 } END { print c + 0 }' "$f")
            [ "$n" -eq 6 ] || { printf 'Data 段有 %s 行，应为 6 行（3 键 3 值，键值分行）' "$n"; return 1; }
            return 0
        }

        if [ "$RT_OK" -eq 1 ]; then
            # dump 文本（不是 -x）：load 默认只认这个格式
            if wt_out dump table:rt >"$RT_A" 2>&1 && why=$(dump_contract "$RT_A"); then
                say "  ok    wt dump table:rt（$(wc -c <"$RT_A") 字节，3 条记录，格式合 load 的契约）"
            else
                say "  FAIL  wt dump table:rt：${why:-命令本身失败（见 $RESULT_DIR/wt.log）}"
                head -4 "$RT_A" 2>/dev/null | sed 's/^/        /'
                fail=$((fail + 1))
                RT_OK=0
            fi
        fi

        if [ "$RT_OK" -eq 1 ]; then
            WT_HOME="$H2"
            # step 内部已经记过一次 fail，这里不要再记 —— 否则一次失败报成两项
            if step "load -f（导入全新 home）" load -f "$RT_A"; then
                if wt_out dump table:rt >"$RT_B" 2>&1 && cmp -s "$RT_A" "$RT_B"; then
                    say "  ok    dump A == dump B（load 与 dump 互为逆操作）"
                else
                    say "  FAIL  导入后再 dump 与原 dump 不一致"
                    diff "$RT_A" "$RT_B" 2>/dev/null | head -6 | sed 's/^/        /'
                    fail=$((fail + 1))
                fi
            fi
            WT_HOME="$H"
        fi

        # dump -x：人类可读的十六进制转储。正确性由上面对拍负责（dump/load 闭环），
        # 这里只判"打出来的是真数据"。
        #
        # ★ 格式以实测为准（真 WT 11.3.0）：
        #     -x 不是 "6b 31 00" 这种空格分隔，而是**连写的十六进制**，一行一条：
        #       6b3100      ← 键 "k1\0"
        #       763100      ← 值 "v1\0"
        #   所以不能拿 `xx xx` 的模式去匹配 —— 那会永远 FAIL。这里的判据是：
        #   存在连写 hex 行，且其中确实有 k1\0 的编码 6b3100。
        if [ "$RT_OK" -eq 1 ]; then
            if out=$(wt_out dump -x table:rt) &&
                printf '%s\n' "$out" | grep -qE '^([0-9a-f]{2})+$' &&
                printf '%s\n' "$out" | grep -qx '6b3100'; then
                say "  ok    wt dump -x table:rt（十六进制里有 k1\\0 → 6b3100）"
            else
                say "  FAIL  wt dump -x table:rt（没有连写 hex 行，或找不到 k1 的编码）"
                fail=$((fail + 1))
            fi
        fi
    fi
fi

# ---------------------------------------------------------------- C3
say
say "[C3] 对照组：真 WT vs tinywt（同 runner、同 icount）"
if [ "${SELFTEST:-0}" = 1 ]; then
    say "  —   自检模式：跳过 C3（两边本来就是同一个库）"
elif [ ! -x "$WTPERF" ] || [ ! -f "$SHLIB" ]; then
    say "  SKIP  缺 wtperf 或 libwiredtiger.so"
    skip=$((skip + 1))
else
    printf '  %-22s %12s %12s %8s\n' runner "real-WT" "tinywt" "ratio"
    for r in ${C3_RUNNERS:-50r50u evict-btree}; do
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
if [ "${SELFTEST:-0}" = 1 ]; then
    say "  —   自检模式：不写榜单（这一轮的\"tinywt\"数字其实是真库的）"
elif [ "$SHLIB" != "$PWD/build/libwiredtiger.so" ]; then
    # 校准跑（SHLIB 指着真库）出来的数字也是真 WT 的，写进榜就是假数据。
    # 榜单只收"本项目构建产物"跑出来的数。
    say "  —   SHLIB 不是本项目产物（$SHLIB），不写榜单"
elif ls "$RESULT_DIR"/*.tinywt.log >/dev/null 2>&1; then
    ROWS=$(mktemp)
    for r in $RUNNERS; do
        # 走和 C1 同一个解析函数：这里以前也取最后一行，于是写进榜单的是
        # "Executed 0 scan operations ... 0 ops/sec" 的 0。
        v=$(wt_ops_from_log "$RESULT_DIR/$r.tinywt.log")
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
if [ "${SELFTEST:-0}" = 1 ]; then
    if [ "$fail" -eq 0 ]; then
        say " 自检通过：C1/C2 的判据在真库上全绿 —— 判据没写错"
        say "=================================================================="
        exit 0
    fi
    say " 自检失败（$fail 项）：判据本身有问题，先修判据，再谈实现"
    say "=================================================================="
    exit 1
fi
if [ "$fail" -gt 0 ]; then
    say " Capstone: 未通过（$fail 项 FAIL，$skip 项 SKIP）"
elif [ "$skip" -gt 0 ]; then
    # ★ SKIP 不算通过。以前这里写的是 "PASSED（N 项 SKIP）" —— 一次
    #   "一个 runner 都没跑"的运行也会显示 PASSED，那是最容易骗过自己的
    #   一种绿：它只证明"没跑"，不证明"跑对了"。
    say " Capstone: 未完成（$skip 项 SKIP）—— SKIP 不算通过"
    say "           缺依赖就补：make wt-tools，然后 make capstone"
else
    say " Capstone: PASSED"
fi
say "=================================================================="
# 退出码区分三种情况：0 通过 / 1 有 FAIL / 2 没跑完（只有 SKIP）。
# 不把计数当退出码（>255 会被截断）。
if [ "$fail" -gt 0 ]; then
    exit 1
fi
if [ "$skip" -gt 0 ]; then
    exit 2
fi
exit 0
