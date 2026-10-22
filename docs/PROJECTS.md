# tinywt 总任务书

> **这是唯一入口。** 项目地图、子项目清单、验收标准、依赖顺序、评分规则都在这一份里；
> 每个 Project 的机制讲解与踩坑细节在 `docs/projects/*.md`。
>
> **约束**：内核 C++17；对外 C ABI（`wiredtiger.h` 语义，直接用真头文件）。
> **终局验收**：真 WiredTiger 工具（`wtperf` / `wt`，原源码编译）直接链接
> `build/libwiredtiger.so` 跑起来；缺哪个接口，链接器当场报哪个符号。
> **真源码定位**均指 `wiredtiger/wiredtiger` 的 `mongodb-8.0` 分支。
>
> **当前仓库状态**：只有骨架（接口 + 数据结构 + 测试 + 任务书），
> 190 个函数体是 `UNIMPLEMENTED("Px/Ty.z ...")`。跑 `make grade` = 3/49。

---

## 0. 三分钟上手

```bash
make todo P=P0     # 我这段要写哪几个函数（带 file:line）
vim src/common/defs.cpp
make p0-test       # 只跑 P0
make grade         # 看总分从 3/49 往上走
```

**唯一一条规则**：打了 `UNIMPLEMENTED("Px/Ty.z ...")` 标签的函数体要你写；
没打标签的代码不要改（要加私有 helper 就加在对应 `.cpp` 里，别动 `include/` 的布局）。

**边界为什么这么划**：

| 给定 | 你写 |
|---|---|
| 全部类型、位布局、enum、成员变量、接口签名 | 156 个函数体 |
| 49 个评分项（测试 + harness + 评分器） | —— |
| 真 `wiredtiger.h` + ABI 合同探针 | —— |
| 页 / update 链 / WAL 记录 / HS 的**字段定义** | 链维护、reconcile 归并、split、evict、下探、回放 |
| 构建、门禁、排行榜脚本 | —— |

---

## 1. 这是 WiredTiger，不是 BusTub 的第二季

先说清楚借了什么、没借什么。

**借的只有作业形式**（四条，跟学什么无关）：

1. 一个 Project = 一组能独立编译、独立验收的测试二进制
2. 一个测试函数 = 一个评分项，harness 打 `PASS / FAIL / NOTIMPL / ERROR`
3. 门禁：`cp1` / `cp2` 过了才准进下一段
4. 链接器当评分器：拿真头文件、真工具链，缺符号就链不上

**内容一条没借。** 主轴是 WT 自己的机制依赖图：

```
错误码 / 编码  ──▶  磁盘格式（块 · extent · cell）
                          │
                          ▼
                 内存页（entries + update 链 + insert 跳表）
                          │
            ┌─────────────┴─────────────┐
            ▼                           ▼
      reconcile（整页重写）          读路径（descend / 两流归并）
            │                           │
            ▼                           ▼
      eviction + checkpoint       时间戳可见性（start/stop/durable）
            │                           │
            └─────────────┬─────────────┘
                          ▼
              版本回收（HS 搬家 / RTS / watermark）
                          │
                          ▼
              并发（WT_REF + hazard） 与 WAL（LSN 对齐 checkpoint）
```

**同名不同物** —— 这张表才是关键，别被名字骗了：

