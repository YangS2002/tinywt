#!/usr/bin/env python3
"""tinywt 逐项打分器。

用法：make grade  （或 python3 scripts/grade.py build/p0_defs_test ...）

每个测试二进制会打印形如
    RESULT <project>\t<test>\t<status>\t<pass>\t<fail>\t<ms>
    SCORE  <project>\t<ok>\t<total>
的行；本脚本只做汇总，不重新解释测试语义。

状态含义：
    PASS    该 Task 完成
    FAIL    有断言失败（写错了）
    NOTIMPL 还没写（命中 UNIMPLEMENTED）
    ERROR   抛了别的异常 / 零断言
"""

import os
import subprocess
import sys

PROJECT_TITLE = {
    "P0": "编码件与错误码（varint / crc32 / 状态码契约）",
    "P1": "磁盘格式：块 / extent / cell",
    "P2": "页与写路径：内存页 / reconcile / eviction / checkpoint / 编码 / 压缩",
    "P3": "对外 ABI：cursor / 事务 / 日志",
    "P4": "版本、时间与并发：WT_REF / hazard / HS / RTS / watermark",
}

STATUS_MARK = {"PASS": "\033[32m PASS \033[0m", "FAIL": "\033[31m FAIL \033[0m",
               "NOTIMPL": "\033[33m TODO \033[0m", "ERROR": "\033[35m ERR  \033[0m"}


def main(argv):
    bins = argv[1:]
    if not bins:
        print("no test binaries", file=sys.stderr)
        return 2

    rows = []
    for b in bins:
        if not os.path.exists(b):
            continue
        # 未实现的测试会以非零退出，这里不关心退出码，只看输出里的 RESULT 行。
        # 输出里可能混有二进制字节（例如 %.*s 打印了非法 UTF-8 的 key），
        # 所以自己解码并容错，不能用 text=True。
        try:
            raw = subprocess.run([b], capture_output=True, timeout=1800).stdout
        except subprocess.TimeoutExpired:
            print(f"  !! {b} 超时（1800s）", file=sys.stderr)
            continue
        out = raw.decode("utf-8", errors="replace")
        for line in out.splitlines():
            if not line.startswith("RESULT "):
                continue
            parts = line[len("RESULT "):].split("\t")
            if len(parts) < 5:
                continue
            proj, name, status, npass, nfail = parts[0], parts[1], parts[2], parts[3], parts[4]
            rows.append((proj, name, status, int(npass), int(nfail)))

    if not rows:
        print("没有解析到任何 RESULT 行 —— 检查 harness.hpp 是否被正确 include", file=sys.stderr)
        return 2

    rows.sort(key=lambda r: (r[0], r[1]))

    print()
    print("=" * 92)
    print(f"{'proj':<5}{'status':<8}{'pass':>6}{'fail':>6}  test")
    print("=" * 92)
    last_proj = None
    for proj, name, status, npass, nfail in rows:
        if last_proj is not None and proj != last_proj:
            print("-" * 92)
        last_proj = proj
        mark = STATUS_MARK.get(status, status)
        print(f"{proj:<5}{mark}{npass:>6}{nfail:>6}  {name}")
    print("=" * 92)

    # 按 project 汇总
    agg = {}
    for proj, _name, status, _p, _f in rows:
        ok, total = agg.get(proj, (0, 0))
        agg[proj] = (ok + (1 if status == "PASS" else 0), total + 1)

    print()
    print(f"{'project':<8}{'score':>10}   {'title'}")
    print("-" * 92)
    grand_ok = grand_total = 0
    for proj in sorted(agg):
        ok, total = agg[proj]
        grand_ok += ok
        grand_total += total
        pct = 100.0 * ok / total if total else 0.0
        print(f"{proj:<8}{ok:>4}/{total:<5} {pct:5.1f}%   {PROJECT_TITLE.get(proj, '')}")
    print("-" * 92)
    pct = 100.0 * grand_ok / grand_total if grand_total else 0.0
    print(f"{'TOTAL':<8}{grand_ok:>4}/{grand_total:<5} {pct:5.1f}%")
    print()

    # 未完成清单，直接抄进 TODO
    todo = [(p, n) for p, n, s, _a, _b in rows if s in ("NOTIMPL", "FAIL", "ERROR")]
    if todo:
        print("未通过（%d 项）：" % len(todo))
        for p, n in todo:
            print(f"  - {p}  {n}")
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
