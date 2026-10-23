#!/usr/bin/env bash
#
# scripts/crash.sh —— 崩溃一致性：在随机时刻 SIGKILL，重开后验证「已提交前缀」不变式。
#
#   make crash              测 tinywt
#   make crash REAL=1       用真 WT 跑同一套判据（自检：判据本身必须能过）
#
# 判据（三条，crash-verify 实现）：
#   1. ack 文件里出现的每个 key 都必须还在（已提交 → 崩溃后必须恢复）
#   2. 键集合必须是 [0, max] 的连续前缀（不允许空洞）
#   3. 没 ack 但存在的 key 是允许的（提交了、还没来得及打印就被杀）
#
# 这条路走的是完整链路：cursor 写 → 事务提交 → WAL → 重开恢复。
# 依赖 P3/T3.4 + T3.5；没到那一步时输出 SKIP。
set -u
cd "$(dirname "$0")/.." || exit 2

SEEDS="${SEEDS:-1 2 3}"
KILL_AFTER="${KILL_AFTER:-3}"   # 秒
KEYS="${CRASH_KEYS:-20000000}"  # 足够大，保证一定会被杀
CKPT_EVERY="${CKPT_EVERY:-5000}"
REAL="${REAL:-0}"
BUILD="${BUILD:-build}"
WTLIB="${WT_LIBDIR:-external/wiredtiger/build}"

say() { printf '%s\n' "$*"; }

if [ "$REAL" = "1" ]; then
    LIBDIR="$WTLIB"
    LABEL="真 WT"
else
    LIBDIR="$BUILD"
    LABEL="tinywt"
fi

if [ ! -f "$LIBDIR/libwiredtiger.so.11.3.0" ]; then
    say "SKIP  没有 $LIBDIR/libwiredtiger.so.11.3.0（真 WT 先 make wt-tools；tinywt 先 make shared）"
    exit 2
fi
if ! cmake --build "$BUILD" -j "$(nproc 2>/dev/null || echo 4)" --target wiredtiger wt_client \
    >/dev/null 2>&1; then
    say "FAIL  $BUILD 构建失败（先 make build）"
    exit 1
fi
BIN="$BUILD/bin/wt_client"
[ -x "$BIN" ] || { say "FAIL  没有 $BIN"; exit 1; }

CFG="create,cache_size=64M,log=(enabled)"
WORK=$(mktemp -d /tmp/twt_crash.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

say "崩溃测试：$LABEL，每组跑 ${KILL_AFTER}s 后 SIGKILL，共 $(echo $SEEDS | wc -w) 组"
say

fail=0
skip=0
for seed in $SEEDS; do
    home="$WORK/h-$seed"
    ack="$WORK/ack-$seed"
    mkdir -p "$home"

    timeout -s KILL "$KILL_AFTER" env LD_LIBRARY_PATH="$LIBDIR" "$BIN" crash-write \
        --home "$home" --keys "$KEYS" --ckpt-every "$CKPT_EVERY" --ack "$ack" \
        --config "$CFG" >/dev/null 2>"$WORK/w-$seed.err"
    rc=$?

    if [ $rc -eq 3 ]; then
        say "SKIP  seed=$seed: 库还没准备好（$(tail -1 "$WORK/w-$seed.err")）"
        skip=$((skip + 1))
        continue
    fi
    if [ $rc -eq 0 ]; then
        say "SKIP  seed=$seed: 没被杀掉就写完了（把 --keys 调大或 KILL_AFTER 调小）"
        skip=$((skip + 1))
        continue
    fi

    acked=$(grep -c '^A ' "$ack" 2>/dev/null || echo 0)
    if [ "$acked" -eq 0 ]; then
        say "SKIP  seed=$seed: 3 秒内一条都没提交（实现太慢或卡住了），看 $WORK/w-$seed.err"
        skip=$((skip + 1))
        continue
    fi

    out=$(LD_LIBRARY_PATH="$LIBDIR" "$BIN" crash-verify --home "$home" --ack "$ack" \
        --keys "$KEYS" --config "$CFG" 2>"$WORK/v-$seed.err")
    vrc=$?
    if [ $vrc -eq 0 ]; then
        say "  ok    seed=$seed: 已提交 $acked 条，恢复后 $out"
    else
        say "FAIL  seed=$seed: $out"
        tail -3 "$WORK/v-$seed.err" | sed 's/^/        /'
        fail=$((fail + 1))
    fi
done

say
if [ $fail -gt 0 ]; then
    say "崩溃一致性失败（$fail 组）—— 检查 WAL 写入顺序 / 只重放已提交事务 / 截断边界"
    exit 1
fi
total=$(echo $SEEDS | wc -w)
if [ $skip -eq "$total" ]; then
    say "全部 SKIP：WAL 与恢复链路还没通（P3/T3.4–T3.5）。"
    say "自检判据：REAL=1 bash scripts/crash.sh —— 用真 WT 跑，必须全过。"
    exit 2
fi
say "崩溃一致性通过：所有已提交的数据都活下来了，没有中间空洞。"
exit 0