| 名字 | BusTub 里是什么 | tinywt 里是什么 | 差别 |
|---|---|---|---|
| B+Tree | 页里存 `(key, RID)` 数组，原地插入 + latch crabbing | 内存页 = 磁盘镜像解出的 `entries` + **每键一条 update 链** + 新键的 **insert 跳表**（`include/tinywt/storage/page.h:151,162-163`） | 写不原地改；reconcile 整页重写，用写放大换无锁读 |
| Buffer Pool | frame 复用固定磁盘页，脏页刷回**原位置** | 页地址是 cookie，页会分裂、会搬家；淘汰前必须 reconcile（`btree/btree.h:241,261`） | 页身份不稳定 → 父页里的地址必须跟着重写 |
| MVCC | 每个 tuple 一条版本链，`txn_id` 判可见 | update 链 + 独立 **History Store** + `start_ts/stop_ts/durable_ts`（`txn/history_store.h`） | 旧版本要落到**另一棵树**；水位是 `min(stable, pinned)` |
| 恢复 | 没有对应作业（只在课上讲） | WAL 记录 + 三条恢复规则 + `ckpt_lsn` 截断（`wal/log.h`） | 要真写日志、真截断、真重放 |
| 并发读 | latch crabbing（读者也拿 latch） | `WT_REF` 状态机 + **hazard pointer**（`concurrency/hazard.h`），读路径不持锁 | 淘汰线程不能直接 free，先确认没人 hazard 住 |

**WT 独有、BusTub 完全没有的八件事** —— 这就是这个项目的全部重点：

1. **内存页 ≠ 磁盘页**：`entries[]` / update 链 / insert 跳表三件套并存
2. **reconcile 整页重写**：把三者归并成新镜像，不是原地改字节
3. **eviction 与 checkpoint 的脏页协议**：脏页必须先 reconcile 才能淘汰
4. **两套结构并存**：链上的旧键和跳表里的新键，读的时候要归并取小
5. **History Store**：旧版本搬家 + 读取时按 `read_ts` 下探
6. **三层时间戳**：`stable` / `pinned` / `oldest`，以及从 stable 回滚的 RTS
7. **`WT_REF` 状态机 + hazard pointer**：`DISK → READING → MEM → LOCKED`
8. **WAL 与 checkpoint 的 LSN 对齐**：恢复点、截断点、turtle 的简化版

那为什么 Project 编号看着像 15-445？因为「底座 → 结构 → 操作 → 并发」这个由浅入深的
顺序对任何存储引擎都成立，不是照抄它的任务清单。WT 自己的依赖是**网状**的
（时间戳贯穿读和写、WAL 与 checkpoint 耦合），硬按源码目录切会得到一堆无法独立
验收的碎片；所以切分边界取「能不能独立跑一个测试二进制」，编号只用来固定顺序。
与原顺序不一样的地方，各任务书开头都标了。

---

## 2. 项目地图

| Project | 子项目 | 评分项 | 待写函数 | 前置 | 出口门禁 | 任务书 |
|---|---|---|---|---|---|---|
| **P0** 编码件与错误码 | T0.1–T0.2 | 3 | 5 | — | `make p0-test` | [p0_foundation](projects/p0_foundation.md) |
| **P1** 磁盘格式：块 / extent / cell | T1.1–T1.3 | 3 | 20 | P0 | `make p1-test` | [p1_block_manager](projects/p1_block_manager.md) |
| **P2** 页与写路径 | T2.1–T2.7 | 13 | 41 | P1 | `make p2-test` | [p2_btree](projects/p2_btree.md) |
| **P3** 对外 ABI：cursor / 事务 / 日志 | T3.0–T3.6 | 23 | 106 | P2 | `cp1` / `cp2` | [p3_api_txn_wal](projects/p3_api_txn_wal.md) |
| **P4** 版本、时间与并发 | T4.1–T4.5 | 7 | 18 | P3 | `make p4-test` + `asan` / `tsan` | [p4_concurrency_mvcc](projects/p4_concurrency_mvcc.md) |
| **Capstone** | C1–C4 | 不计分 | 0 | P4 | `make capstone` | [capstone](projects/capstone.md) |

```
P0 ──▶ P1 ──▶ P2 ──▶ P3 ─┬─▶ cp1 (T3.0–T3.2: 合同 / catalog / 写路径)
                          ├─▶ cp2 (T3.3–T3.5: 读路径 / 事务 / WAL)
                          └─▶ T3.6 (工具面 ABI，由 Capstone C1/C2 验收)
                                   │
                                   ▼
                                  P4 ──▶ Capstone (C1–C4)
```

