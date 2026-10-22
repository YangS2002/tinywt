# P3 任务书 —— 对外 ABI：cursor / 事务 / 日志

> WT 的对外形态：C 方法表（`src/conn/`、`src/session/`、`src/cursor/`）、
> 配置解析（`src/config/`）、catalog（`src/meta/`）、事务（`src/txn/`）、日志（`src/log/`）。
> 这一层的验收标准不是单测全绿，而是「真 `wtperf` 能不能链上并跑起来」。
> 总纲与评分见 [PROJECTS.md](../PROJECTS.md)。
> 验收：23 个测试项（其中 2 项是契约自检，不需要写代码）。
>
> | 子项目 | 内容 | 函数 | 评分项 |
> |---|---|---|---|
> | T3.0 | 错误码契约 + strerror + version（**已给**） | 0 | 2 |
> | T3.1 | config parser / conn / 元数据 | 5 | 2 |
> | T3.2 | cursor 写路径 | 21 | 1（与 T3.1 串成一条链测） |
> | T3.3 | cursor 读路径与遍历 | 16 | 7 |
> | T3.4 | 事务 API + modify/truncate | 21 | 5 |
> | T3.5 | WAL + 崩溃恢复 | 9 | 6 |
> | T3.6 | 工具面内部 ABI（34 个 `__wt_*`） | 34 | 0（由 Capstone C1/C2 验收） |
>
> **与原顺序的差异**：WT 里 log 与 checkpoint 同属持久化（LSN 互相对齐）；这里把
> T3.5 放在 P3 末尾，是因为要先有完整的 C ABI 链路，`wtperf` 才跑得起来。

## 前置

P2 全绿才能开始：P3 的每一次 `create` 都会落到 `Btree::open` + `checkpoint`。

```bash
make p2-test
make probe        # 契约头能独立编译 + 错误码数值对齐
make probe-link   # 链接期：缺哪个接口就缺哪个符号
```

---

## 为什么这一层值一个 Project

因为**真工具只认 C 结构体**。`third_party/wiredtiger/include/wiredtiger.h` 是从 WT 11.3.x 的
`wiredtiger.in` 生成的**真头文件**，里面是：

```c
struct __wt_connection {
    int (*close)(WT_CONNECTION *);
    int (*open_session)(WT_CONNECTION *, WT_EVENT_HANDLER *, const char *, WT_SESSION **);
    ...
};
```

`wtperf` 拿到 `WT_CONNECTION*` 之后，第一件事就是 `conn->open_session(...)`。
所以「函数指针有没有填对」= 「能不能被真工具用」，编译器不会替你检查。

三个必须记住的 ABI 规则：

1. **`iface` 是首成员**：`twt_conn` 的定义里 `WT_CONNECTION iface;` 必须在最前面。
   这样 `WT_CONNECTION*` 和 `twt_conn*` 是同一个地址，`reinterpret_cast` 安全。
   真 WT 也是这个套路（`conn->iface`）。
2. **变参签名**：`set_key` / `set_value` 是 `void(WT_CURSOR *, ...)`；
   `close` 是 `int(WT_SESSION *, const char *)`。签名错一个参数，
   调用方（C 代码）不会报错，只会栈错位。
3. **错误码**：`WT_NOTFOUND != WT_ERROR`。键不存在必须是 `WT_NOTFOUND(-31803)`。

---

## T3.0 错误码契约（已给，2 项）

`src/api/connection.cpp` 里的 `twt_wt_rc()` 和 `wiredtiger_strerror()` 已经写好。
它们的作用是把内部 `Status` 收敛到一个唯一边界：

```
internal Status  ──twt_wt_rc()──▶  WT_* 错误码  ──▶  wtperf / wt 的判断
```

历史坑（这个项目真的踩过）：早期 `Status` 的值和真 WT 差了 3，
`S_NOTFOUND = -31800` 正好等于 `WT_ROLLBACK`，于是 wtperf 把
「键不存在」当成「事务冲突」无限重试。现在 `include/tinywt/common/defs.h` 直接
按 `dist/api_err.py` 的数值定义，`test/contract/abi_probe.c` 里还有 `_Static_assert` 钉着。

