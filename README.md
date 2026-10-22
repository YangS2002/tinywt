# tinywt —— 用重写 WiredTiger 骨架来学存储引擎

一个 **CMU 15-445 式的存储引擎作业集**：仓库里是框架（头文件、数据结构、
测试、任务书），实现全部留空。你要做的是把 `UNIMPLEMENTED(...)` 换成真正的代码，
最后让 **WiredTiger 官方工具 wtperf 直接链接** `libtinywt.a` 跑起来。

- 语言：C++17（内核）+ C ABI（对外）
- 结构：**内存页 ≠ 磁盘页**，update 链 + insert 跳表 + reconcile，和真 WT 一一对应
- 规模：5 个 Project / 49 个测试项 + 1 个 Capstone
- 契约头：`compat/wiredtiger.h` 是**真** WT 11.3.x 的头文件，不是仿写的

---

## 快速开始

```bash
make todo         # 我这段要写哪些函数？（按 Task 分组，带 file:line）
make grade        # 逐项打分（未实现记 TODO，不中断）
make test         # 跑全部测试，任一不过就非零退出
make p2-test      # 只跑 P2
make probe        # 编译期契约：错误码数值 + 头文件可独立编译
make probe-link   # 链接期契约：缺哪个接口，链接器就报哪个符号
make cp1 / cp2    # Checkpoint gate（过了才准进下一段）
make shared       # build/libwiredtiger.so（给真 wtperf / wt 做 drop-in）
make capstone     # C1–C4 结业验收
make asan / tsan  # sanitizer 构建（ASan 抓越界，TSan 抓数据竞争；跑完 make clean）
```

骨架初始状态：

```
project      score   title
P0         1/3      33.3%   基础件（varint / crc32 / 错误码契约）
P1         0/3       0.0%   块管理器 + cell 编解码
P2         0/13      0.0%   Buffer Pool + B+Tree（含 codec / 压缩）
P3         2/23      8.7%   C ABI + 事务 API + WAL
P4         0/7       0.0%   并发 / hazard / HS / RTS / watermark
TOTAL      3/49      6.1%
```

那 3 项已经绿的是**契约自检**（错误码数值、`strerror`），不需要写代码，
但改坏了就会红。

---

## 边界：什么给定，什么留给你写

按 CMU 15-445 的分法：**数据结构、接口签名、测试、评分器全给；核心算法留空。**
判断标准只有一条 —— 打了 `UNIMPLEMENTED("Px/Ty.z ...")` 标签的函数体要你写。
没打标签的代码不要改动（要加私有 helper 请加在对应的 .cpp 里，别动 `include/` 的布局）。

| 给定（读它、别改它） | 你写（156 个函数） |
|---|---|
| `include/*.h` 全部类型、位布局、enum、成员变量 | 每个 `UNIMPLEMENTED` 的函数体 |
| `test/**` 测试与 `harness.hpp`（异常防火墙 / `RUN` / `SCORE`） | —— |
| `compat/wiredtiger.h`（真 WT 头）+ `probe.c` 数值契约 | —— |
| `src_c/` 的内部结构体、`twt_*_create/destroy` 生命周期、`iface` 首成员布局、变参约定注释 | 方法表里每个 `cur_*` / `ses_*` / `conn_*` 的函数体（`...` 的拆包也归你） |
| `Makefile` / `scripts/grade.py` / checkpoint / capstone | —— |
| 页结构与 update 链的**字段**定义 | 链的维护、reconcile 归并、split、evict 策略 |
| WAL 记录格式常量 | 序列化、fsync 顺序、重放规则 |
| `twt_hs.h` 的 HS 接口 | HS 的插入/下探/裁剪，RTS 回滚，watermark 计算 |

查看当前还剩什么：

```bash
make todo              # 全量，按 Px/Ty 分组
make todo P=P2         # 只看某一段
make todo P=P4/T4.3    # 精确到单个 Task
make todo c=1          # 只看每个 Task 的剩余函数数
```

`make todo` 的解析对象就是标签本身，所以它天然不会过期：
你写完一个函数（删掉标签），列表就少一项。

**刻意留白的简化**（别在这里死磕，真 WT 比这复杂得多）：

- `HistoryStore` 用 `std::map` 顶着，不要求你再写一棵 B-Tree
- base page 不带时间戳 → 只有 insert-only 的键能靠「没有 base 行」判断下探
- 压缩器只要求 `NONE` 与 `RLE`，snappy/zlib 走 `WT_COMPRESSOR` 注册表但不评分
- 页内无 `WT_ROW` 的时间窗口聚合（T4.5 只做 `oldest = min(stable, pinned)`）

---

## 任务一览

| Project | 内容 | 关键点 | 测试项 |
|---|---|---|---|
| **P0** | varint、crc32、错误码契约 | LEB128 互逆；`WT_NOTFOUND == -31803` | 3 |
| **P1** | 块管理器 + cell 编解码 | `off==0` 非法；`size` vs `alloc`；前缀压缩 | 3 |
| **P2** | Buffer Pool + B+Tree | descend / split / reconcile / eviction / checkpoint / 编码家族 / 压缩 | 13 |
| **P3** | C ABI + 事务 + WAL | 方法表、变参、元数据表、游标遍历、精确摘链、日志重放 | 23 |
| **P4** | 并发 / MVCC | 读写锁、WT_REF + hazard、History Store、RTS、watermark | 7 |
| **Capstone** | 链接真 `wtperf` / `wt` | 链接器就是评分器（C1–C4，不计入 grade） | — |

