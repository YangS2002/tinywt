#!/usr/bin/env python3
"""把仓库里所有 UNIMPLEMENTED 按 Task 汇总 —— 也就是「你要写的代码」清单。

用法：
    make todo                    # 按 Px/Ty 分组，打印每个函数 + 文件:行
    make todo P=P2               # 只看 P2（等价 python3 scripts/todo.py -p P2）
    make todo c=1                # 只看每个 Task 还剩几个函数（-c）
    python3 scripts/todo.py -p P4/T4.3   # 精确到单个 Task

设计意图：
    这个仓库的边界是「数据结构 / 接口 / 测试 / harness 全给，函数体留白」。
    UNIMPLEMENTED("P3/T3.4 TxnSession::put") 里的标签就是边界本身：
    打标签的地方要你写，没打标签的地方不要动（改了会破坏 ABI 或测试契约）。
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# UNIMPLEMENTED("P3/T3.4 TxnSession::put") / UNIMPLEMENTED_RC(...) / UNIMPLEMENTED_VOID(...)
TAG = re.compile(r'UNIMPLEMENTED(?:_RC|_VOID)?\("(P(\d)/T(\d\.\d))\s+([^"]*)"')

TITLES = {
    "T0.1": "构建骨架 + 错误码契约（已给）",
    "T0.2": "varint(LEB128) + crc32",
    "T1.1": "块读写 + 校验",
    "T1.2": "extent 三表 + 超级块",
    "T1.3": "cell 编解码",
    "T2.1": "内存页三件套 + page_in",
    "T2.2": "descend 搜索",
    "T2.3": "插入 + 叶/内部分裂",
    "T2.4": "reconcile + eviction",
    "T2.5": "checkpoint / verify / 读可见性",
    "T2.6": "编码家族（suffix / overflow / dict）",
    "T2.7": "compressor API（RLE）",
    "T3.0": "错误码契约（已给）",
    "T3.1": "config parser + conn + catalog",
    "T3.2": "cursor 写路径",
    "T3.3": "cursor 读路径与遍历",
    "T3.4": "事务 API + modify/truncate",
    "T3.5": "WAL + 崩溃恢复",
    "T4.1": "并发 L1（tree 读写锁）",
    "T4.2": "WT_REF 状态机 + hazard pointer",
    "T4.3": "History Store",
    "T4.4": "未提交 reconcile + RTS",
    "T4.5": "watermark + 时间窗口聚合",
}

PROJ_TITLE = {
    "P0": "基础件",
    "P1": "块管理器 + cell",
    "P2": "B+Tree",
    "P3": "C ABI / cursor / 事务 / WAL",
    "P4": "并发 / MVCC",
}


def scan():
    """返回 [(proj, task, name, relpath, lineno), ...]"""
    items = []
    for sub in ("src", "src_c"):
        for f in sorted((ROOT / sub).rglob("*.cpp")):
            for lineno, line in enumerate(f.read_text(encoding="utf-8").splitlines(), 1):
                m = TAG.search(line)
                if m:
                    # group2 = 项目号（"4"），group3 = Task 号（"4.3"），group4 = 函数名
                    items.append(
                        ("P" + m.group(2), m.group(3), m.group(4).strip(),
                         str(f.relative_to(ROOT)), lineno)
                    )
    items.sort(key=lambda x: (x[0], x[3], x[4]))
    return items


def _norm(s):
    """P4/T4.3 -> (P4, T4.3)；p2 -> (P2, None)；T4.3 -> (None, T4.3)"""
    s = s.strip()
    if "/" in s:
        a, b = s.split("/", 1)
        return _norm(a)[0], _norm(b)[1]
    body = s[1:] if s[:1] in "PpTt" else s
    kind = s[:1].upper()
    if kind == "T":
        return None, "T" + body
    return "P" + body, None


def main(argv):
    only_count = any(a in ("-c", "--count") for a in argv)
    sel_proj = sel_task = None
    for i, a in enumerate(argv):
        if a in ("-p", "--project") and i + 1 < len(argv):
            sel_proj, sel_task = _norm(argv[i + 1])
    items = scan()
    if sel_proj or sel_task:
        # 支持 -p P2（整段）、-p P4/T4.3 或 -p T4.3（单 Task）
        items = [x for x in items
                 if (sel_proj is None or x[0] == sel_proj)
                 and (sel_task is None or "T" + x[1] == sel_task)]
        if not items:
            print(f"没有匹配 \"{argv[argv.index('-p') + 1]}\" 的 UNIMPLEMENTED。")
            return 0
    if not items:
        print("没有 UNIMPLEMENTED —— 全部写完了。")
        return 0

    # 按 (proj, task) 分组，保持 Task 顺序
    groups = {}
    for proj, task, name, path, lineno in items:
        groups.setdefault((proj, "T" + task), []).append((name, path, lineno))

    for (proj, task), rows in sorted(groups.items()):
        title = TITLES.get(task, "")
        print(f"\n{proj} / {task}  {title}")
        print(f"  要写 {len(rows)} 个函数")
        if not only_count:
            for name, path, lineno in rows:
                print(f"    {name:<34} {path}:{lineno}")

    print("\n" + "-" * 62)
    filtered = bool(sel_proj or sel_task)
    for proj in PROJ_TITLE:
        n = sum(len(r) for (p, _), r in groups.items() if p == proj)
        if filtered and n == 0:
            continue
        tasks = sorted(t for (p, t) in groups if p == proj)
        print(f"  {proj}  {n:>3} 个函数    {PROJ_TITLE[proj]}（{len(tasks)} 个 Task 未完成）")
    if filtered:
        print(f"  合计 {len(items)} 个函数（筛选后）")
    else:
        print(f"  合计 {len(items)} 个函数待实现（= 边界内需要你写的部分）")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
