#!/usr/bin/env bash
#
# scripts/diff_wt.sh —— 行为对拍：同一份确定性操作流，tinywt 与真 WT 各跑一遍，
#                       最终数据必须逐字节相同。
#
#   make diff-wt            # 3 组种子
#   SEEDS="7 42" make diff-wt
#
# 这就是 docs/GUIDELINE.md 附录 B 里写的「调试的最高手段」，从手工步骤变成一条命令。
#
# 前置：make wt-tools（真 WT 源码 + wt_client 能链的 libwiredtiger.so）
# 依赖：P2 + P3/T3.1–T3.2（wiredtiger_open / create / cursor 写读）。
#       没到那一步时输出 SKIP，不算失败。
set -u
cd "$(dirname "$0")/.." || exit 2

SEEDS="${SEEDS:-1 2 3}"
KEYS="${KEYS:-2000}"
OPS="${OPS:-20000}"
WTLIB="${WT_LIBDIR:-external/wiredtiger/build}"
BUILD="${BUILD:-build}"

say() { printf '%s\n' "$*"; }

if [ ! -f "$WTLIB/libwiredtiger.so.11.3.0" ]; then
    say "SKIP  没有真 WT 库（$WTLIB/libwiredtiger.so.11.3.0）—— 先 make wt-tools"
    exit 2
fi
if ! cmake --build "$BUILD" -j "$(nproc 2>/dev/null || echo 4)" --target wiredtiger wt_client \
    >/dev/null 2>&1; then
    say "FAIL  $BUILD 构建失败（先 make build）"
    exit 1
fi
BIN="$BUILD/bin/wt_client"
[ -x "$BIN" ] || { say "FAIL  没有 $BIN"; exit 1; }

WORK=$(mktemp -d /tmp/twt_diff.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

fail=0
skip=0
for seed in $SEEDS; do
    h1="$WORK/tinywt-$seed"
    h2="$WORK/wt-$seed"
    mkdir -p "$h1" "$h2"

    LD_LIBRARY_PATH="$BUILD" "$BIN" diffdata --home "$h1" --keys "$KEYS" --ops "$OPS" \
        --seed "$seed" --out "$WORK/a-$seed.dump" 2>"$WORK/a-$seed.err"
    ra=$?
    LD_LIBRARY_PATH="$WTLIB" "$BIN" diffdata --home "$h2" --keys "$KEYS" --ops "$OPS" \
        --seed "$seed" --out "$WORK/b-$seed.dump" 2>"$WORK/b-$seed.err"
    rb=$?

    if [ $ra -eq 3 ]; then
        say "SKIP  seed=$seed: tinywt 还没准备好（$(tail -1 "$WORK/a-$seed.err")）"
        skip=$((skip + 1))
        continue
    fi
    if [ $ra -ne 0 ] || [ $rb -ne 0 ]; then
        say "FAIL  seed=$seed: 退出码 tinywt=$ra real=$rb"
        tail -3 "$WORK/a-$seed.err" | sed 's/^/        /'
        fail=$((fail + 1))
        continue
    fi

    if cmp -s "$WORK/a-$seed.dump" "$WORK/b-$seed.dump"; then
        n=$(grep -c . "$WORK/a-$seed.dump")
        say "  ok    seed=$seed: 两个引擎的 dump 完全一致（$n 行）"
    else
        say "FAIL  seed=$seed: dump 不一致"
        diff -u "$WORK/b-$seed.dump" "$WORK/a-$seed.dump" 2>/dev/null | head -12 |
            sed 's/^/        /'
        fail=$((fail + 1))
    fi
done

say
if [ $fail -gt 0 ]; then
    say "对拍失败（$fail 组）—— 上面 diff 的 '-' 是真 WT、'+' 是 tinywt"
    exit 1
fi
if [ $skip -eq $(echo $SEEDS | wc -w) ]; then
    say "全部 SKIP：tinywt 的 C ABI 还没通。先把 P2 与 T3.1/T3.2 做完。"
    exit 2
fi
say "对拍通过：同一操作流在两个引擎上产生同一份数据。"
exit 0
