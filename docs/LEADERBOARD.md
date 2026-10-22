# Leaderboard

> **正确性由 `make grade` 判，性能数字记在这里。**（作业形式上的 leaderboard）
> 这里不进评分，但它是「优化有没有用」的唯一证据 —— 改完 eviction 策略、
> 换个压缩器，必须能在这张表里看出差别，否则就是自嗨。

## 记录规则（不遵守这张表就没意义）

1. 同一台机器、同一次构建、同一个 `icount` 才能横向比。
2. 每组跑 3 次取**中位数**，别取最好那次。
3. 表里必须写清楚：`cache_size`、线程数、压缩器、git commit。
4. `make capstone` 会自动把 C1 的结果**追加**到本文件末尾（不要手改那一段）。

## P3 榜 —— 吞吐（ops/s）

真 wtperf，`icount` 缩水版。看的是「cursor 读路径 + 写路径」的综合效率。

| runner | cache_size | 线程 | ops/s | 备注 / commit |
|---|---|---|---|---|
| populate | 512M | 1 | — | 待填 |
| 50r50u | 512M | 4 | — | 待填 |
| 80r20u | 512M | 4 | — | 待填 |
| rdonly | 512M | 4 | — | 待填 |

## P4 榜 —— 淘汰压力（ops/s）与尾延迟

`evict-btree` / `evict-btree-stress`：页几乎活不过一次 descend，考的是
hazard pointer + 淘汰路径的成本。

| runner | cache_size | 读线程 | eviction 线程 | ops/s | p99 | 备注 |
|---|---|---|---|---|---|---|
| evict-btree | 512M | 16 | 4 | — | — | 待填 |
| evict-btree-stress | 256M | 16 | 8 | — | — | 待填 |
| long-txn-btree | 512M | 8 | 4 | — | — | watermark 停滞场景 |

**P4 的硬约束**：旧快照长读场景下 p99 不能比「没有长事务」时劣化一个数量级。
劣化了说明 watermark 把 HS 拖爆了（见 `docs/projects/p4_concurrency_mvcc.md` T4.5）。

## 压缩率 vs 吞吐（P2-T2.6/T2.7）

同一份数据，三种编码档位：

| 配置 | 文件大小 | populate ops/s | 读 ops/s |
|---|---|---|---|
| 全部关闭 | — | — | — |
| 默认（suffix + dict） | — | — | — |
| 默认 + overflow | — | — | — |
| RLE 压缩 | — | — | — |

## 对照表（C3，真 WT vs tinywt）

| runner | real-WT ops/s | tinywt ops/s | ratio |
|---|---|---|---|
| 50r50u | — | — | — |
| evict-btree | — | — | — |

ratio 不设及格线：学习实现慢 1–2 个数量级是正常的，有意义的是**它随改动怎么变**。