> 这两项不需要你写代码，但如果为了让别的测试变绿去改 `Status` 的数值，
> 它们会立刻变红。

---

## T3.1 配置解析 + 连接（2 个评分项）

### T3.1.1 `wiredtiger_config_parser_open`

真 wtperf 自己解析 `.wtperf` 文件，但它调的这个函数必须由引擎提供 ——
这是「能被真工具链接」的第一个硬依赖。

语法：

```
config := item { ',' item }
item   := key [ '=' value ]
value  := bare | '(' config ')' | '"' ... '"'
```

必须处理的边界：

| 输入 | 期望 |
|---|---|
| `cache_size=50M` | `key="cache_size"`, `value="50M"`, type = ID 或 NUM |
| `eviction=(threads_max=8)` | 一个 item，value 原文含括号，type = STRUCT |
| `overwrite=true` | type = BOOL，`val = 1` |
| `verbose` | 只有 key，没有 value（`len == 0`） |
| `a="x,y"` | 引号里的逗号不是分隔符 |

坑：`WT_CONFIG_ITEM::str` 是 `const char*`，**不能指向调用方传进来的缓冲区**
（那个缓冲区可能是临时量）。解析结果要自己持有 `std::string`，
`ParserImpl::keys/vals` 就是干这个的。

另外两个辅助函数（`cfg_find` / `cfg_bytes`）是给 `wiredtiger_open` 用的：
`cfg_bytes("50M") == 50 * 1024 * 1024`。

### T3.1.2 `wiredtiger_open`

```
home 为空 → "."
配置里的 cache_size → cfg.max_cached_pages（本版按 32KB/页估算，下限 8 页）
打开 <home>/WiredTiger.wt  ← 它本身就是一张普通的 tinywt 表
scan_all 扫元数据 → 对每个 uri 建 twt_table 放进 conn->tables
绑定 iface.close / iface.open_session
```

关键设计：**元数据不是文件头，而是一张表**。`WiredTiger.wt` 的
key = `"table:foo"`，value = 建表时的配置串。这让「恢复所有表」变成
「扫一遍表」，也让元数据自动获得 checkpoint / WAL / 事务能力。
（真 WT 就是这么做的，这也是为什么你能用 `wt list` 看元数据。）

### T3.1.3 URI → 文件

`table:foo` → `<home>/foo.wt`。注意 `file:` 和 `table:` 前缀要去掉，
`table:` 与 `file:` 在本版等价（真 WT 里 `table:` 是上层，`file:` 是下层）。

### T3.1.4 建表配置 → `Config`

`session->create` 要把下面这几项接进 `twt::Config`（字段已给出，别自己加）：

| 配置串里的键 | Config 字段 | 换算 |
|---|---|---|
| `leaf_page_max=32KB` | `leaf_page_max` | 直接存字节数 |
| `internal_page_max=4KB` | `internal_page_max` | 直接存字节数 |
| `split_pct=90` | `split_pct` | 0–100，分裂时左页占比 |
| `type=file` | — | 本版只有 file，忽略 |

**坑：`leaf_page_max` 是字节，`max_leaf_entries` 是条目数，差一个数量级。**
两者不能互相赋值。两条路选一条并写进注释：

1. 建表时按平均 cell 大小换算成条目数（`entries ≈ leaf_page_max / avg_cell`）；
2. reconcile 时按累计字节数判满，`max_leaf_entries` 只做兜底上限。

P2 的测试用 `max_leaf_entries` 直接控制页大小（好对拍），
所以第 1 条更省事；第 2 条更接近真 WT（真 WT 就是按字节判的）。

---

## T3.2 session + cursor 写路径（1 个评分项，与 T3.1 串成一条链测）

