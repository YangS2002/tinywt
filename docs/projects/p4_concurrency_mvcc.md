# P4 任务书 —— 版本、时间与并发

> WT 的版本与并发（`src/history/` + `src/txn/txn_rollback_to_stable.c` +
> `src/support/hazard.c`）：旧版本什么时候搬进 History Store、旧快照怎么下探、
> 崩溃后怎么按 stable 回滚、读路径凭什么不持锁还不悬空。
> 总纲与评分见 [PROJECTS.md](../PROJECTS.md)。
> 验收：7 个测试项。
>
> | 子项目 | 内容 | 函数 | 评分项 |
> |---|---|---|---|
> | T4.1 | 并发 L1：tree 级读写锁（跨切面，不新增函数） | 0 | 1 |
> | T4.2 | WT_REF 状态机 + hazard pointer | 8 | 2 |
> | T4.3 | History Store | 6 | 2 |
> | T4.4 | 未提交 reconcile + RTS | 3 | 1 |
> | T4.5 | Watermark：时间窗口聚合 + stable 点 | 1 | 1 |
>
> 别把这层当成「MVCC 作业」：难点是**三层时间戳**（stable / pinned / oldest）
> 与**旧版本落盘**，版本链本身反而是最简单的部分。
>
> **注意**：事务 API 与 WAL 在 P3（T3.4 / T3.5）。P4 建立在它们之上：
> `TxnManager::visible()` 写错了，P4 的每一个测试都会以「值莫名其妙不对」的形式失败。

## 前置

```bash
make cp2         # T3.3–T3.5 全部 PASSED
make p3-test
```

## 为什么这一层值一个 Project

P2 做完之后，一棵树能存能取，但是：

```
  1. 两个线程同时 put  →  split 中途被别人看到半成品
  2. 读者在遍历时，淘汰线程把页 free 了  →  use-after-free
  3. 改同一个键 1000 次  →  update 链 1000 个版本，内存爆掉
  4. 崩溃重开  →  一半的版本是「未提交」的，必须一致地丢掉
```

P4 就是这四件事，一件一个 Task。

---

## T4.1 并发 L1：tree 级读写锁（1 项）

> **真 WT**：WT **没有** tree 级读写锁 —— 它靠页级 `WT_PAGE_LOCK` + hazard pointer（`src/include/btmem.h`）。这里的 tree 级 `shared_mutex` 是本版简化，只为先让多线程不崩。

最低配的并发：`Btree::latch()` 是一把 `std::shared_mutex`。

```
读（get / scan / cursor_*）        → shared_lock
写（put / remove / checkpoint /
    split / eviction）             → unique_lock
```

### 两条不能违反的规则

1. **公开入口加锁，内部实现假定已持锁。**
   `put()` 里加锁，`split_leaf()` 里**不加**。否则 split 在持有 unique 锁的
   情况下再来一次 unique → 自锁（`std::shared_mutex` 不可重入，会直接死锁）。

2. **不要在 shared 区里触发会拿 unique 的操作。**
   典型陷阱：`get()` 拿着 shared 锁 → `page_in()` 发现 cache 满了 →
   `evict_if_needed()` 想拿 unique → 死锁。
   解法：淘汰调用点放在写路径或独立淘汰线程；或把「本次操作要淘汰」记下来，
   在释放读锁之后再做。

### 测试在考什么

4 个线程、互不相交的键区间、随机 put/remove/get，1200 次/线程，×2 种子。
每个线程维护自己的 `std::map` 当期望值（区间不相交 → 不需要跨线程同步），
结束后全量对拍。要求 `stats().splits > 0` —— 否则测试等于什么都没测。

单线程跑得对、多线程跑错，典型原因就是「锁加在了内部函数上」或
「读路径漏了锁」。后者在 x86 上常常几百次才崩一次，用 `-fsanitize=thread` 抓。

---

## T4.2 版本链基础设施：WT_REF 状态机 + hazard pointer（2 项）

### 问题

```
  线程 A：page_in(ref) 拿到 Page* p，开始读
  线程 B：evict 选中 p，reconcile + 析构 p
  线程 A：继续读 p  ──▶ use-after-free
```

