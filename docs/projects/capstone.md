# Capstone —— 结业全量验收（C1–C4）

> 单测只能证明语义对；Capstone 证明这套实现能顶住**真工具的用法**：
> 真 `wtperf` 与真 `wt` 二进制，链接的却是你写的 `libwiredtiger.so`（同 SONAME 直接替换，
> 不是 `LD_PRELOAD` —— 原因见下面「drop-in 是怎么生效的」）。

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

### drop-in 是怎么生效的（以及为什么不能用 `LD_PRELOAD`）

真 `wtperf` / `wt` 的可执行文件里写死了 `NEEDED [libwiredtiger.so.11.3.0]`。
于是有两种做法，**结论完全不同**：

```
（错）LD_PRELOAD=$PWD/build/libwiredtiger.so  $WT_SRC/build/wt ...
      → 真库仍然被加载。我们没导出的 __wt_* 会被它静默兜底，
        "跑通了"可能是一半我们的实现 + 一半真 WT。实测 __wt_getopt 就绑到真库。

（对）LD_LIBRARY_PATH=$PWD/build  $WT_SRC/build/wt ...
      → 只要我们的 .so 与真库同名同 SONAME，ld.so 就只加载我们的，
        真库根本不进场；缺一个符号就是硬 undefined symbol。
```

所以 CMake 里 `wiredtiger` target 的 `SOVERSION` 特意对齐成 `11.3.0`
（产物 `build/libwiredtiger.so.11.3.0`，`build/libwiredtiger.so` 是指向它的符号链接）。
脚本里由 `wt_setup_dropin`（`scripts/lib_wt.sh`）自动判断当前该用哪种方式，
并在 C1/C2 开头打印一行「库模式」——看到 `replace` 才算数，看到 `preload`
说明 SONAME 没对上，那次结果不可信。判完还会用 `wt_check_dropin` 问 `ldd`
**实际解析到哪个文件**，对不上直接判 FAIL 并停手。

> ★ **`wt_setup_dropin` 只能在当前 shell 里调用，不能写成 `$(...)`。**
> 命令替换开子 shell，`WT_ENV` 一赋值就随子 shell 消失，调用方拿到空数组，
> 工具于是照可执行文件里的 `RUNPATH` 找到真库 —— 全绿，但跑的是真 WT。
> 这个坑踩过一次：C1/C2 全过，只有第一行 `wt list` 因为目录不存在而失败，
> 破绽小到几乎看不见。现在有三道防线：函数改成只设全局不打印（说明文字走
> `wt_dropin_desc`）、`wt_run_wtperf` 在 `WT_ENV` 为空时拒绝运行、
> `scripts/check_docs.py` 里有一条检查专门禁止 `$(wt_setup_dropin`。

**这是最强的接口验收**：少一个符号、错一个结构体布局、数据对象尺寸不对，
进程当场起不来，不需要任何测试断言来"证明"。

> 共享库是**独立编译**的第二个 target（`wiredtiger`），不是从静态库拼出来的：
> 静态库链进 `.so` 时未被引用的 `.o` 会被丢掉，替换时就会缺符号。
> 所以 `make shared` 会把这十几个 `.cpp` 再编一遍（`-fPIC`）。

---

## C1 —— runner 全家桶

10 个 runner，每个都跑真实的 `.wtperf` 配置。但**不能只缩 `icount`** ——
原配置在这台机器上有四件事会直接让"跑不起来"，跟实现对不对没关系：

| 参数 | 原值 | 改成 | 不改会怎样 |
|---|---|---|---|
| `icount` | 500000000 | 200000 | 灌一次几十小时 |
| `run_time` | 7200（2 小时） | 20 | 一条 runner 跑两小时 |
| `warmup` | 120 | 5 | 缩了 `run_time` 后预热比跑的时间还长 |
| `*_interval` | `report` 10 / `sample` 10 / `checkpoint` 60 / `backup` 0 | 2 / 2 / 5 / 0 | wtperf 有条硬校验：**任何 interval 长于 `run_time` 就直接报 `interval value longer than the run-time`，一条都不跑**。`checkpoint-stress` 的 `checkpoint_interval=60` 就撞在这条上 |
| `compression` | `snappy` | `none` | 本机没编出 snappy 扩展 → 连接时 `__wt_dlopen` 失败；而且本项目只实现 `NONE`/`RLE` |
| `verbose` | 1 | 2 | 这个构建上 `verbose=1` **一个字都不打**，`load ops/sec` 也没有 |