> **真 WT**：`src/cursor/cur_file.c`（`__wt_curfile_insert` / `_update` / `_remove`）、方法表在 `src/include/cursor.h`。

```
s->create(uri, config)        已存在 → WT_ERROR
s->drop(uri)                  不存在 → WT_NOTFOUND，并删文件 + 摘元数据
s->open_cursor(uri, ...)      表不存在 → WT_NOTFOUND
conn->close()                 所有表 checkpoint + close
c->insert()                   重复键 → WT_DUPLICATE_KEY（除非 overwrite）
c->update()                   键不存在 → WT_NOTFOUND
c->remove()                   键不存在 → WT_NOTFOUND
c->search()                   键不存在 → WT_NOTFOUND
c->modify()                   读 → 打补丁 → 写
```

变参收发（`read_field` / `write_field`）支持的格式码：
`S`（字符串）、`s`（字符串）、`i`（int）、`u`（unsigned）、`l`/`q`（long）。

`wiredtiger_calc_modify` 是本 Project 唯一一个「算法题」：给定 old/new，
算出一个 `WT_MODIFY` 补丁。要求是公共前缀 + 公共后缀，中间那段算替换：

```
old: AAAA[BBBB]CCCC
new: AAAA[DD]CCCC
     p=4  s=4  rm=4  add=2
```

`add > maxdiff` 时返回 `WT_NOTFOUND`（调用方会退化成整值 update）。

---

## T3.3 游标遍历（7 项）

这一项测的是顺序访问，`wtperf` 的 read 模式 100% 走这条路径。

| 测试 | 测什么 |
|---|---|
| 全序正向 | `next` 从第一条走到最后，顺序和值都对 |
| 全序反向 | `prev` 从最后一条走到第一条 |
| search 后 next | 从命中位置继续，不是回到开头 |
| search_near 三边界 | 小于最小 / 精确 / 大于最大 |
| 删除后扫描 | tombstone 不出现 |
| 跨页 + cache 压力 | 遍历过程中页被淘汰，仍然正确 |
| **遍历中 split** | 一边扫一边插（400 键扫到第 50 个时插 200 个），游标位置不丢、不重、不乱序 |

**遍历中 split 是这一项最容易挂的用例。** 游标位置不是一个「下标」，而是
「页 + 页内偏移」；一旦这页裂成两页，原来的偏移在新页里指向另一个键。
真 WT 的做法是让页分裂时**重新定位**所有指向该页的游标，
或者让 `next` 发现「当前键 < 上一个返回的键」时重新 descend。本版允许你用任一种策略，
但测试会逐条检查**单调递增 + 无重复 + 无遗漏**，蒙不过去。

两个语义细节（真 WT 就是这么规定的）：

1. **没定位过的游标 `next()` 返回 `WT_NOTFOUND`**。真 WT 不会替你从头扫，
   全表扫描的正确姿势是 `search(最小键)` 再 `next`。
2. **走到头之后游标复位**：`next` 越过最后一行后，再 `prev` 从末尾开始。

实现上，游标引擎本体（`Btree::cursor_first/last/next/prev/seek/current/release`
以及 `leaf_load` / `descend_left/right/seek`）**属于本 Task 的实现范围** ——
它们不在 P2 里，P2 只到 `scan_all`。`src/btree/btree.cpp` 里这些函数的
`UNIMPLEMENTED` 标的就是 `P3/T3.3`。
游标销毁前必须 `cursor_release`，否则 pin 泄漏 → eviction 永远失败。

---

## T3.4 事务 API + 增量操作（5 项）

`begin_transaction` / `commit_transaction` / `rollback_transaction` 三个入口，
加上两个「不是整值覆盖」的写操作：`cursor->modify`（补丁写）和
`session->truncate`（范围删）。

内核侧实现在 `include/tinywt/txn/txn.h` + `src/txn/txn.cpp`；C ABI 侧只是把
`WT_SESSION` 上的调用转成 `TxnSession` 的调用。

### 为什么「比大小」不够