加锁能解决，但读路径会被写者阻塞（而且 `shared_mutex` 在 Linux 上写者优先，
读多了会饿死写者）。hazard pointer 的思路：

> 读之前先把自己的指针登记到一个全局可见的地方；
> 淘汰方在析构任何页之前扫一遍所有登记点，有人在用就不淘汰。

读路径因此只有**一次 store**，完全无锁；代价是 eviction 要扫描。

### WT_REF 状态机

```
   读入：  DISK ──CAS(DISK→LOCKED)──▶ LOCKED(reading) ──读完──▶ MEM
   淘汰：  MEM  ──CAS(MEM→LOCKED)───▶ LOCKED ──reconcile──▶ DISK
   作废：  DELETED（fast-delete 后）/ SPLIT（父页分裂后）
```

真 WT 只有这五个状态（`src/include/btmem.h:1093-1097`）：
`DISK(0) / DELETED(1) / LOCKED(2) / MEM(3) / SPLIT(4)`。
**没有 READING 状态** ——「正在读入」是 `WT_REF_FLAG_READING` 标志位
（`btmem.h:1086`），不是状态。`src/btree/bt_read.c:112` 先把状态 CAS 成
`LOCKED`，`bt_read.c:129` 再置 `WT_REF_FLAG_READING`，读完才 `set` 成 `MEM`。

为什么需要 `LOCKED` 而不直接锁页：eviction 要先把页从「可被别人 page_in」的
状态**摘出来**，否则会出现「A 正在 page_in、B 也在 page_in」的重复实例化。
真 WT 用 CAS 抢，赢的人才拥有这个 ref；`reading` 标志用来区分
「page_in 的赢家」和「eviction 的赢家」。

本作业用 `Ref::state`（`include/tinywt/storage/page.h`）+ 一把内部小锁做等价的事；
测试通过 `Btree::root_ref_state()` 观察迁移：

```cpp
bt.open(path);                  // 只建 Ref，不读页
CHECK(bt.root_ref_state() == REF_DISK);
bt.get(k, v);                   // 第一次 descend 才 page_in
CHECK(bt.root_ref_state() == REF_MEM);
```

### hazard pointer 的三条内存序要求

```
写方（发布页）：  写完 page 内容  →  store(release) 登记指针
读方（拿指针）：  load(acquire) 读到指针  →  才能访问 page 内容
淘汰方（回收）：  load(acquire) 扫不到  →  才能 free
```

三个方向都不能用 `relaxed`。另外 `attach`/`detach` 与 `any_holds` 并发时，
不能漏掉「正在 set 的指针」—— 真 WT 用链表 + 原子插拔。

`any_holds` 的假阳性（多扫到）只会让淘汰变慢；假阴性（漏扫）就是 use-after-free，
所以「宁可保守」。

---

## T4.3 History Store（2 项）

### 为什么必须有

```
  k1 改了 1000 次 → update 链：
     [v1000] → [v999] → ... → [v1] → [base]
       ▲
       读要沿链逐个判可见性；reconcile 时整页重写

  但旧版本不能直接删 —— 还有旧快照在读 v500
```

所以需要一个「安全水位」之下的版本搬家机制：

```
  update 链（热）                        History Store（冷）
  [v1000] → [v999]                        (btree_id, k1, ts=500) → v500
     ▲                                    (btree_id, k1, ts=400) → v400
     oldest 水位之后的留在链上              oldest 之前的搬到这里
```

### key 布局

真 WT 的 HS 是一棵普通 B-Tree（`WiredTigerHS.wt`），
key = `(btree_id, user_key, start_ts 降序)`，value = 版本或 tombstone + stop_ts。

`btree_id` 必须在 key 里：HS 是全库共用的一棵树，不同表的同名 key 不能串味。
`start_ts` 降序：这样「找 <= read_ts 的最新版本」就是一次 seek 加顺序下扫。

### 写入侧：不是「旧就搬」

判据是「**所有活跃读者都看不到**」，也就是 `oldest = min(stable, pinned)`，
不是 `stable_ts`。`reconcile_with_history()` 就是：

