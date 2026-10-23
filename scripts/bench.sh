#!/usr/bin/env bash
#
# scripts/bench.sh —— tinywt 自己的性能曲线（不等 wtperf，也不替代 wtperf）
#
#   make bench              只跑 tinywt
#   make bench REAL=1       同时跑真 WT，给出 ratio（对照组）
#
# 为什么需要它：
#   capstone 要等 P3/T3.6 全做完才能跑 wtperf。在那之前的几周里，"改完
#   eviction 是变快还是变慢"没有任何数字可看，只能靠感觉 —— 这个脚本就是
#   那条提前可用的曲线。
#
# ★ 性能数字必须用 Release 构建。Debug(-O0) 下比真 WT 慢一个数量级，
#   那是编译选项的差距，不是实现的差距。所以这里单开 build-perf/，
#   不碰 make grade 用的 Debug build/。
#
# 规矩（和 LEADERBOARD 一致）：
#   同一台机器、同一个构建、同一份 icount；每组跑 REPS 次取中位数。
set -u
cd "$(dirname "$0")/.." || exit 2

REPS="${REPS:-3}"
KEYS="${KEYS:-200000}"
SECONDS_PER_RUN="${SECONDS_PER_RUN:-3}"
VALUE_SIZE="${VALUE_SIZE:-120}"
REAL="${REAL:-0}"
PERF_BUILD=build-perf
RESULTS=bench/results

mkdir -p "$RESULTS"

say() { printf '%s\n' "$*"; }

# ---------------------------------------------------------------- 构建
say "== 构建 Release（$PERF_BUILD）"
if ! cmake -S . -B "$PERF_BUILD" -DCMAKE_BUILD_TYPE=Release -DTINYWT_BUILD_TESTS=OFF \
    >/dev/null 2>&1; then
    say "!! cmake 配置失败"; exit 2
fi
if ! cmake --build "$PERF_BUILD" -j "$(nproc 2>/dev/null || echo 4)" \
    --target wiredtiger wt_client >/dev/null 2>&1; then
    say "!! 构建失败（先跑 make build 看错误）"; exit 2
fi
BIN="$PERF_BUILD/bin/wt_client"
[ -x "$BIN" ] || { say "!! 没有 $BIN"; exit 2; }

# ---------------------------------------------------------------- 工具函数
median() {
    # shellcheck disable=SC2068
    printf '%s\n' $@ | sort -n | awk '{a[NR]=$1} END { if (NR) print a[int((NR+1)/2)] }'
}

json_field() { # <json> <field>
    printf '%s' "$1" | sed -n "s/.*\"$2\":\([0-9.]*\).*/\1/p"
}

run_one() { # <libdir> <home> <args...>
    local libdir="$1" home="$2"
    shift 2
    LD_LIBRARY_PATH="$libdir" "$BIN" "$@" --home "$home" \
        --config "create,cache_size=256M" 2>/dev/null
}

