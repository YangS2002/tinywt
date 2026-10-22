# tinywt 作业计划 v3 —— 功能拆分按 CMU 15-445（BusTub）作业结构

> **约束**：内核 C++17；对外 C ABI（`wiredtiger.h` 语义）。
> **终局验收**：真 WiredTiger 测试工具（wtperf / wt，原源码编译）直接链接 tinywt 的
> `libwiredtiger.a` 运行。
> 真源码定位均指 `wiredtiger/wiredtiger` `mongodb-8.0` 分支（本地 `wiredtiger-src/`）。
> 本版取代 v2 的 P4–P10 编号；全部工作归位为 P0–P4 + Capstone。
>
> **当前仓库状态**：只保留骨架（接口 + 数据结构 + 测试 + 任务书），
> 所有函数体是 `UNIMPLEMENTED("Px/Ty ...")`。任务→测试项的映射见 §10。

---

## 0. 与 15-445 的同构（拆分的总纲）

BusTub 的教学弧线是「底座 → 数据结构 → 让数据可操作 → 并发控制」，
tinywt 逐段对上：

| BusTub (15-445 F24) | tinywt 作业 | 状态 |
|---|---|---|
| **P0** C++ Primer（Trie 热身） | **P0** 基础件热身（varint/Slice/CRC/构建骨架） | ⬜ |
| **P1** Task1 LRU-K Replacer | **P1** Task1 块读写与校验 | ⬜ |
| **P1** Task2 Disk Scheduler | **P1** Task2 extent 三表 + 超级块 | ⬜ |
| **P1** Task3 Parallel Buffer Pool | **P1** Task3 cell 编解码 + 前缀压缩 | ⬜ |
| **P2** Task1 B+Tree Pages | **P2** Task1 内存页三件套 + page_in | ⬜ |
| **P2** Checkpoint 1: Search | **P2** Task2 descend 搜索 | ⬜ |
| **P2** Checkpoint 2: Insert/Delete | **P2** Task3 插入 + 叶/内部分裂（两个分隔符坑） | ⬜ |
| **P2** Task3 Index Iterator | **P2** Task4 reconcile + LRU eviction（iterator 本体挪到 tinywt P3 的 cursor） | ⬜ |
| （BusTub 无对应） | **P2** Task5 checkpoint + 重开恢复 + verify + 读可见性 | ⬜ |
| **P3** Task1 System Catalog | **P3** Task1 URI + 元数据表 + config parser | ⬜ |
| **P3** Task2 Executors | **P3** Task2–4 Cursor 读写遍历 + 事务 API | ⬜ |
| （BusTub 无日志作业） | **P3** Task5 WAL + 崩溃恢复 | ⬜ |
| **P3** Leaderboard | **P3** Leaderboard：真 wtperf 读写混合 ops/s | ⬜ |
| **P4** Task1 Mock Version Chain | **P4** Task2 WT_REF 状态机 + hazard pointer | ⬜ |
| **P4** Task2/3 TxnMgr + GC | **P4** Task3 History Store（版本存储） | ⬜ |
| **P4** rollback/abort | **P4** Task4 未提交 reconcile + RTS | ⬜ |
| **P4** Task4 Watermark | **P4** Task5 时间窗口聚合 + stable watermark | ⬜ |
| **P4** Leaderboard | **P4** Leaderboard：evict-btree-stress | ⬜ |
| （结业） | **Capstone** 全量验收：runner 全家桶 + wt 工具子集 + 真 WT 对照 | ⬜ |

两处 BusTub 没有的东西（WAL、压缩家族）：WAL 补在 P3（它是"操作层"的持久性收口）；
压缩家族补在 P2 的 Task 2.6/2.7（key/value 编码属于"索引结构"层，接口已冻结，
最后做不影响前面）。

**通用规则**（每个作业都按这套机制运转）：