`cp1` / `cp2` 不是计分项，是**人工门**：不过就不准进下一段。
它们的第三道关是「真 wtperf 用你的 `.so` 跑起来」，通常在 P3 后半段才有意义。

---

## 3. 子项目清单

每个子项目（`Pn/Tn.m`）都对应：**一组函数 + 一组测试 + 一段真 WT 源码**。
「函数」列是 `make todo` 报的数量；「评分项」列是 `make grade` 里的条目数。

### P0 —— 编码件与错误码（5 个函数 / 3 项）

| 子项目 | 目标 | 函数 | 评分项 | 关键概念 |
|---|---|---|---|---|
| T0.1 构建骨架 + 错误码契约 | **已给**：CMake、harness、`Status` 数值 | 0 | 1 | `WT_NOTFOUND == -31803`；错一位 wtperf 就死循环 |
| T0.2 varint + crc32 | LEB128 编解码、CRC32C | 5 | 2 | 互逆性；0x80 续位；查表 vs 硬件加速 |

### P1 —— 磁盘格式：块、extent、cell（20 个函数 / 3 项）

| 子项目 | 目标 | 函数 | 评分项 | 关键概念 |
|---|---|---|---|---|
| T1.1 块读写 + 校验 | 块头、checksum、损坏检测 | 4 | 1 | `off == 0` 非法；改 1 字节必须报 CORRUPT |
| T1.2 extent 三表 + 超级块 | alloc / avail / discard、addr cookie | 11 | 1 | free → discard → checkpoint 才真正回收 |
| T1.3 cell 编解码 | short/long key/value、前缀压缩、三种地址 | 5 | 1 | `size` vs `alloc`；cell 是磁盘层的记录格式 |

### P2 —— 页与写路径（41 个函数 / 13 项）

| 子项目 | 目标 | 函数 | 评分项 | 关键概念 |
|---|---|---|---|---|
| T2.1 内存页三件套 + page_in | `entries[]` / update 链 / insert 跳表 | 9 | 1 | **内存页 ≠ 磁盘页** |
| T2.2 descend 搜索 | 二分下降、内部页分隔符语义 | 2 | 1 | 5000 键全量回读 |
| T2.3 插入 + 分裂 | 叶分裂 / 内部分裂 | 5 | 1 | 左半页分隔符漏更新；只降不升 |
| T2.4 reconcile + eviction | 归并三者写回镜像、LRU 淘汰 | 7 | 1 | 写放大；页搬家 → 父页必须标脏 |
| T2.5 checkpoint + verify | 后序遍历、超级块、重开恢复、读可见性 | 8 | 2 | 崩溃点只能落在「改完超级块」之后 |
| T2.6 编码家族 | suffix / overflow / dict | 8 | 4 | 长 value 搬到独立块；字典压缩的 COPY 语义 |
| T2.7 压缩器 | RLE + 可插拔注册表 | 2 | 3 | 压缩发生在**块写入之前**，不感知 cell |

### P3 —— 对外 ABI：cursor / 事务 / 日志（106 个函数 / 23 项）

| 子项目 | 目标 | 函数 | 评分项 | 关键概念 |
|---|---|---|---|---|
| T3.0 错误码契约 | **已给**：`Status → WT_*` 恒等映射 + `wiredtiger_version` | 0 | 2 | 只差 3 的两个负数值 |
| T3.1 config + conn + catalog | 配置解析、URI 映射、元数据表 | 5 | 2 | 元数据自己用 B-Tree 存（`WiredTiger.wt`） |
| T3.2 cursor 写路径 | 方法表、变参拆包、insert/update | 21 | 1 | `iface` 必须首成员；`void(...)` |
| T3.3 cursor 读路径 | next/prev 状态机、search_near、split 中遍历 | 16 | 7 | 两条流归并取小；**游标位置是页+偏移** |
| T3.4 事务 API | begin/commit/rollback、modify、truncate | 21 | 5 | 可见性判断；回滚只摘自己的版本 |
| T3.5 WAL + 恢复 | 记录格式、fsync 顺序、重放规则 | 9 | 6 | 尾部截断合法 / 中间损坏非法 |
| T3.6 工具面内部 ABI | **跨切面**：34 个 `__wt_*`（真 `wt`/`wtperf` 会来要） | 34 | 0（由 C1/C2 验收） | 符号齐 ≠ 功能对；`nm -D` 量缺口 |

