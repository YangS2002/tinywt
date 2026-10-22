#!/usr/bin/env python3
"""
scripts/wt_abi.py —— 量出「真工具 vs 我们的 .so」之间还缺哪些符号

    make wt-abi

不猜、不数记忆里的"23 个"：直接拿真 wt / wtperf 的**未定义符号**，
减去 build/libwiredtiger.so 的**导出符号**，差集就是缺口。

判定口径：
  - 只看 wiredtiger_* 与 __wt_*（工具私有的 util_* 由工具自己提供）
  - 版本化符号（foo@GLIBC_2.2.5）去掉版本后缀再比

退出码：0 = 符号齐了**且**没有空桩；1 = 还有缺口 / 还有空桩（方便进 CI 当进度门）。
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SO = os.path.join(ROOT, "build", "libwiredtiger.so")


def syms(path, defined):
    flag = "--defined-only" if defined else "--undefined-only"
    out = subprocess.run(["nm", "-D", flag, path], capture_output=True, text=True)
    if out.returncode != 0:
        return set()
    res = set()
    for line in out.stdout.splitlines():
        parts = line.split()
        if not parts:
            continue
        name = parts[-1]
        name = name.split("@")[0]  # 去掉 @GLIBC_2.2.5 之类
        if name.startswith("wiredtiger_") or name.startswith("__wt_"):
            res.add(name)
    return res


def main():
    argv = sys.argv[1:]
    if not argv:
        # 默认就量 standard 位置的两个真工具（make wt-tools 的产物）
        src = os.environ.get("WT_SRC", os.path.join(ROOT, "external", "wiredtiger"))
        argv = [
            os.path.join(src, "build", "wt"),
            os.path.join(src, "build", "bench", "wtperf", "wtperf"),
        ]

    have = syms(SO, defined=True) if os.path.exists(SO) else set()
    if not os.path.exists(SO):
        print(f"! 没有 {SO} —— 先 make shared", file=sys.stderr)

    print(f"我们的导出：{len(have)} 个 wiredtiger_* / __wt_* 符号  ({SO})")
    print()

    total_gap = set()
    for binary in argv:
        if not os.path.exists(binary):
            print(f"! 跳过不存在的 {binary}")
            continue
        need = syms(binary, defined=False)
        gap = sorted(need - have)
        total_gap |= set(gap)
        name = os.path.basename(binary)
        print(f"== {name}: 需要 {len(need)} 个，缺 {len(gap)} 个")
        for s in gap:
            print(f"   {s}")
        print()

    if total_gap:
        print(f"合计缺口：{len(total_gap)} 个独立符号 —— LD_PRELOAD 会当场 undefined symbol")
        return 1

    # 缺口 0 只代表"动态链接器肯加载"，不代表功能对。
    # 空桩也是导出的符号，所以这里再报一次"还有几个是空桩"。
    stub_src = os.path.join(ROOT, "src", "api", "wt_tool_abi.cpp")
    stubs = 0
    if os.path.exists(stub_src):
        with open(stub_src, encoding="utf-8", errors="replace") as f:
            stubs = sum(1 for line in f if "UNIMPLEMENTED_" in line)

    print("符号层：缺口 0 —— 动态链接器肯加载了")
    if stubs:
        print(f"功能层：还有 {stubs} 个是空桩（返回 0 / nullptr / WT_ERROR，都不是真结果）")
        print("        进度看 make capstone 的 C1/C2，不看这个数字")
        return 1

    print("功能层：没剩空桩 —— C1/C2 可以当真验收了")
    return 0


if __name__ == "__main__":
    sys.exit(main())
