# tinywt —— 用重写 WiredTiger 骨架来学存储引擎

一个 **CMU 15-445 式的存储引擎作业集**：仓库里是框架（头文件、数据结构、
测试、任务书），实现全部留空。你要做的是把 `UNIMPLEMENTED(...)` 换成真正的代码，
最后让 **WiredTiger 官方工具 wtperf 直接链接** `libtinywt.a` 跑起来。

- 语言：C++17（内核）+ C ABI（对外）
- 结构：**内存页 ≠ 磁盘页**，update 链 + insert 跳表 + reconcile，和真 WT 一一对应
- 规模：5 个 Project / 31 个测试项
- 契约头：`compat/wiredtiger.h` 是**真** WT 11.3.x 的头文件，不是仿写的

---

## 快速开始

```bash
make grade        # 逐项打分（未实现记 TODO，不中断）
make test         # 跑全部测试，任一不过就非零退出
make p2-test      # 只跑 P2
make probe        # 校验契约头
```

骨架初始状态：

```
project      score   title
P0         1/3      33.3%   基础件（varint / crc32 / 错误码契约）
P1         0/2       0.0%   块管理器 + cell 编解码
P2         0/6       0.0%   Buffer Pool + B+Tree
P3         2/11     18.2%   C ABI（config / conn / session / cursor）
P4         0/9       0.0%   事务 / MVCC / History Store / RTS / WAL
TOTAL      3/31      9.7%
```

那 3 项已经绿的是**契约自检**（错误码数值、`strerror`），不需要写代码，
但改坏了就会红。

---

## 任务一览

| Project | 内容 | 关键点 | 测试项 |
|---|---|---|---|
| **P0** | varint、crc32、错误码契约 | LEB128 互逆；`WT_NOTFOUND == -31803` | 3 |
| **P1** | 块管理器 + cell 编解码 | `off==0` 非法；`size` vs `alloc`；前缀压缩 | 2 |
| **P2** | Buffer Pool + B+Tree | descend / split / reconcile / eviction / checkpoint / cursor | 6 |
| **P3** | C ABI | 方法表、变参、元数据表、游标遍历 | 11 |
| **P4** | 事务 / MVCC / HS / RTS / WAL | 快照、精确摘链、`oldest_active`、日志重放 | 9 |
| **Capstone** | 链接真 `wtperf` / `wt` | 链接器就是评分器 | — |

每个 Project 的任务书：

- [P0/P1/P2 总表](docs/PROJECTS.md#四任务表)
- [P3 任务书](docs/P3.md)
- [P4 任务书](docs/P4.md)
- [背景阅读：WT 机制与踩坑](docs/WT-NOTES.md)

---

## 目录

```
include/      数据结构与接口（不要改签名）
  twt_defs.h    Status / Slice / Addr / varint / crc32
  twt_block.h   块管理器（BlockHeader / Extent / SuperBlock）
  twt_cell.h    cell 描述符位布局
  twt_page.h    内存页三件套（Entry / Update / Insert / Ref）
  twt_btree.h   B+Tree + CursorState
  twt_txn.h     事务 / History Store / RTS / WAL
src/          C++17 内核（★★ 主战场）
  defs.cpp  block.cpp  cell.cpp  page.cpp  btree.cpp  reconcile.cpp  txn.cpp
src_c/        C ABI 层（extern "C"，包 C++ 内核）
  config.cpp  conn.cpp  cursor.cpp  twt_capi.h
compat/       真 wiredtiger.h（生成物）+ probe.c（契约探针）
test/         harness.hpp + pN_*_test.cpp（不要改）
scripts/      grade.py、gen_wiredtiger_h.py、fetch_wt_sources.sh
docs/         PROJECTS.md / P3.md / P4.md / WT-NOTES.md
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
3. 交作业前 `make grade` 要 31/31，`grep -rn UNIMPLEMENTED src/ src_c/` 要为空。

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