每个 Project 的任务书：

- [P0/P1/P2 总表](docs/PROJECTS.md#四任务表)
- [P3 任务书](docs/P3.md)
- [P4 任务书](docs/P4.md)
- [背景阅读：WT 机制与踩坑](docs/WT-NOTES.md)

---

## 目录

```
include/      数据结构与接口（不要改签名）
  twt_defs.h     Status / Slice / Addr / varint / crc32
  twt_block.h    块管理器（BlockHeader / Extent / SuperBlock）
  twt_cell.h     cell 描述符位布局 + 编码家族（suffix / overflow / dict）
  twt_page.h     内存页三件套（Entry / Update / Insert / Ref + WT_REF 状态）
  twt_btree.h    B+Tree + CursorState + 并发锁
  twt_compress.h 压缩器接口（RLE / 可插拔）
  twt_txn.h      P3/T3.4 事务状态机与快照
  twt_log.h      P3/T3.5 WAL 记录格式与恢复
  twt_hazard.h   P4/T4.2 hazard pointer
  twt_hs.h       P4/T4.3–T4.5 History Store / RTS / watermark
src/          C++17 内核（★★ 主战场）
  defs.cpp  block.cpp  cell.cpp  page.cpp  btree.cpp  reconcile.cpp
  compress.cpp  txn.cpp  wal.cpp  hazard.cpp  hs.cpp
src_c/        C ABI 层（extern "C"，包 C++ 内核）
  config.cpp  conn.cpp  cursor.cpp  twt_capi.h
compat/       真 wiredtiger.h（生成物）+ probe.c（契约探针）
test/         harness.hpp + pN_*_test.cpp（不要改）
scripts/      grade.py、todo.py、checkpoint.sh、capstone.sh、gen_wiredtiger_h.py、fetch_wt_sources.sh
docs/         PROJECTS.md / P0–P4.md / CAPSTONE.md / LEADERBOARD.md / WT-NOTES.md
```

## 执行顺序（不可跳步）

```
P0 ─▶ P1 ─▶ P2 ─▶ P3 ─┬─▶ cp1 (T3.0–T3.2)
                       └─▶ cp2 (T3.3–T3.5)
                                │
                                ▼
                               P4 ─▶ capstone (C1–C4)
```

---

## 核心模型（先看这张图，再动手）

```
                磁盘镜像 (disk image, 不可变)
                ┌─────────────────────────────────┐
                │ PageHeader │ cell │ cell │ ...   │
                └─────────────────────────────────┘
                       │  page_in：读块 + 建索引
                       ▼
   内存页 Page（可变）
   ┌──────────────────────────────────────────────────────────┐
   │ 1. entries[]      磁盘镜像解出的键        ← WT_ROW[]      │
   │ 2. Entry::upd     每个键一条 update 链     ← WT_UPDATE     │
   │ 3. ins_head[]     不在镜像里的新键（跳表）  ← WT_INSERT     │
   └──────────────────────────────────────────────────────────┘
                       │  reconcile：把上面三者归并
                       ▼
                新的磁盘镜像 → block_write → 新的 Addr（新块）
```

直接推论：

- 改 1 个字节 = 往 update 链头挂一个节点（O(1)），但 reconcile 要**整页重写** → 写放大
- 旧版本不能直接删（还有旧快照要读）→ 只能进 History Store
- 页搬了家，父页镜像里的地址就失效了 → 父页必须标脏重写

---

## 规则（三条）

1. 只改标了 `TODO` 的函数体，别动签名、别动测试。
2. 不要改 `compat/` 和 `include/` 里的类型定义 —— 那是 ABI 契约。
3. 交作业前 `make grade` 要 49/49，`grep -rn UNIMPLEMENTED src/ src_c/` 要为空，
   `make cp1 && make cp2 && make capstone` 全过。

详细规则见 [docs/PROJECTS.md](docs/PROJECTS.md#三规则)。

---

## 为什么错误码这么较真

真 WT 的错误码在 `dist/api_err.py` 里，注释写着 *"These numbers cannot change
without breaking backward compatibility"*：

| 常量 | 值 |
|---|---|
| `WT_ROLLBACK` | -31800 |
| `WT_DUPLICATE_KEY` | -31801 |
| `WT_ERROR` | -31802 |
| `WT_NOTFOUND` | -31803 |
| `WT_PANIC` | -31804 |
| `WT_RESTART` | -31805 |

`WT_NOTFOUND` 和 `WT_ROLLBACK` 只差 3。如果 internal `Status` 写错一位，
`wtperf` 会把「键不存在」当成「事务冲突」无限重试。
`compat/probe.c` 用 `_Static_assert` 把这条契约钉死。
