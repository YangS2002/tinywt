#!/usr/bin/env python3
"""从真 WT 的 wiredtiger.in 生成 third_party/wiredtiger/include/wiredtiger.h（API 合同头）。

为什么自己生成而不是直接拷：
  WT 仓库里只有模板 src/include/wiredtiger.in，wiredtiger.h 是构建产物
  （cmake configure_file + auto.cmake 的探测结果决定那几个 @...@ 的值）。

坑（已核实）：
  auto.cmake 里定义的变量名是 `uintprt_t_decl`（少个 r），而模板引用的是
  `@uintptr_t_decl@`——名字对不上，所以这里显式补上，值取空（uintptr_t
  由 <stdint.h> 提供）。

用法：
  python3 scripts/gen_wiredtiger_h.py [wiredtiger.in 路径]
  默认 third_party/wt/src/include/wiredtiger.in
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_IN = os.path.join(ROOT, "third_party", "wt", "src", "include", "wiredtiger.in")
OUT = os.path.join(ROOT, "third_party", "wiredtiger", "include", "wiredtiger.h")

# 与 RELEASE_INFO / auto.cmake 的 Linux 探测结果一致
SUBS = {
    "VERSION_MAJOR": "11",
    "VERSION_MINOR": "3",
    "VERSION_PATCH": "0",
    "VERSION_STRING": '"11.3.0"',
    "wiredtiger_includes_decl": "\n".join(
        [
            "#include <sys/types.h>",
            "#include <inttypes.h>",
            "#include <stdarg.h>",
            "#include <stdbool.h>",
            "#include <stdint.h>",
            "#include <stdio.h>",
        ]
    ),
    "off_t_decl": "typedef off_t wt_off_t;",
    "uintmax_t_decl": "",   # <stdint.h> 已提供
    "uintptr_t_decl": "",   # 见文件头：cmake 侧变量名 typo
}


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_IN
    if not os.path.exists(src):
        print(f"找不到模板：{src}\n先跑 scripts/fetch_wt_sources.sh 或手动指定路径", file=sys.stderr)
        return 2

    with open(src, "r", encoding="utf-8") as f:
        text = f.read()

    out = re.sub(r"@([A-Za-z0-9_]+)@", lambda m: SUBS.get(m.group(1), m.group(0)), text)

    leftovers = sorted(set(re.findall(r"@([A-Za-z0-9_]+)@", out)))
    if leftovers:
        print(f"仍有未替换的占位符：{leftovers}", file=sys.stderr)
        return 3

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    header = (
        "/*-\n"
        " * wiredtiger.h —— 由 scripts/gen_wiredtiger_h.py 生成，不要手改。\n"
        " * 源：wiredtiger/wiredtiger src/include/wiredtiger.in（WT 11.3.x）\n"
        f" * 生成命令：python3 scripts/gen_wiredtiger_h.py {os.path.relpath(src, ROOT)}\n"
        " *\n"
        " * 这是 tinywt 的 C ABI 合同：只实现被测试工具引用到的符号，\n"
        " * 缺什么链接器就报什么（见 test/contract/abi_probe.c）。\n"
        " */\n"
    )
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(header + out)

    n = out.count("\n") + 1
    print(f"生成 {os.path.relpath(OUT, ROOT)}（{n} 行）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
