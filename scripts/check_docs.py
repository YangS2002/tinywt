#!/usr/bin/env python3
"""对账：README.md / docs/PROJECTS.md / docs/projects/*.md 里写死的数字、路径、命令，
与仓库实际情况逐条比对。只做机械核对，不判断语义。
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
README = ROOT / "README.md"
MASTER = ROOT / "docs/PROJECTS.md"
PROJ_DOCS = {p.stem[:2].upper(): p for p in sorted((ROOT / "docs/projects").glob("*.md"))}
ALL_DOCS = [README, MASTER, *PROJ_DOCS.values()]

problems, checked = [], []


def ok(m):
    checked.append(("OK  ", m))


def bad(m):
    problems.append(m)


# ---------------------------------------------------------------- 实际值
TAG = re.compile(r'UNIMPLEMENTED(?:_RC|_VOID)?\("(P(\d)/T(\d+\.\d+))')
RES = re.compile(r"^RESULT\s+P(\d)\t([^\t]+)\t(\w+)")
TASKNAME = re.compile(r"P\d/T(\d+\.\d+)")

funcs, items, passes = {}, {}, {}
for f in (ROOT / "src").rglob("*.cpp"):
    for line in f.read_text(encoding="utf-8").splitlines():
        m = TAG.search(line)
        if m:
            key = ("P" + m.group(2), "T" + m.group(3))
            funcs[key] = funcs.get(key, 0) + 1

for b in sorted((ROOT / "build/bin").glob("*_test")):
    out = subprocess.run([str(b)], capture_output=True, timeout=600).stdout.decode("utf-8", "replace")
    for line in out.splitlines():
        m = RES.match(line)
        if not m:
            continue
        name = m.group(2)
        t = TASKNAME.search(name)
        if not t:
            bad(f"{b.name}: 测例名缺 Px/Ty 前缀 → {name}")
            continue
        key = ("P" + m.group(1), "T" + t.group(1))
        items[key] = items.get(key, 0) + 1
        if m.group(3) == "PASS":
            passes[key] = passes.get(key, 0) + 1

TOT_F, TOT_I = sum(funcs.values()), sum(items.values())
PROJS = ("P0", "P1", "P2", "P3", "P4")
PF = {p: sum(v for (q, _), v in funcs.items() if q == p) for p in PROJS}
PI = {p: sum(v for (q, _), v in items.items() if q == p) for p in PROJS}
PP = {p: sum(v for (q, _), v in passes.items() if q == p) for p in PROJS}
ok(f"实测 {TOT_F} 个函数 / {TOT_I} 个评分项；"
   + " ".join(f"{p} {PP[p]}/{PI[p]}({PF[p]}函数)" for p in PROJS))

readme = README.read_text(encoding="utf-8")
master = MASTER.read_text(encoding="utf-8")

# ---------------------------------------------------------------- 1. 总数
for doc, txt, label in ((README, readme, "README"), (MASTER, master, "PROJECTS")):
    m = re.search(r"(\d+)\s*个(?:评分项|测试项)", txt)
    if not m:
        bad(f"{label}: 没写「N 个评分项」")
    else:
        (ok if int(m.group(1)) == TOT_I else bad)(
            f"{label} 声称 {m.group(1)} 个测试项 / 实测 {TOT_I}")
    m = re.search(r"(\d+)\s*个函数", txt)
    if not m:
        bad(f"{label}: 没写「N 个函数」")
    else:
        (ok if int(m.group(1)) == TOT_F else bad)(
            f"{label} 声称 {m.group(1)} 个函数 / 实测 {TOT_F}")

# ---------------------------------------------------------------- 2. README 分数块
for m in re.finditer(r"^(P\d)\s+(\d+)/(\d+)", readme, re.M):
    p, a, b = m.group(1), int(m.group(2)), int(m.group(3))
    if (a, b) != (PP[p], PI[p]):
        bad(f"README 分数块 {p} {a}/{b}，实际 {PP[p]}/{PI[p]}")
    else:
        ok(f"README 分数块 {p} {a}/{b}")
m = re.search(r"TOTAL\s+(\d+)/(\d+)", readme)
if m:
    a, b = int(m.group(1)), int(m.group(2))
    expect = (sum(PP.values()), TOT_I)
    if (a, b) != expect:
        bad(f"README TOTAL {a}/{b}，实际 {expect[0]}/{expect[1]}")
    else:
        ok(f"README TOTAL {a}/{b}")

# ---------------------------------------------------------------- 3. §2 项目地图
for m in re.finditer(
        r"^\|\s*\*\*(P\d)\*\*[^|]*\|\s*T\d\.\d–T\d\.\d\s*\|\s*(\d+)\s*\|\s*(\d+)\s*\|",
        master, re.M):
    p, i, f = m.group(1), int(m.group(2)), int(m.group(3))
    if (i, f) != (PI[p], PF[p]):
        bad(f"§2 项目地图 {p}: 文档 {i} 项 / {f} 函数，实际 {PI[p]} / {PF[p]}")
    else:
        ok(f"§2 项目地图 {p}: {i} 项 / {f} 函数")

# ---------------------------------------------------------------- 4. §3 逐任务表
row = re.compile(r"^\|\s*(T\d\.\d+)[^|]*\|[^|]*\|\s*(\d+)[^|]*\|\s*(\d+)[^|]*\|")
proj, seen_rows = None, 0
for line in master.splitlines():
    h = re.match(r"^###\s+(P\d)\b", line)
    if h:
        proj = h.group(1)
    m = row.match(line)
    if not (m and proj):
        continue
    t, df, di = m.group(1), int(m.group(2)), int(m.group(3))
    seen_rows += 1
    af, ai = funcs.get((proj, t), 0), items.get((proj, t), 0)
    if (df, di) != (af, ai):
        bad(f"§3 {proj}/{t}: 文档 {df} 函数 / {di} 项，实际 {af} / {ai}")
    else:
        ok(f"§3 {proj}/{t}: {df} 函数 / {di} 项")
if seen_rows != 24:
    bad(f"§3 只解析到 {seen_rows} 行任务（应为 24）")

# ---------------------------------------------------------------- 5. 各项目文档
for p, path in PROJ_DOCS.items():
    if p == "CA":
        continue
    txt = path.read_text(encoding="utf-8")
    m = re.search(r"验收：(\d+) 个测试项", txt)
    if not m:
        bad(f"{path.name}: 没写「验收：N 个测试项」")
    elif int(m.group(1)) != PI[p]:
        bad(f"{path.name}: 验收 {m.group(1)} 项，实际 {PI[p]}")
    else:
        ok(f"{path.name}: 验收 {m.group(1)} 项")

# 项目文档 H1 名字与总纲 §2 一致
names = dict(re.findall(r"^\|\s*\*\*(P\d)\*\*\s*([^|]+?)\s*\|", master, re.M))
for p, path in PROJ_DOCS.items():
    if p not in names:
        continue
    h1 = re.search(r"^#\s+(P\d)\s+任务书\s+——\s*(.+)$", path.read_text(encoding="utf-8"), re.M)
    if not h1:
        bad(f"{path.name}: H1 不是「Pn 任务书 —— 名称」")
        continue
    if h1.group(2).strip() != names[p].strip():
        bad(f"{path.name}: H1「{h1.group(2)}」与总纲「{names[p]}」不一致")
    else:
        ok(f"{path.name}: 标题与总纲一致（{names[p]}）")

# ---------------------------------------------------------------- 6. 路径与链接
WT_ONLY = ("src/include/", "src/block/", "src/cell/", "src/reconcile/", "src/history/",
           "src/support/", "src/utilities/", "src/conn/", "src/session/", "src/cursor/",
           "src/btree/bt_", "src/log/", "src/txn/txn.c", "src/txn/txn_rollback", "src/txn/txn_ckpt",
           "src/checksum/", "src/compress/", "src/config/", "src/meta/", "src/lsm/",
           "test/format/", "test/suite/", "test/utility/", "test/pN/", "bench/",
           "src_c/", "ext/compressors/")
TICK = re.compile(r"`([^`\n]+)`")
SUFFIX = re.compile(r":\d+(?:[,\-]\d+)*$")
BRACE = re.compile(r"\{([^{}]+)\}")


def expand(p):
    out = [p]
    while True:
        nxt, hit = [], False
        for s in out:
            m = BRACE.search(s)
            if not m:
                nxt.append(s)
                continue
            hit = True
            nxt += [s[:m.start()] + a.strip() + s[m.end():] for a in m.group(1).split(",")]
        out = nxt
        if not hit:
            return out


seen = set()
for d in ALL_DOCS:
    for line in d.read_text(encoding="utf-8").splitlines():
        for raw in TICK.findall(line):
            c = SUFFIX.sub("", raw.strip())
            if not re.match(r"^(src|include|test|scripts|docs|third_party)/", c):
                continue
            if c.startswith(WT_ONLY) or c in seen:
                continue
            for p in expand(c):
                seen.add(p)
                if any(ch in p for ch in "*?<>|"):
                    continue
                if not (ROOT / p).exists():
                    bad(f"{d.name}: 路径不存在 → {p}")
ok(f"tinywt 自身路径引用 {len(seen)} 个（真 WT 源码路径已排除）")

nlink = 0
for d in ALL_DOCS:
    for target, _a in re.findall(r"\]\(([^)#\s]+)(#[^)]*)?\)", d.read_text(encoding="utf-8")):
        if target.startswith("http"):
            continue
        nlink += 1
        if not (d.parent / target).resolve().exists():
            bad(f"{d.name}: 链接失效 → {target}")
ok(f"Markdown 链接 {nlink} 个")

# ---------------------------------------------------------------- 7. 构建系统
mk = (ROOT / "Makefile").read_text(encoding="utf-8")
targets = set(re.findall(r"^([a-z0-9%-]+):", mk, re.M))
for need in ("build", "test", "grade", "todo", "probe", "probe-link", "shared",
             "cp1", "cp2", "capstone", "asan", "tsan", "clean", "p%-test",
             "wt-tools", "wt-abi"):
    ok(f"make {need}") if need in targets else bad(f"Makefile 缺 make {need}")

# ---------------------------------------------------------------- 8. 结构与声明
page = (ROOT / "include/tinywt/storage/page.h").read_text(encoding="utf-8")
bt = (ROOT / "include/tinywt/btree/btree.h").read_text(encoding="utf-8")
hs_h = (ROOT / "include/tinywt/txn/history_store.h").read_text(encoding="utf-8")
hs_c = (ROOT / "src/txn/history_store.cpp").read_text(encoding="utf-8")
comp = (ROOT / "include/tinywt/compression/compress.h").read_text(encoding="utf-8")

for name, cond in [
    ("page.h：Entry::upd + entries + ins_head", all(
        s in page for s in ("Update *upd", "std::vector<Entry> entries", "Insert *ins_head"))),
    ("btree.h：reconcile / evict / checkpoint", all(
        s in bt for s in ("reconcile_page", "evict_one", "Status checkpoint"))),
    ("btree.h：HistoryReader 钩子", "class HistoryReader" in bt and "set_history_reader" in bt),
    ("hs：oldest = min(stable, pinned)", "stable_ts < pinned_ts" in hs_h),
    ("hs：std::map 实现（声明的简化）", "std::map" in hs_c or "std::map" in hs_h),
    ("compression：只有 NONE / RLE", "NONE = 0" in comp and "RLE = 1" in comp
        and "SNAPPY" not in comp and "ZLIB" not in comp),
    ("真 wiredtiger.h 在 third_party", (ROOT / "third_party/wiredtiger/include/wiredtiger.h").exists()),
    ("ABI 探针在 test/contract", (ROOT / "test/contract/abi_probe.c").exists()),
    ("测试按 p0..p4 分目录", all((ROOT / f"test/p{i}").is_dir() for i in range(5))),
    # 工具面 ABI 的"34 个"是文档里反复引用的数字，必须和代码、构建两处都对得上。
    # 少一个 = 真 wt / wtperf 会 undefined symbol；多了 = 文档说谎。
    ("工具面 ABI：34 个 T3.6 函数", len(re.findall(
        r'UNIMPLEMENTED(?:_RC|_VOID)?\("P3/T3\.6 ',
        (ROOT / "src/api/wt_tool_abi.cpp").read_text(encoding="utf-8"))) == 34),
    ("CMake 编入 wt_tool_abi.cpp", "src/api/wt_tool_abi.cpp" in
        (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")),
]:
    ok(name) if cond else bad(name)

# ---------------------------------------------------------------- 输出
print("=" * 78)
for tag, m in checked:
    print(f"  {tag} {m}")
print("=" * 78)
if problems:
    print(f"\n{len(problems)} 处不一致：\n")
    for p in problems:
        print(f"  - {p}")
    sys.exit(1)
print("\n文档与仓库一致。")
