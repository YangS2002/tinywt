#!/usr/bin/env bash
#
# scripts/lib_wt.sh —— 被 checkpoint.sh / capstone.sh source 的真 WT 依赖定位层
#
# 只有一份实现，避免两个脚本各判一次、判法还不一样。
# 关键约定：**目录存在 ≠ 源码有效**。旧逻辑用 [ -d third_party/wt ] 判断，
# 结果一个只有 wiredtiger.in 的空壳目录就能让全部门 SKIP —— 那不是"没准备好"，
# 是"假装找过了"。这里改为检查真实文件。
#
# 提供：
#   wt_src_root    选真 WT 源码根（无效树一律跳过）
#   wt_find_bin    按候选顺序找第一个可执行文件
#   wt_howto       缺依赖时打印补齐方式（统一口径，不各写一套）

# 真 WT 源码树的判据：wtperf 的入口源 + 内部头目录同时在。
_twt_is_wt_tree() {
    [ -n "${1:-}" ] && [ -f "$1/bench/wtperf/wtperf.c" ] && [ -d "$1/src/include" ]
}

# wt_src_root → stdout。优先 $WT_SRC（显式指定必须有效，否则报错而不是静默降级）。
wt_src_root() {
    if [ -n "${WT_SRC:-}" ]; then
        if _twt_is_wt_tree "$WT_SRC"; then
            printf '%s' "$WT_SRC"
            return 0
        fi
        echo "WT_SRC=$WT_SRC 不是完整的 WiredTiger 源码树（缺 bench/wtperf/wtperf.c）" >&2
        return 1
    fi

    local c
    for c in third_party/wt external/wiredtiger; do
        if _twt_is_wt_tree "$c"; then
            printf '%s' "$c"
            return 0
        fi
    done

    # 都没找到：给一个"本来就是它该在的地方"作为默认值，方便报错信息可读。
    printf '%s' "external/wiredtiger"
    return 1
}

# wt_find_bin <候选...> → 打印第一个可执行的；全都不行时打印第一个候选并返回 1。
wt_find_bin() {
    local cand
    for cand in "$@"; do
        if [ -x "$cand" ]; then
            printf '%s' "$cand"
            return 0
        fi
    done
    printf '%s' "${1:-}"
    return 1
}

# wt_howto —— 缺 wtperf / wt 时统一提示。
wt_howto() {
    echo "        → make wt-tools        # 浅克隆固定 tag + cmake 构建 wt / wtperf"
    echo "        → 或 WT_SRC=/path/to/wiredtiger 指向一个已构建的完整克隆"
}