```cpp
bt.extract_old_versions(wm.oldest(), [&](const Key &k, const Update &u) {
    hs.insert(btree_id, k, TimeWindow{u.start_ts, u.stop_ts, u.durable_ts},
              u.data, u.is_tombstone());
}, moved);
```

用回调而不是让 `Btree` 直接依赖 `HistoryStore`，是为了避免
`tinywt/btree/btree.h` 与 `tinywt/txn/history_store.h` 互相 include（循环依赖）。

### 读取下探

内存链上没有可见版本时，带着 `read_ts` 去 HS 查：

```
HS 找到 start_ts <= read_ts 的最大版本 → 返回
HS 没有                              → 回落到磁盘镜像里的 base
HS 命中 tombstone                    → found = false（键在那时被删了），
                                        不能再往下找更老的版本
```

第三条最容易写错：tombstone 是「删除」这个事实本身，不是「没有数据」。

### Btree 怎么够到 HS：`HistoryReader` 钩子

`Btree` 不能 include `tinywt/txn/history_store.h`（`HistoryStore` 反过来要用 `Btree` 取页），
所以 `tinywt/btree/btree.h` 里只声明一个抽象基类：

```cpp
class HistoryReader {
public:
    virtual Status read(const Key &key, u64 read_ts, Val &out, bool &found) = 0;
};

bt.set_history_reader(&hs);   // 接线；不接就退化成「只有内存 + base」
```

`Btree::get` 的回落顺序因此被钉死为三步（顺序写错就测试不过）：

```
1. 内存 update 链有可见版本         → 返回它（最快的路径，绝大多数读走这里）
2. 链上没有可见版本，且没有 base 行  → history_reader_->read(key, read_ts, ...)
3. 链上没有可见版本，但有 base 行    → 用 base（本版 base 不带时间戳）
```

第 2、3 步的分界靠「base 行存不存在」：只有 **insert-only 的键**（从没被
checkpoint 进 base）才可能整条链都在 HS 里。这是本版的简化 —— 真 WT 的
base 页带 `start_ts/stop_ts`，所以任何键都能靠 base 判断「更老的还有没有」。

钩子为空时 `get` 只能走第 1、3 步，测试 `P4/T4.3 旧快照下探（HS 钩子接线）`
用 `FakeHistory` 记录调用次数和收到的 `read_ts`，直接验证接线接对了：

| 断言 | 说明 |
|---|---|
| 新快照（`read_ts=20`）不碰 HS | 内存链有可见版本 → `calls == 0` |
| 旧快照（`read_ts=10`）必须下探 | `calls == 1` 且 `last_ts == 10`（传的是读快照，不是 stable） |
| 下探结果对 | 返回的是 HS 里 `start_ts <= 10` 的最新版本，不是最老的 |
| 摘掉钩子 → `NOTFOUND` | 不能 panic，也不能返回脏版本 |

---

## T4.4 未提交 reconcile + RTS（1 项）

### reconcile 遇到未决 update

淘汰要写磁盘镜像，但链头可能是「未提交」的版本。真 WT 的解法是
`WT_PM_REC_MULTIBLOCK` + saved updates：磁盘镜像只写已提交的部分，
未决的修改挂在一个「重建时再挂回去」的列表里。

不做这件事的后果：未提交的数据被 checkpoint 落盘 → 崩溃后无法区分
「已提交」和「没提交」→ RTS 没有依据。

### RTS：从 stable 点回滚

```
  stable_ts = 50

  ts=10  ──── 保留（所有副本都保证有）
  ts=100 ──── 回滚（崩溃时无法保证一致，只能一致地丢弃）
```

规则：把所有 `start_ts > stable_ts` 的 Update 摘掉；链摘空后回落到 base；
如果键只存在于 insert 跳表里，整个键删掉。

两个必须成立的附加性质：

1. **幂等**：再回滚一次不能把 stable 版本也啃掉。
   测试里连做两次 `rollback_to_stable(bt, hs, 50)`，值必须还是 `stable`。
