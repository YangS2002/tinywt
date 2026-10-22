# tinywt 作业总纲

用「重写一遍 WiredTiger 的骨架」来学存储引擎。仓库里现在**只有框架**：
头文件、数据结构、测试、任务书齐全，每个需要你写的函数体都是一行

```cpp
UNIMPLEMENTED("P2/T2.2 descend");
```

你的任务就是把这些行替换成真正的实现。

---

## 一、怎么跑

```bash
make grade      # 逐项打分（未实现的记 TODO，不中断，最后给每项分数）
make test       # 跑全部测试，任何一项不过就非零退出
make p2-test    # 只跑 P2
make probe      # 校验 compat/wiredtiger.h 契约头能独立编译
make clean
```

`make grade` 的输出长这样（骨架初始状态）：

```
proj status    pass  fail  test
P0    PASS      8     0   P0/T0.1 错误码契约（Status vs WT_*）
P0    TODO      0     0   P0/T0.2 varint (LEB128)
...
project      score   title
P0         1/3      33.3%   基础件（varint / crc32 / 错误码契约）
P1         0/2       0.0%   块管理器 + cell 编解码
...
TOTAL      3/31      9.7%
```

四种状态的含义：

| 状态 | 含义 | 你该做什么 |
|---|---|---|
| `PASS` | 该 Task 完成 | 下一项 |
| `FAIL` | 写了，但逻辑不对 | 看 FAIL 行里的文件:行号 |
| `TODO` | 命中了 `UNIMPLEMENTED` | 还没写 |
| `ERR` | 抛了别的异常 / 零断言 | 多半是空指针或越界 |

---

## 二、目录

```
include/      数据结构与接口（★ 不要改签名）
src/          C++17 内核实现（★★ 你的主战场）
src_c/        C ABI 层：extern "C" + 真 wiredtiger.h 的结构体
compat/       真 wiredtiger.h（生成物，别动）+ probe.c（契约探针）
test/         harness.hpp + pN_*_test.cpp（★ 不要改测试）
scripts/      grade.py（打分）、gen_wiredtiger_h.py（生成 compat 头）
docs/         本文件 + P3.md / P4.md 任务书 + WT-NOTES.md（背景阅读）
```

## 三、规则

1. **只改标了 TODO 的函数体。** 签名、结构体布局、头文件里的注释都别动。
2. **不要改测试。** 测试是验收标准。觉得测试本身有错可以提，但不要自己改绿。
3. **不要留着 `UNIMPLEMENTED` 在被调用的路径上**：它只会让该项继续显示 TODO。
4. 允许加**私有**辅助函数（`.cpp` 里的 `static` / 匿名 namespace），别加到头文件。
5. 编译警告不扣分，但别引入 UB（越界、悬空指针、未初始化读）。
6. 调试可以用 `printf`，交之前删掉。

---

## 四、任务表

一共 **31 个测试项**。每个测试项对应下面表里的一行；`验收` 列就是 `make grade`
里显示的名字。

### P0 基础件（3 项）

> 目的：把「工具链 + 错误码契约 + 两个最基础的原语」跑通。
> 阅读：[WT-NOTES.md](WT-NOTES.md) 第五节。

| ID | 要写什么 | 文件 | 对标 WT | 验收 |
|---|---|---|---|---|
| T0.1 | 什么都不用写，但要读懂 | `include/twt_defs.h` | `dist/api_err.py`、`src/include/error.h` | `P0/T0.1 错误码契约` |
| T0.2 | LEB128 编解码 4 个函数 | `src/defs.cpp` | `__wt_vpack_uint` / `__wt_vunpack_uint` | `P0/T0.2 varint (LEB128)` |
| T0.3 | CRC32（查表法） | `src/defs.cpp` | `__wt_checksum_sw` / `__wt_crc32c` | `P0/T0.3 crc32` |

要点：
- `pack_uint(unpack_uint(x))` 必须严格互逆；`uint_size(0) == 1`。
- `unpack_uint` 遇到不完整 / 超过 64 位的编码返回 0，别读越界。
- crc32 必须满足 `crc32("123456789") == 0xCBF43926`。

### P1 块管理器 + cell（2 项）

> 目的：先把「磁盘长什么样」定下来。这一层是后面所有 IO 的地基。
> 阅读：`include/twt_block.h`、`include/twt_cell.h` 顶部注释。

