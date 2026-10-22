#!/usr/bin/env bash
#
# scripts/checkpoint.sh —— Checkpoint gate（不计入 make grade，但「没过不准进下一段」）
#
#   make cp1   # = T3.0–T3.2  合同对齐 / config+catalog / cursor 写路径
#   make cp2   # = T3.3–T3.5  读路径 / 事务 API / WAL
#
# 三道门，缺一不可：
#   1. 单测子集全绿（不是"能跑"，是全 PASS）
#   2. probe-link 能链上（缺哪个接口，链接器直接报符号）
#   3. 真 wtperf 能跑（用 build/libwiredtiger.so 做 drop-in）
#
# 第 3 道门需要真 WT 源码；没准备好时**跳过并明说**，不装作通过。
set -u

cd "$(dirname "$0")/.." || exit 2
# shellcheck source=scripts/lib_wt.sh
. "$PWD/scripts/lib_wt.sh"

CP="${1:-}"
case "$CP" in
1)
    SUBSET="p3_api"
    NEED="T3.0 合同对齐 / T3.1 config+catalog / T3.2 cursor 写路径"
    RUNNERS="populate"
    ;;
2)
    SUBSET="p3_cursor p3_txn p3_wal"
    NEED="T3.3 读路径 / T3.4 事务 API / T3.5 WAL"
    # crash-recovery 不是 wtperf 的 runner，它由上面的 p3_wal 子集覆盖；
    # 这里只跑真工具面 —— 别在提示里承诺脚本没做的事。
    RUNNERS="50r50u 80r20u"
    ;;
*)
    echo "用法: scripts/checkpoint.sh 1|2" >&2
    exit 2
    ;;
esac

echo "=================================================================="
echo " Checkpoint $CP"
echo "   要求：$NEED"
echo "   真工具面：wtperf $RUNNERS"
echo "=================================================================="

# 空桩的 __wt_thread_create 不建线程，wtperf 主线程会死等 —— 必须设上限。
WT_TIMEOUT="${WT_TIMEOUT:-600}"
ICCOUNT="${ICCOUNT:-1000000}"

fail=0

# ---------------------------------------------------------------- 门 1：单测子集
echo
echo "[1/3] 单测子集"
for name in $SUBSET; do
    # 产物在 build/bin/（见 CMakeLists 的 CMAKE_RUNTIME_OUTPUT_DIRECTORY），
    # 不是 build/ 根下 —— 写错这里会让门 1 永远是 MISS，看起来像"没编译"。
    bin="build/bin/${name}_test"
    log="/tmp/twt_cp${CP}_${name}.log"
    if [ ! -x "$bin" ]; then
        echo "  MISS  $bin（先 make build）"
        fail=1
        continue
    fi
    if ./"$bin" >"$log" 2>&1; then
        echo "  ok    $name"
    else
        echo "  FAIL  $name  （日志 $log）"
        grep -E 'FAIL |NOTIMPL|ERROR|exception' "$log" | grep -v '^RESULT ' | head -20
        fail=1
    fi
done

# ---------------------------------------------------------------- 门 2：链接期合同
echo
echo "[2/3] 链接期合同（probe-link）"
plog=/tmp/twt_cp${CP}_probe.log
if make -s probe-link >"$plog" 2>&1; then
    echo "  ok    全部符号可解析"
else
    echo "  FAIL  缺符号（= 还没实现的接口）："
    grep -o "undefined reference to [^ ]*" "$plog" | sed 's/.*to //' | sort -u | head -40
    fail=1
fi

# ---------------------------------------------------------------- 门 3：真 wtperf
echo
echo "[3/3] 真 wtperf（drop-in libwiredtiger.so）"

# 源码根只有一个来源：lib_wt.sh 的 wt_src_root（无效树不算数）
WT_SRC=$(wt_src_root) || true

# wtperf 的位置随放法不同，按候选顺序找第一个可执行的
if [ -z "${WTPERF:-}" ]; then
    WTPERF=$(wt_find_bin "$WT_SRC/build/bench/wtperf/wtperf" \
        "$WT_SRC/bench/wtperf/wtperf" \
        "$WT_SRC/build/wtperf") || true
