# P2 任务书 —— Row-Store B-Tree

> 对标 BusTub P2（B+Tree Index）。这是整个项目最重的一个 Project：
> 骨架里一半的 `UNIMPLEMENTED` 都在这。
> 总纲与评分见 [PROJECTS.md](../PROJECTS.md)。验收：13 个测试项。

| 子项目 | 内容 | 函数 | 评分项 |
|---|---|---|---|
| T2.1 | 内存页三件套 + page_in | 9 | 1 |
| T2.2 | descend 搜索 | 2 | 1 |
| T2.3 | 插入 + 叶/内部分裂 | 5 | 1 |
| T2.4 | reconcile + eviction | 7 | 1 |
| T2.5 | checkpoint + 重开 + verify + 读可见性 | 8 | 2 |
| T2.6 | 编码家族（suffix / overflow / dict） | 8 | 4 |
| T2.7 | compressor API（RLE） | 2 | 3 |

## 前置

```bash
make p1-test     # 块设备 + cell 编码必须全绿
```

P2 的每一次 `page_in` 都要 `BlockManager::read`，每一次 reconcile 都要 `cell_pack_*`。
底层不对，上面的所有失败都会伪装成"B-Tree 逻辑错"，极难定位。

## 一张图

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

由此直接推出：

- 改 1 个字节 = 往 update 链头挂一个节点（O(1)），但 reconcile 要**整页重写**
- 页搬了家，父页镜像里的地址就失效了 → 父页必须标脏重写
- 旧版本不能直接删（旧快照还要读）→ P4 的 History Store

## T2.1 内存页三件套 + page_in（1 项）

| 结构 | 对标 | 语义 |
|---|---|---|
| `Entry{key, base, upd}` | `WT_ROW` + `WT_UPDATE` | 磁盘镜像里的一行 + 之后的修改链 |
| `Insert{key, upd, next[]}` | `WT_INSERT` | 磁盘镜像里**没有**的新键，有序跳表 |
| `Ref{key, addr, page_id, page, state}` | `WT_REF` | 父页指向子页的引用 |

`materialize_leaf()` 是这一项的核心：把「entries（有序）+ 跳表（有序）+ 各自的
update 链」归并成一份 `(key, value)` 列表。归并规则：

```
  两个有序流，各取最小：
    entries[i] vs ins 跳表当前节点
    key 相同        → entry 胜出（跳表节点只是它的新版本）
    取到之后沿 upd 链找第一个可见版本
    全链不可见      → entry 回落到 base；跳表节点 → 该键不存在
```

`free_update_chain` 要能释放整条链；`Page::~Page` 里调用它
（析构不能抛异常，所以里面只做 free）。

## T2.2 descend（1 项）

内部页二分下降：`refs[i].key <= target < refs[i+1].key` → 走 `refs[i]`。

`refs[i].key` 是**子树的下界**（`>=` 语义），不是"这棵子树里的第一个键"。
这两个说法在最左子树、以及 split 之后会分叉 —— 记错了会出现"某些键永远查不到"。

测试：插 5000 键后全量回读，顺序和值都对。

## T2.3 插入 + 分裂（1 项）

插入路径：

```
descend → 找到叶子 → 已存在就挂 update 链 / 不存在就插跳表
       → 超过 max_leaf_entries → split_leaf
       → 父页 refs 满了 → split_intl（可能级联到根，根分裂则树高 +1）
```

### 两个必踩的坑

**坑 1：左半页的分隔符漏更新。**
split 之后，右半页的第一个键成为新的分隔符插进父页；**左半页的旧分隔符**
如果指向的是被截断的旧页，也必须一起改。忘了这一步，左半页的所有键都会查不到。

**坑 2：分隔符「只降不升」。**
分隔符发生变化时，很多实现只处理"变大"的情况（插入新分隔符），
忘了"变小"（拆分后左半页的下界变了）。规则是：**父页里的分隔符必须始终等于
子树的下界**，方向不限。

测试：对拍 `std::map`，4 组种子 × 2 种 cache 压力（40 页疯狂淘汰 / 1000 页几乎不淘汰）。

## T2.4 reconcile + eviction（1 项）

`reconcile_page`：把内存页写成一份新镜像。

```
  leaf   : 按 key 升序，逐个 cell_pack_key（带前缀压缩）+ cell_pack_value
  intl   : 按 ref 顺序，cell_pack_key（子树下界）+ cell_pack_addr（新地址）
  写完   → BlockManager::write → 新 Addr → 更新 ref->addr → 父页标脏
```

`reconcile_tree` 必须**后序**（先子后父）：子页换了地址，父页镜像里的 cookie
才需要更新；先父后子会写出一份引用旧地址的父页镜像。