### P4 —— 版本、时间与并发（18 个函数 / 7 项）

| 子项目 | 目标 | 函数 | 评分项 | 关键概念 |
|---|---|---|---|---|
| T4.1 并发 L1：tree 级锁 | **跨切面**：给公开入口加 `shared_lock` / `unique_lock` | 0 新函数（在十几个公开入口加锁） | 1 | 递归加锁会自锁 → 入口加锁、内部假定已持有 |
| T4.2 WT_REF + hazard | 状态机 `DISK→READING→MEM→LOCKED`、hazard 槽 | 8 | 2 | 读线程被淘汰线程踩到 = use-after-free |
| T4.3 History Store | 旧版本搬家 + 读取下探（`HistoryReader` 钩子） | 6 | 2 | 判据是 `oldest = min(stable, pinned)`，不是 stable |
| T4.4 未提交 reconcile + RTS | 未决 update 的落盘形态、crash 后回滚 | 3 | 1 | RTS 从 stable 点回滚，不是最新点 |
| T4.5 watermark | `oldest = min(stable, pinned)` | 1 | 1 | 长事务会卡住水位 → HS 和 cache 都涨 |

### Capstone —— 结业验收（不计分）

| 关卡 | 内容 | 过关标准 |
|---|---|---|
| C1 | 真 `wtperf` 全家桶（populate / read / update / evict / checkpoint / long-txn） | 全部跑通，无符号缺失 |
| C2 | 真 `wt` 工具子集（`wt list` / `wt dump` / `wt verify`） | 能读自己写的库 |
| C3 | 对照组：同一 runner 跑真 WT，对比吞吐 / 空间放大 | 差距在可解释范围内 |
| C4 | 排行榜：`docs/LEADERBOARD.md` 填数字 | 至少一条曲线 |

---

## 4. 三种交付形态（不是所有子项目都是填空）

| 形态 | 例子 | 你怎么做 |
|---|---|---|
| **填空** | 绝大多数子项目 | 把 `UNIMPLEMENTED(...)` 换成实现，签名不动 |
| **加壳（跨切面）** | T4.1 并发 L1 | 已有函数体不动逻辑，只在公开入口加一层锁；`make todo` 看不到它，因为不新增函数 |
| **接线** | T4.3 读取下探 | 实现 `HistoryStore::read`，并确认 `Btree::get` 走 `history_reader_`（钩子已在头里，测试用 `FakeHistory` 验） |

`make todo` 只统计第一种。第二种看 `tinywt/btree/btree.h` 里 `P4/T4.1` 的注释；
第三种看 `tinywt/txn/history_store.h` 与 `tinywt/btree/btree.h` 里 `P4/T4.3` 的注释。

---

## 5. 目录结构

按模块分目录，公共头与私有头分离 —— 和 BusTub / 绝大多数 C++ 项目一致。