```
Task N         固定接口签名 + 真 WT 源码定位 + 已知的坑；不许多改接口
Checkpoint     作业内里程碑，过 gate 测试才能进下一段（对标 BusTub CP1/CP2）
Leaderboard    可选竞技任务：性能数字进排行榜（对标 BusTub P3/P4 leaderboard）
Written Q      待做作业 → 面试口径书面题；已完成作业 → 复习题（按你单概念复习的习惯）
提交纪律       每 Task 一个 commit："P3-T2: cursor next cross-page"
链接器即评分器  真 wiredtiger.h 原样拷给测试工具编译，tinywt 只实现用到的符号，
               缺功能 = 链接当场缺符号报错，nm/ld 替我们当 grader
```

---

## 1. Project 0 —— 基础件热身（⬜ 待做：见 docs/P0.md）

对标 BusTub P0（C++ Primer）。BusTub 用 Trie 热身，tinywt 用编码件热身。

| Task | 内容 | 评分测试 |
|---|---|---|
| T0.1 | 构建骨架：Makefile（`-MMD -MP` 头依赖陷阱已踩平）、断言框架 | `make` 全绿 |
| T0.2 | varint（LEB128）、Slice、CRC32（IEEE） | p0-test：编解码往返 |

**复习题**：① varint 为什么选 LEB128 而不是固定 4B？在 cell 头里的代价模型？
② CRC 为什么放在 BlockHeader 而不是页尾？

---

## 2. Project 1 —— Block Manager：磁盘块底座（⬜ 待做：见 docs/P1.md）

对标 BusTub P1（Buffer Pool Manager）：独立组件逐个造，再组装。

| Task | 内容（BusTub 对标） | 评分测试 |
|---|---|---|
| T1.1 | 块读写 + BlockHeader{disk_size,checksum} + 损坏检测（← LRU-K Replacer：独立可测组件） | 往返 / 改坏 1 字节必报 CORRUPT |
| T1.2 | extent 三表 alloc/avail/discard + 超级块 + addr cookie（← Disk Scheduler） | free→discard→checkpoint 回收 |
| T1.3 | cell 编解码：short/long key/value、前缀压缩、三地址类型（← Parallel Buffer Pool：组件组装） | cell 往返 + 前缀还原 |

**复习题**：① 为什么 offset 0 非法（首块是描述块）？② alloc/avail/discard 三张表
为什么 discard 必须跨 checkpoint 存活而 avail 不用？③ addr cookie 为什么对 B-Tree
层是不透明字节串？

---

## 3. Project 2 —— Row-Store B-Tree（⬜ 待做：见 docs/P2.md）

对标 BusTub P2（B+Tree Index）。Checkpoints 已按 BusTub 的"先 Search 后 Insert"节奏走过。

| Task | 内容（BusTub 对标） | 评分测试 |
|---|---|---|
| T2.1 | 内存页三件套 entries/update链/insert跳表 + page_in（← B+Tree Pages） | 物化往返 |
| T2.2 | descend 二分下降（← **Checkpoint 1: Search**） | 5000 键全量回读 |
| T2.3 | 插入 + 叶/内部分裂；分隔符两个坑：左半页分隔符漏更新 / 只降不升（← **Checkpoint 2: Insert**） | 对拍 std::map ×4 种子 |
| T2.4 | reconcile + LRU eviction（← Index Iterator 的前置） | cache=3 强制淘汰 1800+ 次仍全对 |
| T2.5 | checkpoint 后序遍历 + 超级块 + 重开恢复 + verify + 读可见性 | 持久化重开 800 键 |
| **T2.6** 🚧 | **编码家族**：suffix compression（`rec_write.c:723`，`internal_key_truncate`）+ overflow page（`WT_CELL_*_OVFL`）+ 字典压缩（`WT_CELL_VALUE_COPY`） | 压缩开关下全量对拍 + 文件体积表 |
| **T2.7** 🚧 | **compressor API**：`add_compressor` + `compression=` 配置；先内建 RLE，再挂真 snappy（`ext/compressors/snappy/`） | 解锁 `compression="snappy"` runner |
| Leaderboard 🚧 | 压缩率 vs ops/s 曲线（snappy / RLE / none 三档） | — |