space_bytes() { # <home>
    local total=0 f
    for f in "$1"/*.wt; do
        [ -f "$f" ] || continue
        total=$((total + $(stat -c %s "$f" 2>/dev/null || echo 0)))
    done
    printf '%s' "$total"
}

# ---------------------------------------------------------------- 主流程
bench_lib() { # <label> <libdir> <rows_file>
    local label="$1" libdir="$2" rows="$3"
    local home="/tmp/twt_bench_$label"
    rm -rf "$home"; mkdir -p "$home"

    local out rc
    out=$(run_one "$libdir" "$home" populate --keys "$KEYS" --value-size "$VALUE_SIZE")
    rc=$?
    if [ $rc -eq 3 ]; then
        say "  SKIP  $label: 库还没准备好（wiredtiger_open 失败，P3/T3.1 之前正常）"
        return 3
    elif [ $rc -ne 0 ]; then
        say "  FAIL  $label: populate 失败（exit=$rc）"
        return 1
    fi
    local space; space=$(space_bytes "$home")

    local op r v vals
    for op in read update; do
        vals=()
        for ((r = 1; r <= REPS; r++)); do
            out=$(run_one "$libdir" "$home" bench --op "$op" --keys "$KEYS" \
                --seconds "$SECONDS_PER_RUN")
            rc=$?
            if [ $rc -ne 0 ]; then
                say "  FAIL  $label/$op: bench 失败（exit=$rc）"; return 1
            fi
            v=$(json_field "$out" ops_per_sec)
            [ -n "$v" ] || { say "  FAIL  $label/$op: 没有 ops_per_sec"; return 1; }
            vals+=("$v")
        done
        echo "$op $(median "${vals[@]}") $space" >>"$rows"
        say "  $label/$op: $(median "${vals[@]}") ops/s（${vals[*]}）"
    done
    return 0
}

say
say "== tinywt 性能（Release，keys=$KEYS，每组 ${SECONDS_PER_RUN}s × $REPS）"
TMP=$(mktemp)
bench_lib tinywt "$PERF_BUILD" "$TMP"
rc=$?

if [ $rc -eq 3 ]; then
    rm -f "$TMP"
    say
    say "当前是骨架状态，没有可测的数字。先做 P0–P2 + T3.1/T3.2。"
    exit 2
fi
if [ $rc -ne 0 ]; then
    rm -f "$TMP"
    exit 1
fi

REAL_ROWS=""
if [ "$REAL" = "1" ]; then
    WTLIB="${WT_LIBDIR:-external/wiredtiger/build}"
    if [ ! -f "$WTLIB/libwiredtiger.so.11.3.0" ]; then
        say "  SKIP  对照组：没有 $WTLIB/libwiredtiger.so.11.3.0（先 make wt-tools）"
    else
        say
        say "== 对照组：真 WiredTiger"
        REAL_ROWS=$(mktemp)
        bench_lib wt "$WTLIB" "$REAL_ROWS" || true
    fi
fi

# ---------------------------------------------------------------- 汇总
say
say "=================================================================="
printf '  %-10s %14s %14s %8s\n' workload tinywt "real-WT" ratio
say "------------------------------------------------------------------"
while read -r op val space; do
    real="-"
    ratio="-"
    if [ -n "$REAL_ROWS" ] && [ -s "$REAL_ROWS" ]; then
        real=$(awk -v o="$op" '$1 == o { print $2 }' "$REAL_ROWS")
        [ -n "$real" ] || real="-"
        if [ "$real" != "-" ] && [ "$real" -gt 0 ] 2>/dev/null; then
            ratio=$(awk -v a="$val" -v b="$real" 'BEGIN { printf "%.2f", a / b }')
        fi
    fi
    printf '  %-10s %14s %14s %8s\n' "$op" "$val" "$real" "$ratio"
done <"$TMP"
say "=================================================================="
SPACE=$(awk 'NR == 1 { print $3 }' "$TMP")
say "  数据文件：${SPACE:-0} 字节（keys=$KEYS，value=$VALUE_SIZE）"
say "  ratio = tinywt / 真 WT；学习实现慢 1–2 个数量级是正常的。"
say

STAMP=$(date '+%Y%m%d-%H%M%S')
OUT="$RESULTS/bench-$STAMP.json"
{
    printf '{"time":"%s","keys":%s,"value_size":%s,"reps":%s,"rows":[' \
        "$(date -Iseconds)" "$KEYS" "$VALUE_SIZE" "$REPS"
    first=1
    while read -r op val space; do
        [ $first -eq 1 ] || printf ','
        first=0
        printf '{"op":"%s","tinywt_ops":%s,"space_bytes":%s' "$op" "$val" "${space:-0}"
        if [ -n "$REAL_ROWS" ] && [ -s "$REAL_ROWS" ]; then
            real=$(awk -v o="$op" '$1 == o { print $2 }' "$REAL_ROWS")
            [ -n "$real" ] && printf ',"real_ops":%s' "$real"
        fi
        printf '}'
    done <"$TMP"
    printf ']}\n'
} >"$OUT"
say "结果已写入 $OUT"
rm -f "$TMP"
[ -n "$REAL_ROWS" ] && rm -f "$REAL_ROWS"
exit 0
