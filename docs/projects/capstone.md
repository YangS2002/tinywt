# Capstone —— 结业全量验收（C1–C4）

> 单测只能证明语义对；Capstone 证明这套实现能顶住**真工具的用法**：
> 真 `wtperf` 直接 `LD_PRELOAD` 你的 `.so`，真 `wt` 工具打开你写的库。

`make capstone` 调 `scripts/capstone.sh`，逐项跑 C1–C4。任一项 FAIL 则整体退出非 0；
缺依赖的项记 SKIP 并打印补齐方式 —— **SKIP 不算通过，也不假装通过**。

```
make grade        # 判「语义」：每个 Task 一个测试项，NOTIMPL/FAIL/PASS
make cp1 / cp2    # 判「能否进下一段」：单测子集 + probe-link + 真 wtperf
make capstone     # 判「真工具面」：C1 runner / C2 wt / C3 对照组 / C4 汇总
make wt-abi       # 量进度：真工具要的符号 - 我们导出的符号 = 还差几个
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

| 依赖 | 放置位置（默认） | 怎么来 |
|---|---|---|
| 真 WiredTiger 源码 | `external/wiredtiger`（或 `WT_SRC` 指定） | `make wt-tools` |
| `wtperf` 可执行 | `<源根>/build/bench/wtperf/wtperf` | 同上（只编 `wt` / `wtperf` 两个 target） |
| `wt` 可执行 | `<源根>/build/wt` | 同上 |
| tinywt 共享库 | `build/libwiredtiger.so` | `make shared` |

```bash
make wt-tools     # 一次性：浅克隆固定 tag + cmake 构建（约 1 分钟，44MB）
make wt-abi       # 看还缺哪些符号 / 还剩几个空桩
make capstone
```

可用环境变量覆盖：`WT_SRC`、`WTPERF`、`WT_BIN`、`ICCOUNT`、`WT_TIMEOUT`。

> **`wt-tools` 为什么要 clone 而不是下载几个文件**：wtperf 要链 `libwiredtiger`，
> 还要 `bench/wtperf/runners/` 下 104 个 `.wtperf` 配置（C1 用的 10 个就在里面）。
> 只抓 `.c` / `.h` 是编不出二进制的 —— 旧版 `fetch_wt_sources.sh` 就是这么失效的。
> 另外 WT 的 `cmake/configs/base.cmake` 无条件 `find_package(Python3 ... REQUIRED)`，
> 没有 `python3-dev` 的机器会配不过去；`wt_tools.sh` 会打一个幂等补丁把这次探测降级为
> 可选（只影响 python 绑定，`wt` / `wtperf` 不需要），不动系统、不装包。

> **每个外部调用都有超时**（`WT_TIMEOUT`，默认 300s）。原因不是"怕慢"，是空桩会**挂死**：
> `__wt_thread_create` 返回错误但没建线程时，wtperf 的主线程会永远等 worker 出现。
> 没有超时，整条 capstone 会吊在那儿，你只会以为"还在跑"。

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

验收：每个 runner 退出码 0，且日志里能解析出 `N ops/sec`，且在 `WT_TIMEOUT` 内结束。

> **runner 文件名不统一**（踩过）：10 个里 6 个就叫 `evict-btree.wtperf`，另外 4 个
> （`populate` / `50r50u` / `80r20u` / `rdonly`）带规模前缀 `500m-btree-`。
> 脚本用 `resolve_runner` 依次试 `$name.wtperf` → `500m-btree-$name.wtperf` → `*-$name.wtperf`。
> 直接拼 `$name.wtperf` 的结果是这 4 个静默返回 `-`，看起来像实现错了。

## C2 —— wt 工具子集

```
wt list / create / verify / stat / drop / dump -x / load
```

真 `wt` 要 **20 个 `__wt_*`**（`nm -D --undefined-only` 实测，不是估的），
分四组：平台胶水 14 / 缓冲格式化配置 10 / JSON 5 / 内核元数据与日志 5。
加上 wtperf 另外要的 18 个、扣掉 4 个重叠，总共 **34 个**，都在
`src/api/wt_tool_abi.cpp`（T3.6）。签名取自预处理后的真实声明，别凭记忆改。

其中 5 个**确实碰存储内核**：`__wt_block_ckpt_decode`（checkpoint 地址 cookie）、
`__wt_metadata_get_ckptlist` / `__wt_metadata_free_ckptlist`（元数据表）、
`__wt_txn_printlog`（WAL 解码）、`__wt_copy_and_sync`。C2 的难点就在这里 ——
它把你 P2 写的 checkpoint 格式拉到真 WT 的字节级标准。

验收：**每条子命令的退出码 + 输出内容都要对**，不是"exit 0 就算过"。
真 `wt` 在参数解析失败时会打印 usage 然后退出，光看退出码会把"什么都没做"记成通过：

| 步骤 | 断言 |
|---|---|
| `list`（空库） | 不打印 usage 段 |
| `create table:t` | 不打印 usage 段 |
| `list` | 输出里**有** `table:t`（写进去了的唯一证据） |
| `verify table:t` | 不打印 usage 段 |
| `stat table:t` | 有 `名称: 数值` 形式的统计行（空输出 = 命令被吞了） |
| `drop table:t` | 不打印 usage 段 |
| `list` | 输出里**没有** `table:t`（drop 不是空操作） |
| `dump -x` | 有成对的十六进制字节行（字节级对齐的唯一验收方式） |

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
- [ ] `make wt-abi` 缺口 0 **且** 0 个空桩（两个条件都要）
- [ ] C1 十个 runner 全部退出码 0
- [ ] C2 `wt` 七条子命令全部通过
- [ ] C3 对比表已生成（ratio 有数即可，不设阈值）
- [ ] C4 `docs/LEADERBOARD.md` 已更新

## 常见失败

| 现象 | 原因 |
|---|---|
| `LD_PRELOAD` 生效但立刻 segfault | `wiredtiger.h` 的结构体布局对不上（`iface` 必须是首成员） |
| `undefined symbol: __wt_config_merge` | 工具面 ABI 没编进去：确认 `src/api/wt_tool_abi.cpp` 在源列表里 |
| `make wt-abi` 报缺口 0，但 `wt list` 只打印 usage | 符号齐了但 `__wt_getopt` 还是空桩 —— **符号齐 ≠ 功能对** |
| 某个 runner 跑满 `WT_TIMEOUT` 没动静 | 多半是 `__wt_thread_create` 空桩：没建线程，主线程死等 worker |
| `wt` 报 `WT_TRY_SALVAGE` / `database corruption` | 超级块读失败：`SBF_*` 标志位或 `alloc_size` 与块管理器不一致 |
| runner 全 FAIL 但单测全绿 | 单测走的是自己构造的 Config，runner 走 `.wtperf` 里的 `conn_config`，config 解析没接上 |
| `dump -x` 输出乱码 | cell 编码与真 WT 不一致 —— 这是唯一必须**字节级**对齐的地方 |
| `wt` 打印 usage 且 exit 0 | 参数没被识别（`__wt_getopt` 空桩），C2 靠"输出内容断言"抓这种假通过 |
| `cmake 配置失败：Could NOT find Python3` | 未打 `wt_tools.sh` 那个补丁；直接 `WT_CLEAN=1 make wt-tools` 重来 |