```
时刻   T1                T2                T3
      A.begin()  ─────────────────────────────▶
                        B.begin()
                                          B.commit()
      A.get(k)  ←──── 这里必须看不到 B 的写
```

如果只判 `u->txnid <= A.id`，而 B 的 id 比 A 小（B 开得早但提交晚），
A 就会读到 B 未提交的数据 —— 脏读。所以必须记「A 开始时谁还活着」：

```
snap_min = 开始时最小的活跃 txn id
snap_max = 开始时最大的已分配 txn id + 1
snapshot = 开始时活跃的 txn id 列表
```

### visible() 的判定顺序

```
1. u->txnid == reader.id              → true     （读自己的写）
2. u->txnid 仍在活跃集合               → false    （别人没提交）
3. u->txnid >= reader.snap_max        → false    （begin 之后才开的）
4. u->txnid ∈ reader.snapshot         → false    （begin 时还活着）
5. u->start_ts > reader.read_ts       → false    （时间戳太新）
6. 否则                                → true
```

**顺序不能换**。第 1 条必须在第 2 条前面：自己的 txnid 也在活跃集合里，
不特判就永远读不到自己刚写的值。这是这一项最常见的 bug。

### rollback：精确摘链

回滚不是「把链头置 nullptr」。链上可能有别的事务的版本：

```
回滚前： [7/txnA] → [5/txnA] → [3/txnB] → [1/base]
回滚 A： 摘掉 7 和 5，保留 3 和 1
```

所以 `Txn.writes` 要记 `(key, Update*)`，回滚时：

1. descend 到 key 所在叶子；
2. 在 `Entry::upd` 链（或 insert 节点的链）上找这个 `Update*` 的**前驱**；
3. 前驱跳过它（`prev->next = cur->next`），释放 cur；
4. 如果这个键本来就不在磁盘镜像里（是 insert 节点），且链被摘空，
   节点也要删掉、`ninserts--`。否则回滚后还能扫到一个空壳键。

### 写写冲突

规则（本版，先写者赢）：

```
put(k) 时看链头：
  链头存在 && 链头的 txnid 属于「另一个未提交事务」 → S_ROLLBACK
```

`TxnSession::put` 的顺序很重要：**先检查冲突，再挂链**。反过来会让
失败的事务污染链。真 WT 更客气：把后来者挂到冲突队列上等对方结束
（`WT_TXN_OP` + `txn->mod`），本版让后来者直接重试。

### modify：补丁写

`WT_MODIFY` 是 `(offset, size, data)` 的数组，语义是「把 oldv 的
`[offset, offset+size)` 换成 `data`」：

```
old: AAAA[DD]CCCC
new: AAAABBCCCC        ← 一个 modify entry: {offset=4, size=2, data="BB"}
```

两条必须记住的规则：

1. **entries 按 offset 降序应用**（真 WT 要求调用方这么给）。因为前一个补丁
   会改变后面字符串的长度，从后往前改则前面的 offset 不受影响。
2. `wiredtiger_calc_modify` 用「最长公共前缀 + 最长公共后缀」切一刀，
   中间那段就是补丁。`add > maxdiff` 时返回 `WT_NOTFOUND`，调用方退化成整值 update。

### truncate：范围删

`session->truncate(uri, start, stop, cfg)`：`start`/`stop` 为 `NULL` 表示整表。
实现就是「scan + remove」，真 WT 明确说明不是 O(1) 的元数据操作 ——
因为 range truncate 必须让并发读者看到一致的结果。

---

## T3.5 WAL 与崩溃恢复（6 项）

内核侧实现在 `include/tinywt/wal/log.h` + `src/wal/log.cpp`。