**复习题（已完成部分）**：① 为什么 internal key 是子树下界而不是精确键？
（答到 suffix compression 的源码依据算满分）② 内存页三件套各自的 reconcile 归并
顺序？③ prefix 与 suffix 压缩分别作用于哪层页、为什么不能互换？

---

## 4. Project 3 —— 数据操作层：C API / Cursor / WAL（⬜ 待做：见 docs/P3.md）

对标 BusTub P3（Query Execution）：从「能存」到「能用」。cursor 就是 tinywt 的
"executor"——每个操作都是一次 cursor 驱动的树遍历。

**真 WT**：`src/include/wiredtiger.h`、`src/config/`、`src/meta/`、
`src/btree/bt_curnext.c / bt_curprev.c / bt_cursor.c`、`src/log/`。

### Task 3.0 —— Primer：合同对齐（对标 BusTub "Before You Begin"）

`compat/wiredtiger.h` 已就位（生成物）；`Status` 与真 WT 错误码**错位 3**，
需在 C ABI 边界做映射（`WT_NOTFOUND=-31803`、`WT_ROLLBACK=-31800`，详见
`docs/P3.md` T3.0）；
`src_c/` 建 extern "C" 层骨架（opaque 句柄包 C++ 内核）。

### Task 3.1 —— System Catalog 的对应物：URI + 元数据 + config（← BusTub P3 Task 1 System Catalog，几乎一一对应）

- `wiredtiger_config_parser_open` + `WT_CONFIG_PARSER::{next,close,get}`：
  语法 `k=v`、嵌套 `()`、引号串、逗号分隔（对标 `src/config/config.c` 词法，~200 行）。
- `table:X` → `file:X.wt` 映射；**元数据用 tinywt 自己的 Btree 存**
  （对标 `WiredTiger.wt` 就是一张表）。
- `session->create` 解析 `leaf_page_max/internal_page_max/split_pct/type=file`
  接进现有 `Config`；多表多 session 管理。
- `wiredtiger_strerror`。

### Task 3.2 —— Cursor 写路径（← executors 的 Insert）

`WT_CURSOR` 函数表：`open/reset/set_key/set_value/insert`；
隐式事务顶住显式事务的位（T3.4 转正）。

> **Checkpoint 1（= T3.0–T3.2）**：hn01 链接真 wtperf，**跑通缩水版 populate**
> （icount 500m→1m，populate_threads=1），链接零缺符号。

### Task 3.3 —— Cursor 读路径与遍历（← SeqScan/Update/Delete executors）

- `next` 状态机：insert 链流与磁盘 entries 流**归并取小**；tombstone/不可见版本
  在归并循环内跳过；页耗尽沿 walk 栈换页。坑：`ins_head[i]` 间隙下标语义
  （i==entries.size() = 页尾）先写进注释。
- `prev` 对称实现（跳表加 prev 数组；真 WT 无 prev 靠重建——Written Q）。
- `search`（精确/下界两态）、`get_key/get_value`、`update/remove`，
  reader snapshot 绑定 cursor open 时刻。

### Task 3.4 —— 事务 API + 增量操作（← 剩余 executors）

`begin/commit/rollback_transaction`；`cursor->modify` + `wiredtiger_calc_modify`
（WT_MODIFY_REC 增量 entries）；`session->truncate`（游标范围删）。

### Task 3.5 —— WAL + 崩溃恢复（BusTub 没有的，补齐持久性收口）

- LogRecord（布局已核实 log.h:311）：`u32 len | u32 checksum | u8 flags
  (COMPRESSED 0x01/ENCRYPTED 0x02) | payload`；`LOG_ALIGN=128`（:51）；
  LSN=`(file<<32)|offset`（:60）；首块 `WT_LOG_DESC{magic,version,allocsize}`。