`evict_one` 的流程：

```
  1. 从 LRU 尾取一个候选页
  2. 跳过 pinned（正在被 descend 路径引用）
  3. 脏 → reconcile_page
  4. 断 parent_ref、从 owned_ 移除、ref->page = nullptr
```

**pin 泄漏**是最常见的死法：路径上的页忘了 unpin → 所有候选都被跳过 →
`evict_if_needed` 死循环或直接放弃 → 内存无限增长。
测试用 `cache=3` + 1500 键强制频繁淘汰（断言 `evictions > 0`，真 WT 量级在千次以上），
一旦 pin 泄漏立刻表现为卡死或 OOM。

## T2.5 checkpoint + 重开 + verify + 读可见性（2 项）

### checkpoint

```
  reconcile_tree(root)        // 后序，全部脏页落盘
  → SuperBlock{root_page_id, root_addr, next_page_id, avail}
  → bm_.checkpoint_extents(avail)   // discard 合并
  → bm_.write_super(sb)             // 提交点
```

**写超级块是提交点**：它之前的任何崩溃 = 上一次 checkpoint 仍然有效；
它之后 = 新的有效。所以超级块必须最后写、且一次写成。

### verify

页内 key 有序、父页分隔符等于子树下界、每个 addr 都能读回来且 checksum 对。
这是唯一能自动发现"结构悄悄坏了"的手段，checkpoint 之后必须跑一遍。

### 读可见性

```cpp
bool update_visible(const Update *u, u64 reader_txnid, u64 read_ts);
Status entry_read(const Entry &e, u64 reader_txnid, u64 read_ts, Val &out, bool &found);
```

P2 只需要朴素规则：写事务 id <= 读者快照上界，且 `start_ts <= read_ts`。
P3 的 `TxnManager::visible()` 是它的完整版，P4 再加上 History Store 下探。

## T2.6 编码家族（4 项）

| 机制 | 作用 | 真 WT 位置 |
|---|---|---|
| suffix truncation | internal key 只留"能区分相邻子树"的前缀 | `rec_write.c:723` / `__wt_cell_pack_int_key` |
| overflow page | 超长 key/value 搬到独立块，cell 只存 Addr | `WT_CELL_KEY_OVFL` / `WT_CELL_VALUE_OVFL` |
| value dictionary | 页内重复 value 只存一次，其余存槽位号 | `WT_CELL_VALUE_COPY` |

`suffix_truncate(key, next_key)` 的规则：取与 `next_key` 的公共前缀长度 **+1**
（多留 1 字节保证仍严格区分）；没有 `next_key`（最右子树）则不截断。
返回 `>= 1`，且只有 `< key.size()` 时才真的截断。

为什么 internal key 只需要前缀：内部页的 key 只用于路由，
「能区分左右子树」就够了。真 WT 的注释里把这件事叫"discarding unnecessary bytes"。

## T2.7 compressor API（3 项）

```
  add_compressor("rle", ...) + compression="rle"
  → reconcile 出镜像 → compress_page → BlockManager::write
  → read → decompress_page → page_in
```

先内建 RLE（`{u8 value}{varint run_len}`），再挂真 snappy（`ext/compressors/snappy/`）。
注意：**压缩发生在块写入之前**，所以 `BlockHeader.disk_size` 是压缩后的大小，
`Addr.size` 也是压缩后的 —— 两个数一致，否则 `read` 会算出错的镜像长度。

解压失败（恶意/损坏的 RLE 流）必须返回 `S_CORRUPT`，不能越界读。
这条和 `cell_unpack` 的边界检查是同一类要求：**所有来自磁盘的长度都必须验证**。

## 评分测试

| 测试 | 覆盖 |
|---|---|
| `test/p2/btree_test.cpp` | 页三件套、descend 5000 键、对拍 std::map ×4、reconcile/eviction、checkpoint 重开、读可见性 |
| `test/p2/codec_test.cpp` | suffix truncation、overflow、value dict、三种编码同时开启 |
| `test/p2/compress_test.cpp` | RLE 往返、损坏流报 CORRUPT、页压缩后文件更小且数据一致 |

## 复习题

1. 为什么 internal key 是子树下界而不是精确键？（答到 suffix compression 的源码依据算满分）
2. 内存页三件套各自的 reconcile 归并顺序？
3. prefix 与 suffix 压缩分别作用于哪层页，为什么不能互换？
4. `reconcile_tree` 为什么必须后序？
5. eviction 时 pin 泄漏会以什么形式暴露？为什么 `cache=3` 能把问题放大？