这些是环境约束，不是"随便调小"。`scripts/lib_wt.sh` 的 `wt_shrink_opts` 一处生成，
C1 / C3 / `checkpoint.sh` 三处共用。改判断之前先跑 `make capstone-selftest`。

还有两件 runner 自己的事，不看配置就会踩：

- **`create=false` 的 runner 要先 populate**。`50r50u` / `80r20u` / `rdonly` 的配置注释原话是
  "This test assumes that its correlating populate already completed and exists"。
  直接在一个空 home 上跑，得到的是 `find_table_count: open_cursor failed`。
  脚本用 `wt_runner_needs_populate` 判断，先跑 `500m-btree-populate.wtperf`
  把数据灌进**同一个 home**，再跑 workload。
- **home 目录必须先存在**。wtperf 的第一步是 `fopen(<home>/CONFIG.wtperf, "w")`，
  目录不存在时它报的不是 `ENOENT` 而是 `EPERM`（`config_opt_log`），然后
  `__wt_abort` 直接 core dump —— 看起来像"库崩了"。所以 `wt_new_home` 一定带 `mkdir -p`。

| runner | 压的是什么 |
|---|---|
| `populate` | 纯插入：split + reconcile 的稳态 |
| `50r50u` / `80r20u` | 读写混合：update 链长度 + 淘汰（`create=false`） |
| `rdonly` | 只读：cache 命中路径（`create=false`） |
| `evict-btree` | 淘汰压力（16 读线程 + eviction 线程池） |
| `evict-btree-stress` | 淘汰极限：页几乎活不过一次 descend |
| `checkpoint-stress` | checkpoint 与写并发 |
| `insert-rmw` | 读-改-写：先 get 再 update |
| `long-txn-btree` | 长事务挂着 → watermark 停滞 → HS 堆积 |
| `index-pareto-btree` | 倾斜键分布：热点页反复 split |

验收：每个 runner 退出码 0，**且日志里能解析出非零的 ops/sec**，且在 `WT_TIMEOUT` 内结束。
退出码 0 但一个数字都没有（`verbose` 太低、`run_time` 太短）记 FAIL ——
"跑了但什么都没测到"不是通过。

> **ops/sec 不能取最后一行**。wtperf 的结尾是"每种操作一行"：
> ```
> Executed 0 insert operations (0%) 0 ops/sec
> Executed 89810552 read operations (100%) 4490527 ops/sec   ← 真数字在这一行
> Executed 0 scan operations (0%) 0 ops/sec                  ← 最后一行是它
> ```
> 取最后一行会把纯读的 `evict-btree` 读成 `0 ops/sec`。现在是把所有
> `Executed ... N ops/sec` 的速率**求和**（总吞吐）；`populate` 走另一条格式
> `load ops/sec: N`（数字在冒号后面），两条都要认。

> **runner 文件名不统一**（踩过）：10 个里 6 个就叫 `evict-btree.wtperf`，另外 4 个
> （`populate` / `50r50u` / `80r20u` / `rdonly`）带规模前缀 `500m-btree-`。
> 脚本用 `resolve_runner` 依次试 `$name.wtperf` → `500m-btree-$name.wtperf` → `*-$name.wtperf`。
> 直接拼 `$name.wtperf` 的结果是这 4 个静默返回 `-`，看起来像实现错了。

排查单个 runner 时别跑全家桶：`RUNNERS="populate 50r50u" make capstone`。

## C2 —— wt 工具子集

```
wt list / create / drop / dump -x / load
```

> `wt verify` / `wt stat` **不在默认判据里**：前者要 `WT_SESSION::verify`，后者要
> `statistics:` 游标，两者都没有对应的 Task（statistics 属可选池 O6）。想自测用
> `C2_OPTIONAL=1 make capstone` —— 它会跑，但不把缺实现记成 capstone FAIL。

真 `wt` 要 **27 个符号**，拆开看是三块：

| 块 | 个数 | 内容 |
|---|---|---|
| `__wt_*` 内部函数 | 20 | 平台胶水 / 缓冲格式化 / JSON / 内核元数据与日志 |
| `wiredtiger_*` 公共 API | 3 | `wiredtiger_open`、`wiredtiger_strerror`、`wiredtiger_version` |
| 数据对象（`R_X86_64_COPY`） | 4 | `__wt_optind`、`__wt_optarg`、`__wt_optreset`、`__wt_optwt` |

