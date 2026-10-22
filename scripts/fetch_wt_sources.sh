#!/usr/bin/env bash
# 拉取真 WT 测试工具源码子集到 third_party/wt/（不提交，见 .gitignore）。
#
# P3-T3.2 起用它编译真 wtperf，链接 tinywt 的 libwiredtiger.a。
# 本地有 WT 克隆时可直接 cp，不必联网。
#
# 用法：
#   scripts/fetch_wt_sources.sh                # 从 GitHub raw 下载
#   scripts/fetch_wt_sources.sh /path/to/clone # 从本地克隆拷
set -euo pipefail

BRANCH="${WT_BRANCH:-mongodb-8.0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/third_party/wt"
BASE="https://raw.githubusercontent.com/wiredtiger/wiredtiger/$BRANCH"

# 依赖面：bench/wtperf（7 源文件）+ test/utility（testutil）+ 公共头
FILES=(
  src/include/wiredtiger.in
  bench/wtperf/CMakeLists.txt
  bench/wtperf/wtperf.c
  bench/wtperf/wtperf.h
  bench/wtperf/wtperf_config.c
  bench/wtperf/wtperf_misc.c
  bench/wtperf/wtperf_throttle.c
  bench/wtperf/wtperf_truncate.c
  bench/wtperf/track.c
  bench/wtperf/idle_table_cycle.c
  bench/wtperf/config_opt.h
  bench/wtperf/wtperf_opt_inline.h
  test/utility/test_util.h
  test/utility/parse_opts.c
  test/utility/misc.c
  test/utility/thread.c
  test/utility/file.c
  test/utility/util_modify.c
  test/utility/util_random.c
)

if [[ $# -ge 1 ]]; then
  SRC="$1"
  [[ -d "$SRC" ]] || { echo "不是目录：$SRC" >&2; exit 2; }
  for f in "${FILES[@]}"; do
    mkdir -p "$OUT/$(dirname "$f")"
    cp -f "$SRC/$f" "$OUT/$f"
  done
else
  for f in "${FILES[@]}"; do
    mkdir -p "$OUT/$(dirname "$f")"
    curl -fsSL "$BASE/$f" -o "$OUT/$f"
  done
fi

echo "WT 测试工具源码子集 → $OUT"
echo "下一步：make probe（校验合同头），P3-T3.2 起编译真 wtperf"