| 测试 | 测什么 |
|---|---|
| WAL 重放 | 干净关闭后从空数据文件重开，已提交事务全部还原 |
| 未提交丢弃 | 没有 COMMIT 记录的事务不进数据文件 |
| 尾部截断 | 半条记录 = 正常崩溃现场，`replay()` 返回 `S_OK` |
| 中间损坏 | 坏记录后面还有完整记录 → `S_CORRUPT`，不能当截断放过 |
| ckpt 之后的日志 | 只重放 `>= ckpt_lsn` 的记录，之前的可以裁 |
| **随机 crash ×4 种子** | 随机截断点 + 混入未提交事务与「幽灵键」，恢复后状态必须等于**某个已提交前缀** |

随机 crash 用例的判据不是「恢复到最后状态」，而是「恢复到一个合法状态」：

```
日志： B1 R(a) C1  B2 R(b) R(c) C2  B3 R(d) C3
                      ↑ 从这里截断
合法解：{a, b}      （C2 的上半截必须丢，C1 必须全在）
非法解：{a, b, c}   （c 属于未提交的 T2）
非法解：{a}         （C1 已提交却被吞掉 = 丢数据）
```

判据实现上就是枚举所有 `txn_end` 边界，恢复结果必须**字面等于**其中某一个；
同时断言所有「幽灵键」（只在未提交事务里出现过的键）一个都不能残留。
4 个种子各跑一遍，专门抓「按固定偏移截断」测不出来的错位 bug。

### 规则只有一句

```
写路径：  日志 append + fsync  →  改内存（update 链）  →  （checkpoint 时）写数据页
```

顺序不能反：先改内存再写日志，崩溃后就丢了 —— 内存里的修改没有任何副本。
反过来说，只要日志落了盘，内存丢失可以用日志重放补回来。

### 记录格式（本版）

```
[u32 len][u32 crc32(payload)][u8 flags][payload]
  payload = [u8 type][varint txnid][varint klen][key][varint vlen][val]
  type: LOG_TXN_BEGIN=1 / LOG_ROW_PUT=2 / LOG_ROW_REMOVE=3 / LOG_TXN_COMMIT=4
```

真 WT 的记录头（`src/include/log.h:311`）多两样东西：`LOG_ALIGN=128` 对齐、
`LSN = (file << 32) | offset`，首块是 `WT_LOG_DESC{magic, version, allocsize}`。
这些放 Capstone（C1）再补，本版只保留「先日志后数据」这个核心不变量。

### 三条恢复语义（缺一条都会「越崩溃越起不来」）

| # | 规则 | 不这么做会怎样 |
|---|---|---|
| 1 | 尾部半截记录 / checksum 不符 → **停下，不报错** | 崩溃现场必然有半条记录，报错就永远起不来 |
| 2 | 只重放**有 COMMIT 记录**的事务 | 未提交的写进了数据文件 = 脏读 |
| 3 | 重放完立刻做一次 checkpoint | 下次恢复又要从头扫一遍，恢复时间随日志线性增长 |

「尾部半截」和「中间损坏」必须分开判：两者的现场都是 checksum 对不上，
区别只在**后面还有没有完整记录**。一律报错 → 起不来；一律放过 → 真损坏被
当成正常截断，静默丢数据。所以 `replay()` 对前者返回 `S_OK`、对后者返回 `S_CORRUPT`。

### 与 checkpoint 的交互

checkpoint 之后，`[0, ckpt_lsn)` 的记录已经落进数据页，可以裁掉：

```
ckpt_lsn 写进超级块 → 日志只保留 >= ckpt_lsn → truncate_upto(ckpt_lsn)
```

不裁也行，正确性不受影响，但恢复时间会无限增长。

### 测试怎么模拟崩溃

```cpp
LogManager log;  log.open(p);  ...写事务...  log.close();
// 断电：把日志文件砍掉最后 3 个字节
fs::resize_file(p, size - 3);
// 重开：数据文件是空的，只能靠日志活过来
Btree bt; bt.open(db);
LogManager log2; log2.open(p);
log2.replay(bt);   // 必须 S_OK，且只还原有 COMMIT 的事务
```

---

## T3.6 工具面内部 ABI（0 个评分项，由 Capstone C1/C2 验收）

T3.0–T3.5 只覆盖 `wiredtiger.h` 里的**公开**接口。真工具不止用公开接口。

