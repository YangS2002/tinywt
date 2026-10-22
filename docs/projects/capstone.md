# Capstone —— 结业全量验收（C1–C4）

> 单测只能证明语义对；Capstone 证明这套实现能顶住**真工具的用法**：
> 真 `wtperf` 直接 `LD_PRELOAD` 你的 `.so`，真 `wt` 工具打开你写的库。

`make capstone` 调 `scripts/capstone.sh`，逐项跑 C1–C4。任一项 FAIL 则整体退出非 0；
缺依赖的项记 SKIP 并打印补齐方式 —— **SKIP 不算通过，也不假装通过**。

```
make grade        # 判「语义」：每个 Task 一个测试项，NOTIMPL/FAIL/PASS
make cp1 / cp2    # 判「能否进下一段」：单测子集 + probe-link + 真 wtperf
make capstone     # 判「真工具面」：C1 runner / C2 wt / C3 对照组 / C4 汇总
```

三者的关系：

```
        P0 ──▶ P1 ──▶ P2 ──▶ P3 ──┬─ cp1 (T3.0–T3.2)
                                  ├─ cp2 (T3.3–T3.5)
                                  ▼
                                 P4 ──▶ capstone (C1–C4)
```

---

## 前置条件

| 依赖 | 放置位置（默认） | 来源 |
|---|---|---|
| 真 WiredTiger 源码 | `third_party/wt`（或 `external/wiredtiger`） | `scripts/fetch_wt_sources.sh` |
| `wtperf` 可执行 | `$WT_SRC/build/bench/wtperf/wtperf` 等（脚本会按候选路径找） | WT 官方 cmake 流程 |
| `wt` 可执行 | `$WT_SRC/build/wt` | 同上 |
| tinywt 共享库 | `build/libwiredtiger.so` | `make shared` |

可用环境变量覆盖：`WT_SRC`、`WTPERF`、`WT_BIN`、`ICCOUNT`。

> 两个脚本（`checkpoint.sh` / `capstone.sh`）默认按「`third_party/wt` 存在就用它，
> 否则用 `external/wiredtiger`」决定源码根目录，再在几个常见位置里找二进制。
> 完整克隆时 `WT_SRC=/path/to/wiredtiger` 显式指定更省事。

### drop-in 是怎么生效的

真 `wtperf` / `wt` 链接的是 `libwiredtiger.so`。我们不去改它的源码，而是：

```
LD_PRELOAD=$PWD/build/libwiredtiger.so  $WT_SRC/build/bench/wtperf/wtperf -O ...
```

动态链接器会优先用我们的 `.so` 解析 `wiredtiger_open` 等符号。
**这是最强的接口验收**：少一个符号、错一个结构体布局，进程当场起不来，
不需要任何测试断言来"证明"。

> 共享库是**独立编译**的第二个 target（`wiredtiger`），不是从静态库拼出来的：
> 静态库链进 `.so` 时未被引用的 `.o` 会被丢掉，`LD_PRELOAD` 就会缺符号。
> 所以 `make shared` 会把这十几个 `.cpp` 再编一遍（`-fPIC`），产物在 `build/libwiredtiger.so`。

---

## C1 —— runner 全家桶

10 个 runner，只把 `icount` 缩到 `ICCOUNT`（默认 200000），其余保持原配置 ——
改别的参数结论就不可比了。

| runner | 压的是什么 |
|---|---|
| `populate` | 纯插入：split + reconcile 的稳态 |
| `50r50u` / `80r20u` | 读写混合：update 链长度 + 淘汰 |
| `rdonly` | 只读：cache 命中路径 |
| `evict-btree` | 淘汰压力（16 读线程 + eviction 线程池） |
| `evict-btree-stress` | 淘汰极限：页几乎活不过一次 descend |
| `checkpoint-stress` | checkpoint 与写并发 |
| `insert-rmw` | 读-改-写：先 get 再 update |
| `long-txn-btree` | 长事务挂着 → watermark 停滞 → HS 堆积 |
| `index-pareto-btree` | 倾斜键分布：热点页反复 split |

验收：每个 runner 退出码 0，且日志里能解析出 `N ops/sec`。

## C2 —— wt 工具子集

```
wt list / create / verify / stat / drop / dump -x / load
```

真 `wt` 只调用 23 个 `__wt_*` **工具类**内部函数（`__wt_config_merge` /
`__wt_json_*` / `__wt_buf_*` / `__wt_getopt`），不碰存储内核。
补齐这 23 个即可链接 —— 这是 §7 里核实过的结论。

验收：每条子命令退出码 0；`dump -x` 能打出可解析的十六进制转储。

## C3 —— 对照组

同一个 runner、同一个 `icount`，跑两遍：

```
real-WT  =  $WTPERF -O runner.wtperf -o icount=N       （不 preload）
tinywt   =  LD_PRELOAD=build/libwiredtiger.so $WTPERF ... （preload）
```

输出对比表（`ratio = tinywt / real-WT`）。这里**不设及格线**：
学习实现慢 1–2 个数量级是正常的，有意义的是「ratio 随 P3/P4 的优化怎么变」。
真正该看的是**功能等价性**：同样的 runner、同样的 icount，数据必须一样。

## C4 —— 汇总

把 C1/C3 的数字追加进 `docs/LEADERBOARD.md`：

- P3 榜：populate / 读写混合的 ops/s，按 `cache_size × 线程数` 出曲线
- P4 榜：`evict-btree-stress` 的 ops/s，以及「旧快照长读」场景下 p99 不劣化的断言

表格是给人看的，不进 `make grade`。

---

## 验收清单

- [ ] `make grade` 全绿（P0–P4 每个测试项 PASS，无 NOTIMPL）
- [ ] `make cp1` / `make cp2` PASSED
- [ ] `make probe-link` 零 undefined reference
- [ ] `make shared` 产出 `build/libwiredtiger.so`
- [ ] C1 十个 runner 全部退出码 0
- [ ] C2 `wt` 七条子命令全部通过
- [ ] C3 对比表已生成（ratio 有数即可，不设阈值）
- [ ] C4 `docs/LEADERBOARD.md` 已更新

## 常见失败

| 现象 | 原因 |
|---|---|
| `LD_PRELOAD` 生效但立刻 segfault | `wiredtiger.h` 的结构体布局对不上（`iface` 必须是首成员） |
| `undefined symbol: __wt_config_merge` | C2 的 23 个工具 helper 还没补 |
| `wtperf` 卡在 `wiredtiger_open` | 超级块读失败：`SBF_*` 标志位或 `alloc_size` 与块管理器不一致 |
| runner 全 FAIL 但单测全绿 | 单测走的是自己构造的 Config，runner 走 `.wtperf` 里的 `conn_config`，config 解析没接上 |
| `dump -x` 输出乱码 | cell 编码与真 WT 不一致 —— 这是唯一必须**字节级**对齐的地方 |