fi
WTPERF="${WTPERF:-$WT_SRC/build/bench/wtperf/wtperf}"

if [ ! -x "$WTPERF" ]; then
    echo "  SKIP  找不到 wtperf（已找过 $WT_SRC 下的几个常见位置）"
    wt_howto
    echo "        → 然后 make shared（产出 build/libwiredtiger.so）"
    echo "        → 本脚本会让真工具加载 build/ 下的库（SONAME 同名则替换，否则预加载）"
else
    if ! make -s shared; then
        echo "  FAIL  build/libwiredtiger.so 编不出来"
        fail=1
    else
        # ★ 不能写成 echo "$(wt_setup_dropin ...)"：命令替换是子 shell，WT_ENV
        #   一赋值就随子 shell 消失，wtperf 于是照 RUNPATH 找到真库 —— 照样跑通，
        #   但那跑的是真 WT。这里必须在当前 shell 调用，再单独读说明文字。
        wt_setup_dropin "$WTPERF" "$PWD/build/libwiredtiger.so"
        echo "  库模式：$(wt_dropin_desc)"
        if ! real_lib=$(wt_check_dropin "$WTPERF" "$PWD/build/libwiredtiger.so"); then
            echo "  FAIL  替换没生效：wtperf 实际链的是 ${real_lib:-?}"
            echo "        不再往下跑 —— 那样得到的数字是真 WT 的"
            fail=1
            RUNNERS="" # 空列表 = 下面的 for 一次都不跑
        fi
        # 跑**真 runner 配置**，不是手写一份长得像的：
        #   cp1 = populate（纯插入：split + reconcile 稳态）
        #   cp2 = 50r50u / 80r20u（读写混合：update 链 + 淘汰）
        # ★ 缩的是四项（icount / run_time / warmup / compression），理由见
        #   lib_wt.sh 的 wt_shrink_opts：不缩 run_time 一条要跑两小时，
        #   不改 compression 则连接时 __wt_dlopen(snappy) 直接失败。
        wt_shrink_opts "$ICCOUNT" "${RUN_TIME:-20}" "${WARMUP:-5}"
        RUNNERS_DIR="$WT_SRC/bench/wtperf/runners"
        for runner in $RUNNERS; do
            if ! CFG=$(resolve_runner "$RUNNERS_DIR" "$runner"); then
                echo "  FAIL  找不到 runner 配置：$runner（$RUNNERS_DIR）"
                wt_howto
                fail=1
                continue
            fi
            HOME_DIR="/tmp/twt_cp${CP}_${runner}_db"
            LOG="/tmp/twt_cp${CP}_${runner}.log"
            wt_new_home "$HOME_DIR"
            # create=false 的 runner 要先有一份 populate 数据，否则
            # find_table_count: open_cursor failed（详见 lib_wt.sh 同名判据）
            if wt_runner_needs_populate "$CFG"; then
                if PC=$(resolve_runner "$RUNNERS_DIR" populate); then
                    wt_run_wtperf "$WTPERF" "$PC" "$HOME_DIR" "$WT_TIMEOUT" \
                        "${LOG%.log}.populate.log" yes "${WT_SHRINK[@]}" >/dev/null
                else
                    echo "  FAIL  $runner 需要 populate 打底，但找不到 populate runner"
                    fail=1
                    continue
                fi
            fi
            V=$(wt_run_wtperf "$WTPERF" "$CFG" "$HOME_DIR" "$WT_TIMEOUT" "$LOG" yes \
                "${WT_SHRINK[@]}")
            if [ "$V" = "-" ]; then
                echo "  FAIL  wtperf $runner（日志 $LOG）"
                tail -3 "$LOG" | sed 's/^/        /'
                fail=1
            else
                echo "  ok    wtperf $runner  ${V} ops/sec（icount=$ICCOUNT）"
            fi
        done
    fi
fi

echo
if [ $fail -eq 0 ]; then
    echo "Checkpoint $CP: PASSED"
else
    echo "Checkpoint $CP: 未通过"
fi
exit $fail