- 逻辑日志 `TXN_BEGIN / ROW_PUT / ROW_REMOVE / TXN_COMMIT`（对标
  `WT_LOGC_KEY_FORMAT "III"` log.h:83）；**先日志后内存页**的次序写进注释。
- flush（`fdatasync`；单 slot，组提交留可选池 O10）。
- checkpoint 交互：`ckpt_lsn` 入超级块；日志只留 ≥ckpt_lsn。
- recovery：扫日志（checksum 坏/半条即停，尾部截断合法）→ 只重放有 COMMIT
  的事务 → 立刻做一次新 checkpoint。

> **Checkpoint 2（= T3.3–T3.5）**：真 wtperf 跑 `50r50u / 80r20u` 缩水版（单线程组）；
> 再跑 `close_conn=false` 的 crash-恢复链（populate → kill -9 → 重开 → 继续）。

### Leaderboard（P3）

populate / 读写混合 runner 的 ops/s 排行，按 cache_size × 线程数出曲线。
（BusTub P3 leaderboard 同款玩法。）

### 单元测试（p3-test）

全序/反向遍历对拍 std::map；删改混合；seek 三边界；遍历中 split；
日志截断/坏 checksum/未提交丢弃/ckpt 裁剪/随机 crash 对拍（×4 种子）。

### Written Questions

1. WT 为什么把 config 解析做成公开 API（`wiredtiger_config_parser_open`）？
2. 元数据用 B-Tree 存而不是文件头，对崩溃恢复意味着什么？
3. `WT_INSERT` 跳表为什么没有 prev 指针？反向遍历的代价？
4. logical redo vs InnoDB physical redo（cell 前缀压缩为什么让物理重放不可行）？
5. 恢复完为何立刻 checkpoint 而不是直接开服？

---

## 5. Project 4 —— 并发与多版本（⬜ 待做：见 docs/P4.md）

对标 BusTub P4（MVCC）。tinywt 的版本链 = update 链 + History Store，
watermark = RTS 的 stable timestamp——同构到可以直接套题。

**真 WT**：`src/include/hazard.h`、`btmem.h` 的 `WT_REF` 状态机、
`src/history/`（1909 行）、`src/txn/txn_rollback_to_stable.c`。

### Task 4.1 —— 并发 L1（对标 BusTub P2 的 Concurrency task，放这里是依赖顺序）

tree 级 `shared_mutex`；split/checkpoint 独占；多线程对拍（4 线程随机
put/remove/get == std::map 终态，分片校验，×2 种子）。

### Task 4.2 —— 版本链基础设施：WT_REF 状态机 + hazard pointer（← BusTub P4 Task 1 "Mock Version Chain" 位置：先造基础设施）

- hazard pointer 简化版：每 session 固定 16 槽，evict 前全表扫描比对。
- `WT_REF` 状态 `DISK→READING→MEM→LOCKED` 单向 CAS 落进 `Ref`；
  并发 page_in 去重；eviction 抢占 exclusive ref。
- TSan 构建跑干净。

### Task 4.3 —— History Store：版本存储（← BusTub P4 Task 2/3 TxnMgr+GC）

- HS = 另一棵普通 B-Tree（复用 P1），key=`(btree_id, user_key, start_ts降序)`，
  value=旧版本或 tombstone + stop_ts。
- reconcile 写入侧：链上存在"对 checkpoint 快照不可见"的旧版本 → 搬 HS
  （修掉现在"直接丢弃"的行为）。
- 读取下探：内存链无可见版本 → 查 HS（前缀匹配 user_key，取 start_ts ≤ read_ts
  最大者）。

### Task 4.4 —— 回滚语义：未提交 reconcile + RTS（← BusTub P4 的 rollback/abort）

- reconcile 遇未决 update → 保留"磁盘镜像 + 未决修改列表"形态
  （真 WT 的 `WT_PM_REC_MULTIBLOCK / saved updates`）。