```
tinywt/
├── CMakeLists.txt              ← 构建的唯一事实来源
├── Makefile                    ← 便捷封装（转发 cmake），保留 make grade 等短命令
├── README.md
├── docs/
│   ├── PROJECTS.md             ← 本文件（总任务书）
│   ├── projects/               ← 每个 Project 的任务书（机制讲解 + 踩坑）
│   │   ├── p0_foundation.md
│   │   ├── p1_block_manager.md
│   │   ├── p2_btree.md
│   │   ├── p3_api_txn_wal.md
│   │   ├── p4_concurrency_mvcc.md
│   │   └── capstone.md
│   ├── notes/wt-notes.md       ← 背景阅读：WT 机制与常见误解
│   └── LEADERBOARD.md
├── include/tinywt/             ← 公共头（布局 = ABI 契约，不要改）
│   ├── common/defs.h           Status / Slice / varint / crc32
│   ├── storage/block.h         块管理器（BlockHeader / Extent / SuperBlock）
│   ├── storage/cell.h          cell 描述符位布局 + 编码家族
│   ├── storage/page.h          内存页三件套（Entry / Update / Insert / Ref）
│   ├── btree/btree.h           B+Tree + CursorState + 并发锁 + HistoryReader
│   ├── compression/compress.h  压缩器接口（RLE / 可插拔）
│   ├── txn/txn.h               事务状态机与快照
│   ├── txn/history_store.h     History Store / RTS / watermark
│   ├── wal/log.h               WAL 记录格式与恢复
│   └── concurrency/hazard.h    hazard pointer
├── src/                        ← 实现（★ 主战场）
│   ├── common/defs.cpp
│   ├── storage/{block,cell,page}.cpp
│   ├── btree/{btree,reconcile}.cpp
│   ├── compression/compress.cpp
│   ├── txn/{txn,history_store}.cpp
│   ├── wal/log.cpp
│   ├── concurrency/hazard.cpp
│   └── api/                    ← C ABI 层（capi.h 是私有头，不对外）
│       ├── capi.h
│       └── {config,connection,cursor}.cpp
├── test/
│   ├── support/harness.hpp     RUN / CASE / SCORE / 异常防火墙
│   ├── contract/abi_probe.c    错误码数值 + iface 布局（编译期 + 链接期）
│   ├── p0/defs_test.cpp
│   ├── p1/block_cell_test.cpp
│   ├── p2/{btree,codec,compress}_test.cpp
│   ├── p3/{api,cursor,txn,wal}_test.cpp
│   └── p4/{concurrency,hs_rts}_test.cpp
├── scripts/                    grade.py / todo.py / check_docs.py / checkpoint.sh / capstone.sh
│                               lib_wt.sh / wt_tools.sh / wt_abi.py / gen_wiredtiger_h.py
└── third_party/
    ├── wiredtiger/include/wiredtiger.h   ← 真 WT 11.3.x 头文件（合同来源）
    └── (external/wiredtiger)             ← 真 WT 完整克隆 + 构建产物（make wt-tools，gitignore）
```

**为什么这样分**：

- `include/tinywt/<module>/` 与 `src/<module>/` 一一对应，找实现不用猜目录。
- `src/api/` 是唯一对外暴露 C ABI 的地方；私有头 `capi.h` 不进 `include/`，
  因为外部用户只应该看到 `wiredtiger.h`。
- `test/pN/` 与 Project 同编号，`make p2-test` 就是跑 `test/p2/` 下的二进制。
- 真 WT 头放 `third_party/` 而不是自己写一份「像样的」：合同只能用真的。

---

## 6. 构建、测试、评分

CMake 是事实来源，Makefile 只是短命令：

| 命令 | 等价 CMake | 用途 |
|---|---|---|
| `make build` | `cmake --build build -j` | 编译 |
| `make test` | 逐个跑 `build/bin/*_test` | 全量测试 |
| `make p2-test` | 只跑 `build/bin/p2_*_test` | 单段测试 |
| `make grade` | `python3 scripts/grade.py build/bin/*_test` | 逐项打分 |
| `make todo` | `python3 scripts/todo.py [-p P2] [-c]` | 待写函数导航 |
| `make check-docs` | `python3 scripts/check_docs.py` | 文档与代码对账（数字 / 路径 / 链接） |
| `make probe` | 目标 `abi_probe_obj` | 编译期合同 |
| `make probe-link` | 目标 `abi_link_check` | 链接期合同 |
| `make shared` | 目标 `wiredtiger` | `build/libwiredtiger.so` |
| `make asan` / `tsan` | 独立构建目录 + `-DTINYWT_SANITIZE=` | 越界 / 数据竞争 |
| `make cp1` / `cp2` | `scripts/checkpoint.sh 1\|2` | 人工门 |
| `make capstone` | `scripts/capstone.sh` | C1–C4 |