2. **边界是 `>` 不是 `>=`**：`start_ts == stable_ts` 的版本要留。

### dirty_shutdown 标志

超级块里的 `SBF_DIRTY_SHUTDOWN`：干净关闭时清掉，异常退出时还在。
`open()` 看到它就说明「上次没走完 close 流程」，必须先 RTS 再对外服务。

```cpp
Status recover_after_crash(Btree &bt, HistoryStore &hs, u64 stable_ts);
// 有脏标志 → 做 RTS；没有 → 什么都不做。两种情况都返回 S_OK。
```

---

## T4.5 Watermark：时间窗口聚合 + stable 点（1 项）

> **真 WT**：`src/include/txn.h` 的 `WT_TXN_GLOBAL`（`stable_timestamp` / `pinned_timestamp`）—— 本版的 `oldest = min(stable, pinned)` 就是这两个的比较；checkpoint 推进 stable 在 `src/txn/txn_ckpt.c`。

### 三根水位线

```
  stable_ts  ──「所有副本都保证有」的时间点。RTS 从它回滚，日志从它截断。
  pinned_ts  ──「最老的活跃读者」。
  oldest_ts  ── 两者取小，是真正的回收水位。

      oldest = min(stable, pinned)
```

```
   ts  ─────────────────────────────────────────▶
   0        10         30          50
            │          │           │
            │          │           stable
            │          │
            │          可回收（< oldest 的旧版本进 HS）
            │
           长事务的 read_ts = pinned → 水位被钉在 10
```

### 长事务为什么会「拖住」一切

```
  长事务 T 从 ts=10 开始读，一直不结束
      → pinned = 10
      → oldest = 10
      → ts=30 的旧版本永远不能回收
      → HS 持续增长、update 链持续变长、eviction 越来越难找到干净页
```

这是 MongoDB / WT 运维里「一个忘记提交的长事务拖垮整个实例」的机制。
测试用最小的方式复现它：HS 里放一个 ts=30 的版本，`oldest=10` 时
`evict_older_than(10)` 之后版本数不变；把 pin 推进到 50，版本立刻可回收。

### 时间窗口聚合

`aggregate_stop_ts(page, agg)` 把一页里所有 update 的窗口合成一个，
让 reconcile / RTS 不用逐行判：

```
  agg.start_ts   = 最小 start_ts     （窗口必须覆盖最老的版本）
  agg.stop_ts    = 最大 stop_ts      （覆盖最晚的失效点）
  agg.durable_ts = 最小 durable_ts   （最保守的持久化点）
  agg.prepared   = 有任何一个 prepared
```

**为什么是「最小 start / 最大 stop」**：聚合窗口必须**覆盖**所有成员。
用最大值当 start 会漏掉更老的版本 —— 漏判等于数据错，宁可保守。

---

## 建议顺序

```
T4.5（只要 Watermark::oldest 和 aggregate_stop_ts，先让水位有定义）
  → T4.3（HS 是 T4.4 的存储）
  → T4.4（RTS 依赖 HS 里的老版本）
  → T4.1（并发锁，独立，随时可做）
  → T4.2（hazard，最难，放最后；先把 T4.1 的锁跑对再拆锁）
```

## 评分测试

| 测试 | 覆盖 |
|---|---|
| `test/p4/concurrency_test.cpp` | T4.1 四线程对拍、T4.2 hazard + 状态机 + 淘汰压力 |
| `test/p4/hs_rts_test.cpp` | T4.3 HS 读写与回收、T4.4 RTS、T4.5 watermark 与窗口聚合 |

## 设计问题（做完再答）

1. eviction 为什么需要 `LOCKED` 状态，而不能直接锁住页？
2. hazard pointer 相比 rwlock 的优势是什么？（两个角度：写者优先、读路径成本）
3. WT 4.0 为什么用独立 History Store 替换 3.x 的 lookaside table？
4. RTS 为什么从 stable 点回滚而不是最新点？和 MongoDB 的 majority commit point 是什么关系？
5. watermark 停滞（有长事务挂着）时，HS 和 cache 分别会发生什么？为什么最终是「慢」而不是「错」？