- RTS：超级块 dirty_shutdown 标志 → open 时回滚未提交/晚于 stable 的版本。

### Task 4.5 —— Watermark：时间窗口聚合 + stable 点（← BusTub P4 Task 4 Watermark，几乎一一对应）

- page/cell 级 stop_ts 聚合最小版（够 RTS 用；完整 `WT_TIME_AGGREGATE` 位打包
  留可选池 O5）。
- stable timestamp 即全局 watermark：HS 回收、RTS 截断都从它出发。

> **Checkpoint（= T4.1–T4.5）**：真 wtperf 跑 `evict-btree.wtperf` 原配置
> （16 读线程 + eviction=threads_max=8，icount 缩水）+ `long-txn-btree` 缩水版。

### Leaderboard（P4）

`evict-btree-stress` 多线程 ops/s；旧快照长读场景下 p99 不劣化断言。

### 单元测试（p4-test）

旧快照跨 eviction 仍可读；模拟 crash+未提交 → RTS 后一致；全部快照关闭后
HS 版本可回收；读线程慢遍历 + 写线程疯狂淘汰无悬空（ASan）。

### Written Questions

1. eviction 为什么需要 LOCKED 状态而不能直接锁页？
2. hazard pointer 相比 rwlock 的优势（无写者优先问题/无锁读路径）？
3. WT 4.0 为什么用独立 History Store 替换 3.x 的 lookaside table？
4. RTS 为什么从 stable 点回滚而不是最新点？和 MongoDB majority commit point 什么关系？
5. watermark 停滞（有长事务挂着）时，HS 和 cache 各会发生什么？

---

## 6. Capstone —— 结业全量验收（⬜ 待做：见 docs/CAPSTONE.md）

（15-445 没有第五个作业，但这是它的 leaderboard 文化的自然延伸。）

| Task | 内容 |
|---|---|
| C1 | runner 全家桶：`populate / 50r50u / 80r20u / rdonly / evict-btree / evict-btree-stress / checkpoint-stress / insert-rmw / long-txn-btree / index-pareto-btree`（缩水 icount，其余原配置） |
| C2 | wt 工具子集：补 23 个 `__wt_*` 工具类 helper（config_merge/json/buf/getopt），链接真 `wt`，跑 `list / create / drop / dump -x / load / verify / stat` |
| C3 | 对照组：hn01 编译真 WT，同 runner 双跑，出对比表（ops/s、p99、文件大小、checkpoint 时长） |
| C4 | 双 leaderboard 汇总：P3（吞吐）+ P4（淘汰压力）曲线合并成最终报告 |

---

## 7. 真工具依赖面（合同，全部已核实）

### wtperf（7 源文件 + testutil grep 实测）

```
wtperf = bench/wtperf/{wtperf,wtperf_config,wtperf_misc,track,wtperf_throttle,
        wtperf_truncate,idle_table_cycle}.c + test/utility/ 按需(4028行,公开API依赖极少)
公开函数: wiredtiger_open / wiredtiger_strerror /
          wiredtiger_config_parser_open(wtperf 解析配置就调它) / wiredtiger_calc_modify
方法面:   close(18) open_cursor(14) open_session(12) set_key(9) get_key(9)
          set_value(7) create(7) next(6) begin_txn(6) search(5) rollback(5)
          insert(5) reset(4) commit(3) checkpoint(3) update(2) remove(2)
          prev(2) drop(2) truncate(1) modify(1)
无 statistics: 游标；压缩器是 .so 扩展，不跑压缩 runner 不需要
```

### wt 工具（可行性已核实）

`src/utilities/*.c` 只调 **23 个 `__wt_*` 内部函数，全是工具类**
（`__wt_config_merge / __wt_json_* / __wt_buf_* / __wt_getopt`），不碰存储内核。
补齐即可链接真 `wt`。

### 环境与错误码

