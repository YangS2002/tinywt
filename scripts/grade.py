#!/usr/bin/env python3
"""tinywt 逐项打分器。

用法：make grade  （或 python3 scripts/grade.py build/p0_defs_test ...）

每个测试二进制会打印形如
    BEGIN  <project>\t<test>                  ← 每一项**开跑前**先报一声
    RESULT <project>\t<test>\t<status>\t<pass>\t<fail>\t<ms>
    SCORE  <project>\t<ok>\t<total>
的行；本脚本只做汇总，不重新解释测试语义。

状态含义：
    PASS    该 Task 完成
    FAIL    有断言失败（写错了）
    NOTIMPL 还没写（命中 UNIMPLEMENTED）
    ERROR   抛了别的异常 / 零断言

★ 分母从哪来（这条最重要）
    分母用**文档里声称的总数**（README 的「N 个评分项」），不是"实际解析到几行"。
    因为后者会被三种情况悄悄缩小，而且缩得越狠百分比越好看：
      - 测试二进制没编出来（`ls build/bin/*_test` 直接少一个，glob 不报错）
      - 二进制崩溃（RESULT 是最后统一打印的，崩了就一行都没有）
      - 二进制超时
    分母固定之后，缺的项会被点名，分数也不会虚高。
"""

import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = (ROOT / "README.md", ROOT / "docs/PROJECTS.md")

PROJECT_TITLE = {
    "P0": "编码件与错误码（varint / crc32 / 状态码契约）",
    "P1": "磁盘格式：块 / extent / cell",
    "P2": "页与写路径：内存页 / reconcile / eviction / checkpoint / 编码 / 压缩",
    "P3": "对外 ABI：cursor / 事务 / 日志",
    "P4": "版本、时间与并发：WT_REF / hazard / HS / RTS / watermark",
}

STATUS_MARK = {"PASS": "\033[32m PASS \033[0m", "FAIL": "\033[31m FAIL \033[0m",
               "NOTIMPL": "\033[33m TODO \033[0m", "ERROR": "\033[35m ERR  \033[0m",
               "MISS": "\033[35m MISS \033[0m",
               "TIMEOUT": "\033[35m TMO  \033[0m"}


def status_mark(status):
    """CRASH(SEGV) 这类带后缀的状态也要有颜色，别原样裸奔。"""
    if status.startswith("CRASH"):
        return "\033[31m CRASH \033[0m"
    return STATUS_MARK.get(status, status)


def expected_items():
    """文档里声称的评分项总数 —— 分母的唯一来源。找不到就当 0（随后回退到实测）。"""
    for p in DOCS:
        if not p.exists():
            continue
        m = re.search(r"(\d+)\s*个评分项", p.read_text(encoding="utf-8"))
        if m:
            return int(m.group(1)), p.name
    return 0, ""


def main(argv):
    bins = argv[1:]
    if not bins:
        print("no test binaries", file=sys.stderr)
        return 2

    rows, missing = [], []
    for b in bins:
        base = os.path.basename(b)
        if not os.path.exists(b):
            # 不要 continue 了事：那样这个项目的项会整片消失，分母跟着变小。
            missing.append((base, "测试二进制不存在（先 make build）"))
            continue

        # 未实现的测试会以非零退出，这里不关心退出码，只看输出里的 BEGIN/RESULT。
        # 输出里可能混有二进制字节（例如 %.*s 打印了非法 UTF-8 的 key），
        # 所以自己解码并容错，不能用 text=True。
        try:
            raw = subprocess.run([b], capture_output=True, timeout=1800).stdout
        except subprocess.TimeoutExpired:
            missing.append((base, "超时（1800s），整份结果作废"))
            continue
        out = raw.decode("utf-8", errors="replace")

        began, nres = [], 0
        for line in out.splitlines():
            if line.startswith("BEGIN "):
                parts = line[len("BEGIN "):].split("\t")
                began.append((parts[0], parts[-1]))
                continue
            if not line.startswith("RESULT "):
                continue
            parts = line[len("RESULT "):].split("\t")
            if len(parts) < 5:
                continue
            proj, name, status, npass, nfail = parts[0], parts[1], parts[2], parts[3], parts[4]
            rows.append((proj, name, status, int(npass), int(nfail)))
            nres += 1

        if not began and nres == 0:
            missing.append((base, "没有输出任何 BEGIN/RESULT（启动就挂了？）"))
            continue

        # BEGIN 和 RESULT 按顺序一一对应（每项跑完立刻打 RESULT）。
        # 对不上的第一项 = 进程死在它身上；再后面的 = 还没轮到就退出了。
        # ★ 只能按**位置**对，不能按名字对：BEGIN 打的是函数名（CASE 还没执行），
        #   RESULT 打的是 CASE 里的名字，两者本来就不一样。
        rest = began[nres:]
        if rest:
            missing.append((base, f"进程在第 {nres + 1} 项（{rest[0][1]}）退出，"
                                  f"后面还有 {len(rest) - 1} 项没轮到"))
            for i, (proj, name) in enumerate(rest):
                rows.append((proj, f"{name}{'（崩溃）' if i == 0 else '（未跑到）'}", "MISS", 0, 0))

    if not rows and not missing:
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
        mark = status_mark(status)
        print(f"{proj:<5}{mark}{npass:>6}{nfail:>6}  {name}")
    print("=" * 92)

    # 按 project 汇总
    agg = {}
    for proj, _name, status, _p, _f in rows:
        ok, total = agg.get(proj, (0, 0))
        agg[proj] = (ok + (1 if status == "PASS" else 0), total + 1)

    exp, src = expected_items()
    grand_ok = sum(v[0] for v in agg.values())
    grand_total = sum(v[1] for v in agg.values())
    denom = max(exp, grand_total)  # 文档说法 > 实测时，用文档的（缺的项已经在上面点名）

    print()
    print(f"{'project':<8}{'score':>10}   {'title'}")
    print("-" * 92)
    for proj in sorted(agg):
        ok, total = agg[proj]
        pct = 100.0 * ok / total if total else 0.0
        print(f"{proj:<8}{ok:>4}/{total:<5} {pct:5.1f}%   {PROJECT_TITLE.get(proj, '')}")
    print("-" * 92)
    pct = 100.0 * grand_ok / denom if denom else 0.0
    tail = "" if denom == grand_total else f"  （只跑到 {grand_total} 项）"
    print(f"{'TOTAL':<8}{grand_ok:>4}/{denom:<5} {pct:5.1f}%{tail}")
    print()

    if missing:
        print(f"!! 有 {len(missing)} 处没跑完 —— 这些项不是「通过」，是「没测」：")
        for who, why in missing:
            print(f"     {who}: {why}")
        if denom != grand_total:
            print(f"   分母按文档的 {denom} 算（{src}）；实测只收到 {grand_total} 行，"
                  f"差额就是上面这些。")
        print()

    # 未完成清单，直接抄进 TODO
    todo = [(p, n) for p, n, s, _a, _b in rows if s in ("NOTIMPL", "FAIL", "ERROR", "MISS")]
    if todo:
        print("未通过（%d 项）：" % len(todo))
        for p, n in todo:
            print(f"  - {p}  {n}")
        print()

    # 缺项（没跑到的）说明这个数字不可信，退出码要能反映出来；
    # 只是"还没实现"（NOTIMPL）属于正常开发状态，不算错。
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