**评分怎么算**：一个测试函数 = 一个评分项。harness 打印
`RESULT <proj> <name> <status> <pass> <fail>`，`grade.py` 只做汇总。
状态只有四种：`PASS` / `FAIL`（写错了）/ `NOTIMPL`（还没写）/ `ERROR`（抛了别的异常）。

> `make asan` / `tsan` 走 `ctest`，退出码跟着测试走 —— 骨架阶段会因为大量
> `NOTIMPL` 整体非 0，这是正常的。它的用途是**在你实现之后**确认没有越界 / 数据竞争：
> 真正的信号是输出里有没有 `AddressSanitizer` / `ThreadSanitizer` 报告。

**骨架初始分**：

```
P0  1/3    P1  0/3    P2  0/13    P3  2/23    P4  0/7    TOTAL 3/49 (6.1%)
```

那 3 项绿的是**契约自检**（错误码数值、`strerror`），不用写代码，改坏了就会红。

---

## 7. 规则（破坏这些会直接失去验收资格）

1. 只改标了 `UNIMPLEMENTED` 的函数体；**不要改** `include/` 里的类型定义、成员顺序、
   函数签名 —— 那些是 ABI 契约，改了 `make probe` 会红，`wtperf` 会崩。
2. **不要改 `test/` 和 `scripts/grade.py`**。测试就是评分标准。
3. 真 `wiredtiger.h` 在 `third_party/wiredtiger/include/`，不要往 `include/tinywt/` 里塞。
4. 提交纪律：一个子项目一个 commit，消息带编号（`P2/T2.3: 叶分裂`）。
5. 交作业前：`make grade` 要 49/49，`grep -rn UNIMPLEMENTED src/` 要为空，
   `make cp1 && make cp2 && make capstone` 全过。

---

## 8. 建议排期

以「每天 2–3 小时、按子项目推进」估：

| 阶段 | 内容 | 手感 |
|---|---|---|
| 第 1 天 | P0 全部 | 半天就够；varint 写错会让 P2 的 descend 莫名失败 |
| 第 2–3 天 | P1 全部 | 坑在 extent 回收 |
| 第 4–9 天 | P2 T2.1–T2.5 | **最难的一段**，约占全部工作量 40% |
| 第 10 天 | P2 T2.6–T2.7 | 编码家族是体力活 |
| 第 11–14 天 | P3 T3.1–T3.3 | C ABI 变参拆包最容易出错 |
| 第 15–17 天 | P3 T3.4–T3.5 | 事务可见性 + WAL 三条恢复规则 |
| 第 18 天 | P3 T3.6 工具面 ABI | 34 个 `__wt_*`；**先 `__wt_getopt`**，否则 `wt` 连参数都解析不了 |
| 第 19–21 天 | P4 全部 | T4.2 / T4.3 是重点；T4.1 只是加锁 |
| 第 22 天+ | Capstone | 真工具一旦跑起来，剩下都是「对齐行为」 |

卡住时的判断顺序：**先 `make todo P=Pn` 看是不是漏了函数，再 `make pN-test` 看第一个红的是哪项**。
不要一上来就怀疑算法错 —— 多数情况是前一个 Project 的某个函数没写对。

---

## 9. 真工具依赖面（合同，`nm` 实测）

> 这一节的所有数字都是**量出来的**，不是数的：`make wt-abi` 拿真二进制的未定义符号
> 减去我们 `.so` 的导出符号，差集就是缺口。下面的 34 / 20 / 18 都能一条命令复现。