真工具链在 hn01（Linux, cmake 齐全）跑；本地 Windows/MinGW 保留单测。
**注意**：`twt::Status` 的数值必须与真 WT **逐一对齐**（`S_NOTFOUND = -31803`
= `WT_NOTFOUND`，`S_ROLLBACK = -31800` = `WT_ROLLBACK`），C ABI 边界只做恒等映射。
`compat/probe.c` 用 `_Static_assert` 把这些数字钉死，`make probe` 编译期就能抓到
偏差；`make probe-link` 进一步把缺的接口交给链接器报符号。详见 `docs/P3.md` T3.0。

---

## 8. 可选池（O1–O12，按性价比排序，全量给出）

| # | 主题 | 真 WT 定位 | 规模感 | 解锁什么 |
|---|---|---|---|---|
| O1 | **test/format 式随机压测器** | `test/format/` | 2–3 天 | WT 的 torture test：随机配置+crash+校验，**性价比最高** |
| O2 | 多 checkpoint 保留 + named checkpoint | `block_ckpt.c`、turtle 文件 | 1–2 天 | `checkpoint-stress` 原配置、`wt list -c` |
| O3 | backup 游标 | `src/cursor/cur_backup.c` | 2 天 | backup 系 runner |
| O4 | column-store | `src/btree/col_*.c` | 3–4 天 | `wt` col 命令、O1 覆盖面、面试题 |
| O5 | 完整 `WT_TIME_AGGREGATE` 位打包 | `src/include/` | 1 天 | reconcile 与 WT 完全同构 |
| O6 | statistics 游标 | `src/cursor/cur_stat.c` | 2 天 | `wt stat`、perf_run_py 自动分析 |
| O7 | workgen | `bench/workgen/` | 一周+ | 新 benchmark 框架（不建议先做） |
| O8 | test/suite python 子集 | `test/suite/` | 逐周扩 | 覆盖最广的官方测试（Capstone 后当回归） |
| O9 | LSM tree | `src/lsm/` | 数周 | lsm runner（**不建议**，规模不成比例） |
| O10 | log slot 并发组提交 | `src/log/log_slot.c` | 2 天 | P3-T3.5 回头补真 slot 机制 |
| O11 | salvage | `src/btree/bt_slvg.c` | 3 天 | `wt salvage`（学 block/extent 很好） |
| O12 | 磁盘格式完全对齐真 WT | `block.h/cell.h` 全量 | 长期 | 真 `wt` 直接打开 tinywt 文件（终极彩蛋） |

---

## 9. 执行顺序与开工基建

```
P3（API+Cursor+WAL）──► P4（并发+HS+RTS）──► P2 补 T2.6/T2.7（压缩家族）──► Capstone
   CP1: wtperf populate      CP: evict-btree 多线程         解锁 snappy runner
   CP2: 50r50u + crash 链
```

**开工首日（半天）**：
1. `test/tinywt_test.cpp` 拆 `p0~p2` + `harness.hpp`（随机/对拍/crash 模拟/计时）。
2. `Makefile` 加 `pN-test` 目标矩阵。
3. `compat/wiredtiger.h`（拷真头）+ `src_c/` extern "C" 骨架。
4. hn01：拉 `bench/wtperf` + `test/utility` + `src/utilities` 的编译脚本。
5. 本文件 → `tinywt/docs/PROJECTS.md`；每个 Project 开工立 `docs/pN.md` 任务书。

---

## 10. 骨架 ↔ 测试项映射（`make grade` 的评分单位）

一个测试函数 = 一个评分项。名字里的 `Px/Ty` 就是本文件的任务号。