| ID | 要写什么 | 文件 | 对标 WT | 验收 |
|---|---|---|---|---|
| T1.1 | 打开/关闭、pread/pwrite、first-fit 分配 | `src/block.cpp` | `block_open.c`、`block_write.c` | `P1/T1.1+T1.2 block manager` |
| T1.2 | 块读写 + CRC 校验、free→discard、extent 合并 | `src/block.cpp` | `block_read.c`、`block_ext.c` | 同上 |
| T1.3 | key/value/addr 三种 cell 的打包与解包、前缀压缩 | `src/cell.cpp`、`src/page.cpp::common_prefix` | `cell_pack.c`、`cell_unpack.c` | `P1/T1.3 cell 编解码` |
| T1.4 | 超级块序列化（varint 顺序固定） | `src/block.cpp` | WT 的 metadata 表 | 同上（在 T1.1+T1.2 里测） |

要点（容易写错的）：
- `off == 0` 是非法地址（首块留给描述块）。`alloc_extent` 绝不能返回 0。
- `size` 是镜像精确长度，`alloc` 是对齐后的块大小，**两个都要存**。
- 读的时候三重校验：`header.disk_size == addr.alloc`、`header.checksum == addr.cksum`、
  `crc32(image) == addr.cksum`。
- cell 的 short 变体里，描述符高 6 位存的是**整键长度**，不是后缀长度。
- `cell_unpack` 是不可信输入解析：任何越界都返回 `S_CORRUPT`，不能读越界。

### P2 Buffer Pool + B-Tree（6 项）

> 目的：项目的主体。写完之后 `make test` 里 19000+ 条断言全绿。
> 阅读：[WT-NOTES.md](WT-NOTES.md) 第一、四节。

| ID | 要写什么 | 文件 | 对标 WT | 验收 |
|---|---|---|---|---|
| T2.1 | 内存页三件套：insert 跳表、`entry_read`、`materialize_leaf`、`page_in`/`instantiate` | `src/page.cpp`<br>`src/btree.cpp` | `row_srch.c`、`rec_row.c`、`bt_read.c` | `P2/T2.1-2.3 B-Tree 基本 put/get/remove` |
| T2.2 | `descend`：内部页二分下降（取最后一个 `key <= target`） | `src/btree.cpp` | `bt_search.c` | 同上 |
| T2.3 | `put`/`get`/`remove` + 叶子/内部页分裂（含 root 长高） | `src/btree.cpp` | `bt_split.c`、`row_modify.c` | `P2/T2.3 分裂 + 5000 键全量回读` |
| T2.4 | `reconcile_page`/`reconcile_tree` + LRU eviction | `src/reconcile.cpp`<br>`src/btree.cpp` | `rec_write.c`、`src/cache/` | `P2/T2.4 eviction` |
| T2.5 | `checkpoint`、`verify`、`scan_all`、游标遍历 | `src/btree.cpp` | `bt_sync.c`、`bt_cursor.c` | `P2/T2.5 持久化`、`P2/T2.5 读可见性`、`P2 对拍 std::map #*` |

要点：
- **pinned 不是装饰**：descend 路径上的页被 pin 住，`evict_one` 才不会把
  你手里的 `Page*` 析构掉。descend 失败时要自己 unpin。
- **页搬家 → 父页必须标脏**：子页 reconcile 后 addr 变了，父页镜像里还是旧地址。
- **split 时左半页的分隔符只能降不能升**（见 WT-NOTES 第四节坑 2）。
- **eviction 只淘汰叶子页**，且淘汰后要把 `Ref::page` 置 nullptr。
- `cursor_next/prev` 走出叶子时要沿路径找兄弟叶；走到头要 release pin，
  否则 cache 被 pin 满之后 eviction 永远失败（表现为「越跑越慢然后卡住」）。

### P3 C ABI（11 项）

> 目的：让真 WiredTiger 的头文件（`compat/wiredtiger.h`）能调到你。
> 阅读：`src_c/twt_capi.h` 的分层说明、[P3.md](P3.md)。

| ID | 要写什么 | 文件 | 对标 WT | 验收 |
|---|---|---|---|---|
| T3.0 | 错误码契约 + `strerror`（**已给**，改坏就红） | `src_c/conn.cpp` | `src/include/error.h` | `P3/T3.0 错误码契约`、`P3/T3.0 strerror` |
| T3.1 | 配置串解析 + `WT_CONFIG_PARSER` 句柄 | `src_c/config.cpp` | `src/config/config*.c` | `P3/T3.1 config parser` |
| T3.1 | `wiredtiger_open`：分配句柄、元数据表、恢复已存在的表 | `src_c/conn.cpp` | `conn_api.c` | `P3/T3.1+T3.2 open/create/insert/search` |
| T3.1 | 重开连接能从 `WiredTiger.wt` 恢复所有表 | 同上 | `conn_dhandle.c` | `P3/T3.1 重开连接` |
| T3.2 | `session->create/drop/open_cursor`、`conn->close` | `src_c/conn.cpp` | `session_api.c` | 同上 |
| T3.2 | 游标方法表：`insert/update/remove/search/modify`、变参收发 | `src_c/cursor.cpp` | `cur_file.c`、`bt_cursor.c` | 同上 |
| T3.3 | `next`/`prev`/`search_near`/`reset` 的顺序访问语义 | `src_c/cursor.cpp` | `bt_cursor.c` | `P3/T3.3 *`（6 项） |