### wtperf（7 源文件 + testutil）

```
wtperf = bench/wtperf/{wtperf,wtperf_config,wtperf_misc,track,wtperf_throttle,
        wtperf_truncate,idle_table_cycle}.c + test/utility/ 按需
公开函数: wiredtiger_open / wiredtiger_strerror /
          wiredtiger_config_parser_open（wtperf 解析配置就调它）/ wiredtiger_calc_modify
方法面:   close(18) open_cursor(14) open_session(12) set_key(9) get_key(9)
          set_value(7) create(7) next(6) begin_txn(6) search(5) rollback(5)
          insert(5) reset(4) commit(3) checkpoint(3) update(2) remove(2)
          prev(2) drop(2) truncate(1) modify(1)
```

**但 wtperf 不是只用公开 API**：它链了 `test/utility/`，那个层用的是 WT 内部封装，
所以二进制里另带 **18 个 `__wt_*`** 未定义符号（`__wt_thread_create` / `__wt_sleep` /
`__wt_random` / `__wt_abort` / `__wt_filename` …）。这些不会出现在"wtperf 调了什么"的
源码阅读里，只有 `nm -D --undefined-only` 才看得见。

### wt 工具

```
$ nm -D --undefined-only $WT_SRC/build/wt | grep __wt    → 20 个
```

20 个，不是 23 个。按性质分四组：

| 组 | 个数 | 例子 | 碰存储内核？ |
|---|---|---|---|
| 平台胶水 | 14 | `__wt_thread_create` `__wt_sleep` `__wt_random` `__wt_abort` | 否 |
| 缓冲 / 格式化 / 配置 | 10 | `__wt_buf_fmt` `__wt_config_merge` `__wt_getopt` `__wt_err_func` | 部分（buf 是 WT_ITEM） |
| JSON | 5 | `__wt_json_token` `__wt_json_strncpy` | 否 |
| 内核元数据与日志 | 5 | `__wt_block_ckpt_decode` `__wt_metadata_get_ckptlist` `__wt_txn_printlog` | **是** |

（20 + 18 − 4 个重叠 = 我们一共要给 **34 个**；另有公开 API `wiredtiger_version`，
`wt` 启动时用它做版本自检，已随 T3.0 给出。）

> ★ 之前这一节写的是"23 个工具类，不碰存储内核"—— **错了，别信**。
> 两个错误：数字是估的；性质也判断错了。`__wt_block_ckpt_decode` 要解 checkpoint 地址
> cookie（`src/block/block_addr.c:272`），`__wt_metadata_get_ckptlist` 要读元数据表
> （`src/meta/meta_ext.c:88`）。C2 之所以难，恰恰难在这 5 个：它们要求你 P2 写出来的
> checkpoint 格式与真 WT **字节级一致**。

符号齐了只是"动态链接器肯加载"。剩下 34 个空桩还在 `src/api/wt_tool_abi.cpp` 里，
`make wt-abi` 会连"还有几个是空桩"一起报。

### 错误码（只差 3 的两个负数）

| 常量 | 值 |
|---|---|
| `WT_ROLLBACK` | -31800 |
| `WT_DUPLICATE_KEY` | -31801 |
| `WT_ERROR` | -31802 |
| `WT_NOTFOUND` | -31803 |
| `WT_PANIC` | -31804 |
| `WT_RESTART` | -31805 |

`test/contract/abi_probe.c` 用 `_Static_assert` 钉死；`make probe` 编译期就能抓到偏差，
`make probe-link` 进一步把缺的接口交给链接器。

---

## 10. 刻意简化（别在这里死磕）