| 任务 | 评分项（`make grade` 里显示的名字） | 测试文件 |
|---|---|---|
| T0.1 | `P0/T0.1 构建骨架 + 错误码契约` | `p0_defs_test.cpp` |
| T0.2 | `P0/T0.2 varint (LEB128)`、`P0/T0.2 crc32` | `p0_defs_test.cpp` |
| T1.1 | `P1/T1.1 块读写 + 校验` | `p1_block_cell_test.cpp` |
| T1.2 | `P1/T1.2 extent + 超级块` | `p1_block_cell_test.cpp` |
| T1.3 | `P1/T1.3 cell 编解码` | `p1_block_cell_test.cpp` |
| T2.1 | `P2/T2.1 内存页三件套 + page_in` | `p2_btree_test.cpp` |
| T2.2 | `P2/T2.2 descend（5000 键回读）` | `p2_btree_test.cpp` |
| T2.3 | `P2/T2.3 分裂 + 对拍 std::map` | `p2_btree_test.cpp` |
| T2.4 | `P2/T2.4 reconcile + eviction` | `p2_btree_test.cpp` |
| T2.5 | `P2/T2.5 checkpoint + 重开 + verify` | `p2_btree_test.cpp` |
| T2.6 | `P2/T2.6 编码家族（suffix/overflow/dict）` | `p2_codec_test.cpp` |
| T2.7 | `P2/T2.7 compressor API（RLE）` | `p2_compress_test.cpp` |
| T3.0 | `P3/T3.0 strerror 与错误码`、`P3/T3.0 错误码契约（Status -> WT_*）`（2 项，无需写代码） | `p3_api_test.cpp` |
| T3.1 | `P3/T3.1 config parser（真 ABI）`、`P3/T3.1 重开连接：元数据恢复 + 数据仍在` | `p3_api_test.cpp` |
| T3.2 | `P3/T3.1+T3.2 open/create/insert/search`（写路径与 T3.1 串在一条链上测） | `p3_api_test.cpp` |
| T3.3 | `P3/T3.3 cursor 读路径`（6 个用例） | `p3_cursor_test.cpp` |
| T3.4 | `P3/T3.4 …`（5 个用例：读自己的写 / 未提交不可见 / 回滚精确摘链 / 快照隔离 / 写冲突） | `p3_txn_test.cpp` |
| T3.5 | `P3/T3.5 …`（5 个用例：WAL 重放 / 未提交丢弃 / 截断 / 尾部截断 / 中间损坏→CORRUPT） | `p3_wal_test.cpp` |
| T4.1 | `P4/T4.1 并发 L1（4 线程对拍 std::map）` | `p4_concurrency_test.cpp` |
| T4.2 | `P4/T4.2 WT_REF 状态机 + hazard pointer`、`P4/T4.2 读线程 + 疯狂淘汰` | `p4_concurrency_test.cpp` |
| T4.3 | `P4/T4.3 History Store` | `p4_hs_rts_test.cpp` |
| T4.4 | `P4/T4.4 未提交 reconcile + RTS` | `p4_hs_rts_test.cpp` |
| T4.5 | `P4/T4.5 watermark（stable 点）+ 时间窗口聚合` | `p4_hs_rts_test.cpp` |
| C1–C4 | 不计入 `grade`，由 `make capstone` 单独 gate | `scripts/capstone.sh` |

**Checkpoint / Leaderboard 不是评分项**：Checkpoint 是「过了才能进下一段」的
人工 gate（`make cp1` / `make cp2` 两个目标跑对应的 runner 子集）；
Leaderboard 是把数字填进 docs/LEADERBOARD.md，不影响正确性得分。

**评分机制**（`test/harness.hpp` + `scripts/grade.py`）：

- `RUN(fn)` 给每个测试函数套异常防火墙：命中 `UNIMPLEMENTED` 只把这一项记 `TODO`，
  进程继续，后面的项照跑 —— 否则骨架下第一个函数就把进程炸了，没法逐项打分。
- C ABI 层不能跨语言抛异常，所以 `src_c` 用 `UNIMPLEMENTED_RC(what, rc)` 打全局标记，
  harness 结束时读标记，同样记 `TODO` 而不是误报 `FAIL`。
- 每个二进制打印 `RESULT`/`SCORE` 行，`grade.py` 只做聚合，不解释测试语义。