要点：
- **`iface` 必须是结构体首成员**，这样 `WT_CONNECTION*` 能直接强转成 `twt_conn*`。
- 变参：`set_key`/`set_value` 是 `void(...)`，不是 `int(...)`。
- 键不存在返回 `WT_NOTFOUND`，**不是** `WT_ERROR`，更不是 `WT_ROLLBACK`。
- 重复插入返回 `WT_DUPLICATE_KEY`。
- 没有定位过的游标 `next()` 返回 `WT_NOTFOUND`（真 WT 不会替你从头扫）。
- 配置解析出来的 `WT_CONFIG_ITEM::str` 必须指向**生命周期够长**的存储，
  不能指向调用方传进来的临时缓冲区。

### P4 事务 / MVCC / HS / RTS / WAL（9 项）

> 目的：从「单线程能存能取」变成「有隔离级别、能回滚、能崩溃恢复」。
> 阅读：[P4.md](P4.md)。

| ID | 要写什么 | 文件 | 对标 WT | 验收 |
|---|---|---|---|---|
| T4.1 | 事务状态机、快照、`visible()` | `src/txn.cpp` | `txn.c`、`txn_inline.h` | `P4/T4.1 事务内读写`、`P4/T4.1 快照隔离` |
| T4.1 | `TxnSession::rollback`：精确摘链 | `src/txn.cpp` | `__wt_txn_rollback` | `P4/T4.1 rollback` |
| T4.2 | 写写冲突 → `S_ROLLBACK` | `src/txn.cpp` | 冲突队列（简化版） | `P4/T4.2 写写冲突` |
| T4.3 | History Store：按 ts 回查 + 回收 | `src/txn.cpp` | `history/hs.c` | `P4/T4.3 History Store` |
| T4.4 | rollback to stable | `src/txn.cpp` | `txn_rollback_to_stable.c` | `P4/T4.4 RTS` |
| T4.5 | WAL 追加 / 重放 / 截断 | `src/txn.cpp` | `src/log/` | `P4/T4.5 *`（3 项） |

要点：
- **读自己的写**必须在 `visible()` 最前面特判，否则自己写的值自己看不见。
- 快照判的是「begin 那一刻别人是否还活着」，不是「现在」。
- 回滚只能摘**自己挂的那个节点**，不能整条链丢掉（链上可能有别人的版本）。
- WAL 重放遇到半截记录要**停止但不报错**（崩溃时写了一半是正常现象）。

---

## 五、依赖关系（别乱序做）

```
        P0 (defs)
          │
          ▼
        P1 (block + cell)
          │
          ▼
   ┌── P2 (btree) ──┐
   │                │
   ▼                ▼
  P3 (C ABI)    P4 (txn/HS/RTS/WAL)
   │                │
   └──────┬─────────┘
          ▼
   Capstone: 链接真 wtperf / wt
```

- P2 没做完之前，P3/P4 全部会是 TODO（它们都建立在 `Btree` 上）。
- P3 的 `open/create` 依赖 P2 的 `checkpoint` 与元数据表扫描。
- P4 的 `TxnSession` 依赖 P2 的 update 链；`rollback` 需要能在链上精确删除。

---

## 六、Capstone：让真工具跑起来

P1–P4 全部通过之后，可以把 WiredTiger 官方工具链的 `wtperf` 直接链接到
`build/libtinywt.a` 上：

```bash
./scripts/fetch_wt_sources.sh          # 拉 wtperf + testutil 子集到 third_party/wt
make wtperf                            # 用 compat/wiredtiger.h 编译 + 链接 tinywt
./build/wtperf -O /tmp/smoke.wtperf
```

评分不是「跑分多少」，而是：

1. **能链接**：`ld` 不报未定义符号 —— 这等价于「你真把公开 API 补齐了」。
2. **能跑完**：`run_time=10` 的 workload 不崩、不返回错误。
3. **数据对**：`wtperf` 的 verify 阶段通过（它会按自己的校验规则随机回读）。

这就是「链接器当评分器」：少实现一个函数，链接就过不去。

---

## 七、交作业前的检查表

- [ ] `make grade` 里 31 项全 `PASS`
- [ ] `make test` 退出码为 0
- [ ] 没有残留的 `UNIMPLEMENTED`（`grep -rn UNIMPLEMENTED src/ src_c/`）
- [ ] 没有残留的调试 `printf`
- [ ] `make probe` 仍然通过（说明没动契约头）
- [ ] `git diff` 里没有改动 `test/`、`compat/`、`include/` 的签名