`wtperf` 要 26 个：18 个 `__wt_*` + 4 个公共 API + 4 个数据对象（它有 `__wt_process`
但没有 `__wt_optwt`）。两者 `__wt_*` 并集 **34 函数 + 5 数据对象 = 39**，都在
`src/api/wt_tool_abi.cpp`（T3.6）。签名取自 `gcc -E` 预处理后的真实声明，别凭记忆改。

> **数据对象为什么容易漏**：它们是变量，被链接器用 `R_X86_64_COPY` 在可执行文件的
> `.bss` 里复制一份，所以 `nm -D` 里显示为**已定义**。只看 `nm` 一定漏，直到 ld.so
> 在加载时硬报 `undefined symbol: __wt_optreset`（数据符号不延迟绑定）。
> COPY 是按 **size** 搬的，`__wt_process` 定义小一个字节就会踩坏工具的相邻内存 ——
> 这也是 `wt_tool_abi.cpp` 里那句 `static_assert(sizeof(__wt_process) == 128)` 的由来。

其中 5 个**确实碰存储内核**：`__wt_block_ckpt_decode`（checkpoint 地址 cookie）、
`__wt_metadata_get_ckptlist` / `__wt_metadata_free_ckptlist`（元数据表）、
`__wt_txn_printlog`（WAL 解码）、`__wt_copy_and_sync`。C2 的难点就在这里 ——
它把你 P2 写的 checkpoint 格式拉到真 WT 的字节级标准。

验收：**每条子命令的退出码 + 输出内容都要对**，不是"exit 0 就算过"。
真 `wt` 在参数解析失败时会打印 usage 然后退出，光看退出码会把"什么都没做"记成通过。
而且 `dump -x` 只判"能打印十六进制"，正确性交给 `dump → load → dump` 的逐字节对拍：

| 步骤 | 断言 |
|---|---|
| `create table:t` | 不打印 usage 段 |
| `list` | 输出里**有** `table:t`（写进去了的唯一证据） |
| `drop table:t` | 不打印 usage 段 |
| `list` | 输出里**没有** `table:t`（drop 不是空操作） |
| `create -c key_format=S,value_format=S table:rt` + `write` ×3 | 不打印 usage 段 |
| `dump table:rt` | 头三行合 load 的契约 + 有 `Data` 段 + 含 `k1\00` + 数据段 6 行 |
| `load -f`（导入**全新 home**） | 不打印 usage 段 |
| 再 `dump` 一次 | 与第一份 dump **逐字节相同**（`cmp`） |
| `dump -x table:rt` | 有连写的十六进制行，且其中出现 `6b3100`（`k1\0`） |

> **第一步不是 `list`，是 `create`**（踩过）：看着"空库能 list"像个合理的冒烟步骤，
> 实测真 WT 11.3.0 在空目录上直接 `wt -h <empty> list` 会退出 1 ——
> `__posix_open_file: .../WiredTiger.lock: open: No such file or directory` →
> `WT_TRY_SALVAGE`；而 `create` 在同一个空目录上能正常建库。把它当判据，
> 我们自己的实现永远过不了。

最后那一行 `cmp` 是 C2 里唯一能证明「`dump` 与 `load` 互为逆操作」的检查 ——
退出码 0 只能证明它没报错，证明不了它还原对了。这一环**不依赖 wtperf**：
用 `wt write` 造 3 条数据即可，所以 C2 不再被 C1 拖着走（以前 populate 一失败整段 SKIP）。

### dump 的文本格式是硬契约

`wt dump` 自己不格式化数据 —— 它 `open_cursor(uri, "dump=print")`，格式化发生在**库里**
（真 WT：`src/cursor/cur_dump.c`），转义规则在 `src/support/hex.c:66` 的
`__wt_raw_to_esc_hex`。而 `wt load` 读回来时的校验在 `src/utilities/util_load.c:253-280`。
这两处一头一尾卡死了格式，中间没有自由度：

```
WiredTiger Dump (WiredTiger Version 11.3.0)   ← 第 1 行，必须以 "WiredTiger Dump " 开头
Format=print                                   ← 第 2 行，-x 时是 Format=hex
Header                                         ← 第 3 行，必须恰好是 Header
table:rt                                       ← 之后是 uri / config **成对**
access_pattern_hint=none,key_format=S,...          （奇数条 load 直接判格式错）
Data                                           ← 遇到这行转入数据段
k1\00                                          ← print 模式：一行键
v1\00                                          ← 　　　　　　一行值（不是一行一对）
```

