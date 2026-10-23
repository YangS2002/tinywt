# tinywt

用**重写一个 WiredTiger 骨架**来学 WT 本身：仓库里给类型、接口、测试和评分器，
实现全部留空，你把 `UNIMPLEMENTED(...)` 换成代码。

作业形式借了 CMU 15-445 的做法（一个 Project 一组可独立编译、独立评分的测试，
外加门禁和排行榜），**内容一条没借** —— 全是 WT 自己的机制：

- **内存页 ≠ 磁盘页**：`entries` + 每键一条 update 链 + 新键的 insert 跳表
- **reconcile 整页重写**：写不原地改，用写放大换无锁读
- **History Store**：旧版本搬家，读的时候按 `read_ts` 下探
- **三层时间戳**：`stable` / `pinned` / `oldest`，RTS 从 stable 回滚
- **WT_REF 状态机 + hazard pointer**：`DISK → READING → MEM → LOCKED`
- **WAL 与 checkpoint 的 LSN 对齐**，以及三条崩溃恢复规则
- 对外 C ABI 直接用**真** `wiredtiger.h`；终局验收是真 `wtperf` / `wt` 链上
  `build/libwiredtiger.so` 跑起来 —— **缺哪个接口，链接器当场报哪个符号**

> 完整任务书（项目地图、子项目清单、验收标准、排期）在
> **[docs/PROJECTS.md](docs/PROJECTS.md)** ← 从这里开始。
> 想先看「WT 和 BusTub 到底哪里不一样」，直接跳
> [§1 同名不同物](docs/PROJECTS.md#1-这是-wiredtiger不是-bustub-的第二季)。

---

## 快速开始

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j   # 或直接 make
make todo              # 我这段要写哪些函数？（按子项目分组，带 file:line）
make grade             # 逐项打分：49 个评分项，未实现记 TODO，不中断
make p2-test           # 只跑 P2
make check-docs        # 文档里的数字/路径/链接是否还和代码一致
make wt-tools          # 一次性：拉真 WT 源码并编出 wt / wtperf（Capstone 前置）
make wt-abi            # 量工具面进度：真工具要的符号，我们导出齐了没
make capstone-selftest # 自检 C1/C2 的判据：换真库跑一遍应该全绿（验判据，不验实现）
```

骨架初始状态：

```
P0  1/3    P1  0/3    P2  0/13    P3  2/23    P4  0/7    TOTAL 3/49 (6.1%)
待写：190 个函数（P0 5 · P1 20 · P2 41 · P3 106 · P4 18）
```

那 3 项绿的是**契约自检**（错误码数值、`strerror`），不用写代码，改坏了就会红。

> 开写作业前先看 **[docs/GUIDELINE.md](docs/GUIDELINE.md)** —— 那是「怎么写作业」的规则手册：
> 能改哪些文件、十条禁止做法、分数怎么算、崩了怎么办、调试附录。机制讲解仍在各 Project 的任务书里。

---

## 项目地图

| Project | 子项目 | 评分项 | 你能学到什么 | 任务书 |
|---|---|---|---|---|
| **P0** 编码件与错误码 | T0.1–T0.2 | 3 | LEB128 / CRC32C / 状态码契约 | [p0](docs/projects/p0_foundation.md) |
| **P1** 磁盘格式 | T1.1–T1.3 | 3 | 块与 addr cookie、extent 三表、cell 打包 | [p1](docs/projects/p1_block_manager.md) |
| **P2** 页与写路径 | T2.1–T2.7 | 13 | 内存页三件套、descend、分裂、reconcile、eviction、checkpoint、编码家族 | [p2](docs/projects/p2_btree.md) |
| **P3** 对外 ABI | T3.0–T3.5 | 23 | 方法表与变参、cursor 两流归并、事务可见性、日志重放 | [p3](docs/projects/p3_api_txn_wal.md) |
| **P4** 版本、时间与并发 | T4.1–T4.5 | 7 | tree 级锁、WT_REF 状态机、hazard、History Store、RTS、watermark | [p4](docs/projects/p4_concurrency_mvcc.md) |
| **Capstone** | C1–C4 | 不计分 | 链接真 `wtperf` / `wt`，与真 WT 对照 | [capstone](docs/projects/capstone.md) |

```
P0 ──▶ P1 ──▶ P2 ──▶ P3 ─┬─▶ cp1 (T3.0–T3.2)
                          └─▶ cp2 (T3.3–T3.5)
                                   │
                                   ▼
                                  P4 ──▶ capstone (C1–C4)
```

---

## 目录

```
CMakeLists.txt        构建的唯一事实来源
Makefile              便捷封装（make grade / p2-test / asan …）
docs/PROJECTS.md      ★ 总任务书
docs/GUIDELINE.md     ★ 作业指南：规则 / 禁止做法 / 评分 / 调试附录
docs/projects/        每个 Project 的机制讲解与踩坑
include/tinywt/       公共头（布局 = ABI 契约，不要改）
  common/defs.h         Status / Slice / Addr / varint / crc32
  storage/{block,cell,page}.h
  btree/btree.h         B+Tree + CursorState + 并发锁 + HistoryReader
  compression/compress.h
  txn/{txn,history_store}.h
  wal/log.h
  concurrency/hazard.h
src/                  实现（★ 主战场），目录与 include/tinywt/ 一一对应
  api/                  C ABI 层（capi.h 是私有头，不对外）
test/
  support/harness.hpp   RUN / CASE / SCORE / 异常防火墙
  contract/abi_probe.c  错误码数值 + iface 布局（编译期 + 链接期合同）
  p0/ p1/ p2/ p3/ p4/   与 Project 同编号
scripts/              grade.py / todo.py / checkpoint.sh / capstone.sh
third_party/
  wiredtiger/include/wiredtiger.h   真 WT 11.3.x 头文件（合同来源）
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

1. 只改标了 `UNIMPLEMENTED` 的函数体，别动签名、别动 `include/` 的布局、别动 `test/`。
2. `third_party/wiredtiger/include/wiredtiger.h` 是**真**头文件，不要改也不要替换。
3. 交作业前：`make grade` 49/49，`grep -rn UNIMPLEMENTED src/` 为空，
   `make cp1 && make cp2 && make capstone` 全过。

详细规则见 [docs/PROJECTS.md §7](docs/PROJECTS.md#7-规则破坏这些会直接失去验收资格)。

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

`WT_NOTFOUND` 和 `WT_ROLLBACK` 只差 3。内部 `Status` 写错一位，`wtperf` 就会把
「键不存在」当成「事务冲突」无限重试。`test/contract/abi_probe.c` 用
`_Static_assert` 把这条契约钉死，`make probe` 在编译期就能抓到偏差。