```
$ nm -D --undefined-only $WT_SRC/build/wt | grep __wt | wc -l
20
$ nm -D --undefined-only $WT_SRC/build/bench/wtperf/wtperf | grep __wt | wc -l
18
# 并集扣掉 4 个重叠 = 34
```

这 34 个不是"顺手加的兼容层"，而是**动态链接器点名要的**：`LD_PRELOAD` 我们的 `.so`
时少一个就 `undefined symbol` 当场起不来。它们集中在 `src/api/wt_tool_abi.cpp`，
按性质分四组：

| 组 | 个数 | 代表 | 实现难点 |
|---|---|---|---|
| 平台胶水 | 14 | `__wt_thread_create` `__wt_sleep` `__wt_random` | 线程 + 计时；`sleep` 单位是(秒,微秒) |
| 缓冲 / 格式化 / 配置 | 10 | `__wt_buf_fmt` `__wt_config_merge` `__wt_getopt` | 要 `WT_ITEM` 语义，不是裸 `char*` |
| JSON | 5 | `__wt_json_token` `__wt_json_strncpy` | 手写流式 reader，`dump -x` / `load -j` 用 |
| 内核元数据与日志 | 5 | `__wt_block_ckpt_decode` `__wt_metadata_get_ckptlist` | **要 P2 的 checkpoint 格式字节级对齐** |

### 为什么没有单测

这 34 个函数的调用方是**外部进程**，不是我们的测试二进制。写单测只能测自己，
测不出 ABI 对不对。所以验收方式是：

```bash
make wt-abi     # 符号层：缺口几个 + 还剩几个空桩
make capstone   # 功能层：C1 十个 runner / C2 wt 七条子命令
```

**符号齐 ≠ 功能对**：空桩也是导出符号，`nm` 看着是齐的，但 `wt list` 只会打印 usage。
`make wt-abi` 因此把两层分开报 —— 看到"缺口 0，还有 34 个空桩"才是真实状态。

### 实现顺序（按依赖，不按字母）

1. **`__wt_getopt` 先做**。它是第一堵墙：没它 `wt` 连 `-h` 和子命令都解析不了，
   所有 C2 步骤都退化成打印 usage。
2. 平台胶水（`thread_*` / `sleep` / `errno` / `random` / `filename`）。
   这组一做，wtperf 至少不会死等 worker。
3. `buf_*` / `scratch` / `strtouq` / `vsnprintf_len_incr`。
4. JSON。
5. `block_ckpt_decode` + `metadata_*ckptlist` + `txn_printlog` —— 最难，也最值钱。

---

## 评分测试

| 测试 | 覆盖 |
|---|---|
| `test/p3/api_test.cpp` | 错误码契约、config parser、open/create/insert/search、重开恢复 |
| `test/p3/cursor_test.cpp` | 正/反向遍历、seek 边界、删除后扫描、跨页 + 淘汰 |
| `test/p3/txn_test.cpp` | 读自己的写、未提交不可见、回滚精确摘链、快照隔离、写冲突 |
| `test/p3/wal_test.cpp` | 重放、未提交丢弃、截断、尾部截断、中间损坏 |

## Written Questions

1. WT 为什么把 config 解析做成公开 API（`wiredtiger_config_parser_open`）？
2. 元数据用 B-Tree 存而不是文件头，对崩溃恢复意味着什么？
3. `WT_INSERT` 跳表为什么没有 prev 指针？反向遍历的代价？
4. `WT_CURSOR::set_key` 为什么是 `void` 而不是 `int`？失败了怎么办？
5. 为什么 `WT_NOTFOUND` 不能复用 `WT_ERROR`？（提示：wtperf 的重试逻辑）
6. `WT_MODIFY` 为什么要求 entries 按 offset **降序**？升序会错在哪里？
7. 日志重放完后为什么立刻 checkpoint，而不是直接开服？（两个角度：恢复时间、RTS 边界）