| 简化 | 真 WT | 为什么这样定 |
|---|---|---|
| `HistoryStore` 内部用 `std::map` | 一棵真 B-Tree（`WiredTigerHS.wt`） | 要学的是**搬家与下探的时机**，不是再写一棵树 |
| base page 不带时间戳 | base 页有 `start_ts/stop_ts` | 只有 insert-only 的键能靠「没有 base 行」判断下探；够测 T4.3 |
| 压缩只要求 `NONE` + `RLE` | snappy / zlib / zstd 扩展 | 压缩器接口才是知识点，算法实现不是 |
| 窗口聚合只做 `min(stable, pinned)` | 完整 `WT_TIME_AGGREGATE` 位打包 | 位打包是体力活，见可选池 O5 |
| 单线程 log slot，无组提交 | `log_slot.c` 组提交 | 见可选池 O10 |

## 11. 可选池（O1–O12，按性价比排序）

| # | 主题 | 真 WT 定位 | 规模感 | 解锁什么 |
|---|---|---|---|---|
| O1 | **test/format 式随机压测器** | `test/format/` | 2–3 天 | 随机配置 + crash + 校验，**性价比最高** |
| O2 | 多 checkpoint 保留 + named checkpoint | `block_ckpt.c`、turtle 文件 | 1–2 天 | `checkpoint-stress` 原配置、`wt list -c` |
| O3 | backup 游标 | `src/cursor/cur_backup.c` | 2 天 | backup 系 runner |
| O4 | column-store | `src/btree/col_*.c` | 3–4 天 | `wt` col 命令、O1 覆盖面 |
| O5 | 完整 `WT_TIME_AGGREGATE` 位打包 | `src/include/` | 1 天 | reconcile 与 WT 完全同构 |
| O6 | statistics 游标 | `src/cursor/cur_stat.c` | 2 天 | `wt stat`、perf_run_py 自动分析 |
| O7 | workgen | `bench/workgen/` | 一周+ | 新 benchmark 框架（不建议先做） |
| O8 | test/suite python 子集 | `test/suite/` | 逐周扩 | 覆盖最广的官方测试 |
| O9 | LSM tree | `src/lsm/` | 数周 | lsm runner（**不建议**，规模不成比例） |
| O10 | log slot 并发组提交 | `src/log/log_slot.c` | 2 天 | 回头补真 slot 机制 |
| O11 | salvage | `src/btree/bt_slvg.c` | 3 天 | `wt salvage`（学 block/extent 很好） |
| O12 | 磁盘格式完全对齐真 WT | `block.h/cell.h` 全量 | 长期 | 真 `wt` 直接打开 tinywt 文件 |

---

## 12. 附：真 WT 源码对照索引

| tinywt | 真 WT |
|---|---|
| `include/tinywt/common/defs.h` | `src/include/error.h`、`misc.h`、`intpack_inline.h` |
| `include/tinywt/storage/block.h` | `src/include/block.h`、`src/block/block_{open,read,write,ext,addr}.c` |
| `include/tinywt/storage/cell.h` | `src/include/cell.h`、`src/cell/cell_{pack,unpack}.c` |
| `include/tinywt/storage/page.h` | `src/include/btmem.h`（`WT_PAGE` / `WT_ROW` / `WT_UPDATE` / `WT_INSERT`） |
| `include/tinywt/btree/btree.h` | `src/btree/bt_{read,search,split,write,sync,curnext,curprev,cursor}.c` |
| `src/btree/reconcile.cpp` | `src/reconcile/rec_write.c` |
| `include/tinywt/compression/compress.h` | `src/compress/`、`ext/compressors/` |
| `include/tinywt/txn/txn.h` | `src/include/txn.h`、`src/txn/txn.c`、`txn_inline.h` |
| `include/tinywt/txn/history_store.h` | `src/history/hs.c`、`src/txn/txn_rollback_to_stable.c` |
| `include/tinywt/wal/log.h` | `src/include/log.h`、`src/log/log.c`、`log_recover.c` |
| `include/tinywt/concurrency/hazard.h` | `src/include/hazard.h`、`src/support/hazard.c` |
| `src/api/*` | `src/conn/`、`src/cursor/`、`src/session/`（对外 ABI 层） |
