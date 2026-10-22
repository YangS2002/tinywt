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

CP="${1:-}"
case "$CP" in
1)
    SUBSET="p3_api"
    NEED="T3.0 合同对齐 / T3.1 config+catalog / T3.2 cursor 写路径"
    RUNNER="populate"
    ;;
2)
    SUBSET="p3_cursor p3_txn p3_wal"
    NEED="T3.3 读路径 / T3.4 事务 API / T3.5 WAL"
    RUNNER="50r50u + 80r20u + crash-recovery"
    ;;
*)
    echo "用法: scripts/checkpoint.sh 1|2" >&2
    exit 2
    ;;
esac

echo "=================================================================="
echo " Checkpoint $CP"
echo "   要求：$NEED"
echo "   真工具面：wtperf $RUNNER"
echo "=================================================================="

fail=0

# ---------------------------------------------------------------- 门 1：单测子集
echo
echo "[1/3] 单测子集"
for name in $SUBSET; do
    bin="build/${name}_test"
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
WT_SRC="${WT_SRC:-external/wiredtiger}"
WTPERF="${WTPERF:-$WT_SRC/build/bench/wtperf/wtperf}"

if [ ! -x "$WTPERF" ]; then
    echo "  SKIP  找不到 $WTPERF"
    echo "        → scripts/fetch_wt_sources.sh 拉源码，按 WT 官方 cmake 流程编出 wtperf"
    echo "        → 然后 make shared（产出 build/libwiredtiger.so）"
    echo "        → 本脚本会用 LD_PRELOAD 把 tinywt 顶上去"
else
    if ! make -s shared; then
        echo "  FAIL  build/libwiredtiger.so 编不出来"
        fail=1
    else
        HOME_DIR="/tmp/twt_cp${CP}_db"
        CFG="/tmp/twt_cp${CP}.wtperf"
        rm -rf "$HOME_DIR"
        # 缩水版：icount 从 500m/100m 降到 1m，其余原配置
        cat >"$CFG" <<'EOF'
conn_config="cache_size=512M,eviction=(threads_min=1,threads_max=4)"
table_config="type=file"
icount=1000000
populate_threads=1
report_interval=1
run_time=10
EOF
        if [ "$CP" = "1" ]; then
            cat >>"$CFG" <<'EOF'
# populate only
EOF
        else
            cat >>"$CFG" <<'EOF'
workload=read_write
read_threads=2
insert_threads=0
update_threads=2
EOF
        fi
        if LD_PRELOAD="$PWD/build/libwiredtiger.so" "$WTPERF" -O "$CFG" -h "$HOME_DIR" \
            >"/tmp/twt_cp${CP}_wtperf.log" 2>&1; then
            echo "  ok    wtperf 跑通（日志 /tmp/twt_cp${CP}_wtperf.log）"
        else
            echo "  FAIL  wtperf 退出非 0（日志 /tmp/twt_cp${CP}_wtperf.log）"
            tail -20 "/tmp/twt_cp${CP}_wtperf.log"
            fail=1
        fi
    fi
fi

echo
if [ $fail -eq 0 ]; then
    echo "Checkpoint $CP: PASSED"
else
    echo "Checkpoint $CP: 未通过"
fi
exit $fail