转义只有三条规则，用 `__wt_raw_to_esc_hex` 逐字节走：

| 字节 | 输出 |
|---|---|
| 可打印字符（`__wt_isprint`） | 原样 |
| `\` | `\\`（要成对，否则 load 把后面的字符当转义） |
| 其余 | `\` + 两位**小写**十六进制，如 `\00`、`\ff` |

`-x` 走的是另一条路（`__wt_raw_to_hex`）：每个字节两位小写 hex、**连写不空格**，
一条记录占两行（键一行、值一行）。实测 `k1\0` → `6b3100`。

> **C2 挂在这里，通常不是 P0–P4 的问题**：`open_cursor` 若把 `dump=print` 当未知配置
> 忽略掉，dump 会打出未转义的原始值，前两行断言都过、`load` 却解不回来。
> 这一条写在 `docs/projects/p3_api_txn_wal.md` 的 T3.2 里，不另记分，但 C2 依赖它。

> **判据必须用真 WT 校准过**：先前的 `-x` 判据写的是 `xx xx xx`（空格分隔），
> 拿真 WT 的输出去跑是 **FAIL** —— 一个永远报错的检查比没有检查更浪费。
> 表里每一条断言都用真 WT 11.3.0 的实际输出验过一遍（含 `dump→load→dump` 确实是
> 逐字节相同），所以"通过"和"没通过"都能指向真问题。

这件事做成了一个目标，随时可以复核：

```
make capstone-selftest
```

它把 `SHLIB` 指向真库跑一遍 C1/C2（跳过 C3/C4：两边本来就是同一个库），
全绿说明**判据没写错**，此时还 FAIL 就一定是实现的问题。判据改过之后先跑它。

## C3 —— 对照组

同一个 runner、同一个 `icount`，跑两遍：

```
real-WT  =  $WTPERF -O runner.wtperf -o icount=N        （清空 LD_LIBRARY_PATH，用真库）
tinywt   =  LD_LIBRARY_PATH=build $WTPERF ...            （同 SONAME 替换，真库不进场）
```

注意两边**不是**靠 `LD_PRELOAD` 区分的。工具的可执行文件里写死了
`NEEDED [libwiredtiger.so.11.3.0]`，`LD_PRELOAD` 只会让真库和我们的库**同时**加载，
没导出的符号被真库静默兜底 —— 那样跑出来的 ratio 既不是真 WT 也不是 tinywt。
对照组要的是"只换库"，所以走 `LD_LIBRARY_PATH` + 同 SONAME（`wt_setup_dropin` 判模式）。

输出对比表（`ratio = tinywt / real-WT`）。这里**不设及格线**：
学习实现慢 1–2 个数量级是正常的，有意义的是「ratio 随 P3/P4 的优化怎么变」。
真正该看的是**功能等价性**：同样的 runner、同样的 icount，数据必须一样。

> **怎么知道 C3 本身没坏**：把 `SHLIB` 临时指向真库跑一遍（两边同一个库），
> ratio 应该落在 1 附近。实测这台机器上 `50r50u` 0.88、`evict-btree` 1.21 ——
> 噪声来自 20 秒的短跑。如果同一份库跑出 0.1 或 10，那是 harness 的问题
> （多半是一边没换上库，或者一边多跑了 populate），别拿去解释成"我的实现快/慢"。

## C4 —— 汇总

把 C1 的数字追加进 `docs/LEADERBOARD.md`：

- P3 榜：populate / 读写混合的 ops/s，按 `cache_size × 线程数` 出曲线
- P4 榜：`evict-btree-stress` 的 ops/s，以及「旧快照长读」场景下 p99 不劣化的断言

只统计 `*.tinywt.log`（真 WT 那一侧的数字不进榜）；一个非零数字都没有就不写表 ——
空表看起来像"跑过了，只是都是 0"。`SHLIB` 不是 `build/libwiredtiger.so` 时也不写：
校准跑（指着真库）出来的数也是真 WT 的，进了榜就是假数据。

表格是给人看的，不进 `make grade`。

---

## 三种结局，两个退出码

```
exit 0  全部通过（fail=0 且 skip=0）
exit 1  有 FAIL
exit 2  没跑完：没有任何 FAIL，但有 SKIP（缺 wt-tools / 缺 .so）
```

`SKIP` **不算通过**。以前这里写的是 `PASSED（N 项 SKIP）` —— 一次"一个 runner 都没跑"
的运行也会显示 PASSED，那是最容易骗过自己的绿：它只证明"没跑"，不证明"跑对了"。
现在 SKIP 走 exit 2 并打印"缺依赖就补：`make wt-tools`"。

## 验收清单

- [ ] `make grade` 全绿（P0–P4 每个测试项 PASS，无 NOTIMPL）
- [ ] `make cp1` / `make cp2` PASSED
- [ ] `make probe-link` 零 undefined reference
- [ ] `make shared` 产出 `build/libwiredtiger.so`
- [ ] `make wt-abi` 缺口 0 **且** 0 个空桩（两个条件都要）
- [ ] C1 十个 runner 全部退出码 0 且各自有非零 ops/sec
- [ ] C2 `wt` 全部子命令通过，且 `dump → load → dump` 逐字节相同
- [ ] C3 对比表已生成（ratio 有数即可，不设阈值）
- [ ] C4 `docs/LEADERBOARD.md` 已更新

## 常见失败

| 现象 | 原因 |
|---|---|
| 换库后立刻 segfault | `wiredtiger.h` 的结构体布局对不上（`iface` 必须是首成员） |
| `undefined symbol: __wt_optreset` | 数据对象漏了：`nm -D` 看不见，得看 `readelf -r` 的 COPY 重定位 |
| `undefined symbol: __wt_config_merge` | 函数没编进去：确认 `src/api/wt_tool_abi.cpp` 在源列表里 |
| C1/C2 开头打印的是 `preload` 而不是 `replace` | SONAME 没对齐 → 真库参与解析，那次"通过"不可信；`make shared` 后重看 |
| 打印了 `replace`，但结果是真 WT 的 | `wt_setup_dropin` 被写进 `$(...)` 里丢了 `WT_ENV`。`wt_check_dropin` 用 `ldd` 会抓出来 |
| `make wt-abi` 报缺口 0，但 `wt list` 只打印 usage | 符号齐了但 `__wt_getopt` 还是空桩 —— **符号齐 ≠ 功能对** |
| 某个 runner 跑满 `WT_TIMEOUT` 没动静 | 多半是 `__wt_thread_create` 空桩：没建线程，主线程死等 worker |
| `interval value longer than the run-time` | `run_time` 没大于 `report_interval`(10)，wtperf 一条都不跑 |
| runner 退出码 0 但一个 ops/sec 都没有 | `verbose` 太低（这个构建要 ≥2），或 `run_time` 短到没出统计 |
| `find_table_count: open_cursor failed` | 这个 runner 是 `create=false`，得先在**同一个 home** 里 populate |
| `config_opt_log: ... CONFIG.wtperf: Operation not permitted` | home 目录不存在就开跑，wtperf 报的是 EPERM 然后 abort；`wt_new_home` 里有 `mkdir -p` |
| `__wt_dlopen ... libwiredtiger_snappy.so` | 没覆盖 `compression=none`：本机没编 snappy 扩展，且本项目只支持 NONE/RLE |
| `wt` 报 `WT_TRY_SALVAGE` / `database corruption` | 超级块读失败：`SBF_*` 标志位或 `alloc_size` 与块管理器不一致 |
| runner 全 FAIL 但单测全绿 | 单测走的是自己构造的 Config，runner 走 `.wtperf` 里的 `conn_config`，config 解析没接上 |
| `dump -x` 输出乱码 | cell 编码与真 WT 不一致 —— 这是唯一必须**字节级**对齐的地方 |
| `load` 只回一句 `invalid format` | 它不说是哪一行。按 `util_load.c:253-280` 查头三行：`WiredTiger Dump ...` / `Format=print` / `Header`，再查 uri-config 是否成对 |
| `dump` 能打、`load` 解不回，`cmp` 有 diff | `open_cursor` 没认 `dump=print`，打出的是未转义原文；转义规则见 `__wt_raw_to_esc_hex`（可打印原样、`\`→`\\`、其余 `\xx` 小写） |
| `wt` 打印 usage 且 exit 0 | 参数没被识别（`__wt_getopt` 空桩），C2 靠"输出内容断言"抓这种假通过 |
| `cmake 配置失败：Could NOT find Python3` | 未打 `wt_tools.sh` 那个补丁；直接 `WT_CLEAN=1 make wt-tools` 重来 |
