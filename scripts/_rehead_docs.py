"""一次性：把各任务书开头的「对标 BusTub」换成 WT 的机制定位。跑完删掉。"""

import pathlib

P0 = ("""# P0 任务书 —— 基础件热身

> 对标 BusTub P0（C++ Primer）。BusTub 用 Trie 热身，tinywt 用编码件热身。
> 总纲与评分见 [PROJECTS.md](../PROJECTS.md)。验收：3 个测试项。""",
      """# P0 任务书 —— 编码件与错误码

> WT 的最底层：状态码体系（`src/include/error.h`）、变长整数（`intpack_inline.h`）、
> 校验和（`src/checksum/`）。这三样决定了后面所有磁盘格式怎么落字节 ——
> LEB128 写错一个续位，P2 的 descend 会以「键找不到」的形式失败，很难倒查。
> 总纲与评分见 [PROJECTS.md](../PROJECTS.md)。验收：3 个测试项。""")

P1 = ("""# P1 任务书 —— Block Manager + cell 编解码

> 对标 BusTub P1（Buffer Pool Manager 的"独立可测组件"思路）：
> 先把块设备和一个记录格式单独造对，再往上面放 B-Tree。
> 总纲与评分见 [PROJECTS.md](../PROJECTS.md)。验收：3 个测试项。""",
      """# P1 任务书 —— 磁盘格式：块、extent、cell

> WT 的磁盘格式层（`src/block/` + `src/cell/`）。这一层回答「一个 `.wt` 文件长什么样」：
> 块怎么定位（addr cookie）、空间怎么回收（extent 三表）、一条记录怎么落字节（cell）。
> 注意它和内存页是两回事 —— B+Tree 的逻辑页到 P2 才出现，这里只有块和字节。
> 总纲与评分见 [PROJECTS.md](../PROJECTS.md)。验收：3 个测试项。""")

P2 = ("""# P2 任务书 —— Row-Store B-Tree

> 对标 BusTub P2（B+Tree Index）。这是整个项目最重的一个 Project：
> 骨架里一半的 `UNIMPLEMENTED` 都在这。
> 总纲与评分见 [PROJECTS.md](../PROJECTS.md)。验收：13 个测试项。""",
      """# P2 任务书 —— 页与写路径

> WT 的核心机制（`src/btree/` + `src/reconcile/`）：内存页 = 磁盘镜像解出的 `entries`
> + 每键一条 update 链 + 新键的 insert 跳表；写不原地改，reconcile 把三者归并成新镜像。
> 这是整个项目最重的一段（约四分之一的待写函数）。
> 总纲与评分见 [PROJECTS.md](../PROJECTS.md)。验收：13 个测试项。
>
> **与原顺序的差异**：WT 里 reconcile / eviction / checkpoint 是一体的
> （`rec_write.c` 与 `bt_evict.c`、`txn_ckpt.c` 互相调用）；这里拆成 T2.4 与 T2.5，
> 只是为了能分别跑测试。""")

P3 = ("""# P3 任务书 —— 数据操作层：C ABI / cursor / 事务 / WAL

> 对标 BusTub P3（Query Execution）。总纲与评分见 [PROJECTS.md](../PROJECTS.md)。""",
      """# P3 任务书 —— 对外 ABI：cursor / 事务 / 日志

> WT 的对外形态：C 方法表（`src/conn/`、`src/session/`、`src/cursor/`）、
> 配置解析（`src/config/`）、catalog（`src/meta/`）、事务（`src/txn/`）、日志（`src/log/`）。
> 这一层的验收标准不是单测全绿，而是「真 `wtperf` 能不能链上并跑起来」。
> 总纲与评分见 [PROJECTS.md](../PROJECTS.md)。""")

P3_AFTER = ("""> | T3.5 | WAL + 崩溃恢复 | 9 | 6 |""",
            """> | T3.5 | WAL + 崩溃恢复 | 9 | 6 |
>
> **与原顺序的差异**：WT 里 log 与 checkpoint 同属持久化（LSN 互相对齐）；这里把
> T3.5 放在 P3 末尾，是因为要先有完整的 C ABI 链路，`wtperf` 才跑得起来。""")

P4 = ("""# P4 任务书 —— 并发 / MVCC / History Store / RTS / Watermark

> 对标 BusTub P4（MVCC）。总纲与评分见 [PROJECTS.md](../PROJECTS.md)。""",
      """# P4 任务书 —— 版本、时间与并发

> WT 的版本与并发（`src/history/` + `src/txn/txn_rollback_to_stable.c` +
> `src/support/hazard.c`）：旧版本什么时候搬进 History Store、旧快照怎么下探、
> 崩溃后怎么按 stable 回滚、读路径凭什么不持锁还不悬空。
> 总纲与评分见 [PROJECTS.md](../PROJECTS.md)。""")

P4_AFTER = ("""> **注意**：事务 API 与 WAL 在 P3（T3.4 / T3.5）。P4 建立在它们之上：""",
            """> 别把这层当成「MVCC 作业」：难点是**三层时间戳**（stable / pinned / oldest）
> 与**旧版本落盘**，版本链本身反而是最简单的部分。
>
> **注意**：事务 API 与 WAL 在 P3（T3.4 / T3.5）。P4 建立在它们之上：""")

CAP = ("""# Capstone —— 结业全量验收（C1–C4）

> 15-445 只有四个 Project，没有第五个作业。Capstone 是它的 leaderboard 文化的
> 自然延伸：**单测证明语义对，Capstone 证明能顶住真工具的用法。**""",
       """# Capstone —— 结业全量验收（C1–C4）

> 单测只能证明语义对；Capstone 证明这套实现能顶住**真工具的用法**：
> 真 `wtperf` 直接 `LD_PRELOAD` 你的 `.so`，真 `wt` 工具打开你写的库。""")

JOBS = [
    ("docs/projects/p0_foundation.md", [P0]),
    ("docs/projects/p1_block_manager.md", [P1]),
    ("docs/projects/p2_btree.md", [P2]),
    ("docs/projects/p3_api_txn_wal.md", [P3, P3_AFTER]),
    ("docs/projects/p4_concurrency_mvcc.md", [P4, P4_AFTER]),
    ("docs/projects/capstone.md", [CAP]),
]

for path, pairs in JOBS:
    p = pathlib.Path(path)
    s = p.read_text(encoding="utf-8")
    for a, b in pairs:
        if a not in s:
            print(f"  MISS {path}: {a.splitlines()[0][:40]}")
            continue
        s = s.replace(a, b, 1)
    p.write_text(s, encoding="utf-8")
    print(f"  ok   {path}")
