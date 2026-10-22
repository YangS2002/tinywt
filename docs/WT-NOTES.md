# tinywt 阅读材料：WiredTiger 的骨架

这份文档是**做作业前先读**的背景材料，不是任务书。任务书见
[PROJECTS.md](PROJECTS.md) 与各 `P*.md`。

所有结论都对着 `wiredtiger/wiredtiger` 仓库 `mongodb-8.0` 分支核实过。

---

## 一、核心：内存页 ≠ 磁盘页

这是整个 WiredTiger 最该先吃透的一条。其余所有机制都是它的推论。

```
                磁盘镜像 (disk image, 不可变)
                ┌─────────────────────────────────┐
                │ PageHeader │ cell │ cell │ ...   │
                └─────────────────────────────────┘
                       │  page_in：读块 + 建索引
                       ▼
   内存页 Page（可变）
   ┌──────────────────────────────────────────────────────────┐
   │ 1. entries[]      磁盘镜像解出的键      ← 对标 WT_ROW[]    │
   │ 2. Entry::upd     每个键一条 update 链，链头最新 ← WT_UPDATE │
   │ 3. ins_head[]     不在镜像里的新键（跳表）  ← WT_INSERT     │
   └──────────────────────────────────────────────────────────┘
                       │  reconcile：把上面三者归并
                       ▼
                新的磁盘镜像 → block_write → 新的 Addr（新块）
```

直接推论（面试常问，且能从这张图推出来）：

- 改 1 个字节 = 往 update 链头挂一个 `Update`（内存 O(1)），但 reconcile 时**整页重写** → 写放大的根源
- 二级索引查询 = 索引 → RecordId → 回表：本质就是又一次 "Addr → page_in"
- 旧版本在 reconcile 时被挪进 History Store（P4 的任务）

---

## 二、文件 ↔ 真 WT 源码对照表

| tinywt 文件 | 真 WT 源码 | 复刻了什么 |
|---|---|---|
| `include/twt_defs.h` | `src/include/error.h`、`misc.h`、`intpack_inline.h` | `WT_RET` 宏式状态码、Slice/WT_ITEM、变长整数、checksum |
| `include/twt_cell.h`<br>`src/cell.cpp` | `src/include/cell.h`<br>`src/cell/cell_pack.c`<br>`src/cell/cell_unpack.c` | **cell 描述符位布局**（`WT_CELL_KEY_SHORT 0x01` / `KEY_SHORT_PFX 0x02` / `VALUE_SHORT 0x03` / `CELL_64V 0x04` / `SECOND_DESC 0x08`，类型位在 bit5-8）、key 前缀压缩 |
| `include/twt_block.h`<br>`src/block.cpp` | `src/include/block.h`<br>`src/block/block_{open,write,read,ext,addr}.c` | `WT_BLOCK_HEADER{disk_size, checksum}`、offset 0 非法（首块是描述块）、三张 extent 表 alloc/avail/**discard**、addr cookie 打包 |
| `include/twt_page.h`<br>`src/page.cpp` | `src/include/btmem.h`<br>`src/btree/row_srch.c`<br>`src/btree/row_ins.c` | `WT_PAGE_HEADER`、`WT_UPDATE`、`WT_INSERT` 跳表、`WT_REF`、可见性读取 |
| `src/btree.cpp` | `src/btree/bt_read.c`<br>`bt_search.c` / `bt_split.c` / `bt_sync.c`<br>`bt_cursor.c` / `src/cache/` | page_in / descend / LRU + eviction / split / checkpoint / 游标遍历 |
| `src/reconcile.cpp` | `src/reconcile/rec_write.c`<br>`src/reconcile/rec_row.c` | 内存页 → 磁盘镜像、物化 update 链、换 Addr、把父页标脏 |
| `src_c/*.cpp` | `src/conn/conn_api.c`<br>`src/session/session_api.c`<br>`src/cursor/cur_file.c`<br>`src/config/config*.c` | C ABI 句柄、方法表、配置解析 |
| `src/txn.cpp` | `src/txn/txn.c`<br>`src/include/txn_inline.h`<br>`src/history/hs.c`<br>`src/log/` | 事务状态机、可见性、History Store、RTS、WAL |

### 三个结构体的逐字段对照（已按源码核实）

```
WT_ADDR  (src/include/btmem.h:136)          tinywt Addr (twt_defs.h)
  uint8_t *addr;  /* block manager cookie */   u32 off;
  uint8_t  size;  /* cookie 长度 */            u32 size;   ← 镜像精确长度
  uint8_t  type;  /* INT/LEAF/LEAF_NO */       u32 alloc;  ← 按 alloc_size 对齐的块大小
                                               u32 cksum;
  cookie 内容 = varint(off) varint(size) varint(cksum)

WT_UPDATE (src/include/btmem.h:1386)        tinywt Update (twt_page.h)
  volatile uint64_t txnid;                    u64 txnid;
  wt_timestamp_t durable_ts, start_ts;        u64 start_ts, durable_ts, stop_ts;
  WT_UPDATE *next;                            Update *next;
  uint32_t size;                              u8 type;   ← UPD_STANDARD / UPD_TOMBSTONE
  uint8_t type;                               Val data;

WT_PAGE_HEADER (src/include/btmem.h:51)     tinywt PageHeader (twt_page.h)
  uint64_t recno;      /* 00-07 */            u64 recno;
  uint64_t write_gen;  /* 08-15 */            u64 write_gen;
  uint32_t mem_size;   /* 16-19 */            u32 mem_size;
  union { entries; datalen; } /* 20-23 */     u32 entries;
  uint8_t type;        /* 24 */               u8 type;
  uint8_t flags;       /* 25 */               u8 flags;
```

---

## 三、有意的简化（读代码时别当真 WT 就这样）

1. **`Addr` 多了一个 `alloc` 字段**：真 WT 的 `__wt_block_free()` 额外接一个 `len` 参数，
   不把对齐后的块大小写进 cookie。
2. **`Ref` 多了一个 `page_id`**：真 WT 用「父页里的 WT_REF 槽位」标识页，没有全局 page_id；
   本版为了给 cache 做 key 引入。
3. **磁盘镜像一次性解码成 `entries[]`**：真 WT 惰性解码（`WT_ROW` 只存偏移，需要时才构造
   `WT_IKEY`）。结构等价，可读性更好。
4. **只淘汰叶子页**：淘汰内部页要处理子页 `parent_ref` 悬空，学习阶段不值得。
5. **单个 checkpoint**：真 WT 保留多个 checkpoint，所以 discard 表必须跨 checkpoint 存活。
6. **extent 表用有序 vector + first-fit**：真 WT 是双跳表（by-offset + by-size）+ best-fit。
7. **tombstone 在 reconcile 时直接删键**：真 WT 只有在该删除对所有读者都可见时才这么做。
8. **History Store 是内存 map**：真 WT 是一棵独立的 B-Tree 文件。
9. **WAL 是表级 redo**：没有 checkpoint LSN 管理，恢复策略是「日志重放」。

### 没做（对应真 WT 的一大块）

| 没做 | 真 WT 在哪 |
|---|---|
| **suffix compression（internal key 截断）** | `src/reconcile/rec_write.c:723`，配置 `internal_key_truncate`（默认 true） |
| 未提交修改的落盘处理 | `rec_write.c` "unresolved modifications" |
| 时间窗口聚合（`WT_TIME_AGGREGATE`） | cell 的第二个描述字节 |
| overflow / 字典压缩 / 页面压缩 | `WT_CELL_VALUE_OVFL`、`WT_CELL_VALUE_COPY` |
| 并发：hazard pointer、`WT_REF` 状态机、split generation | `src/include/hazard.h`、`btmem.h` |
| column-store | `WT_PAGE_COL_*` |

---

## 四、两个真坑（P2 会踩到，先看答案也行）

### 坑 1：叶子页分裂时忘了更新左半页的分隔符

```
分裂前：refs = [A(sep=100, 实际最小 key=5), ...]     ← 分隔符"偏大"
                                                       （比全树最小 key 更小的键会被 descend
                                                         塞进最左子树，best 默认取 0）
只插入右半页的新分隔符：refs = [A(100), R(11), ...]   ← 不再有序！
后续二分查找走错子树 → 小 key 读不到
```

### 坑 2：修正分隔符时**只能降、不能升**

```
内部页 P 的分隔符 = S，它的第一个子节点 key = K。
descend 找不到「key <= target」的子节点时会退回 index 0。
所以只要 K > S，区间 [S, K) 就没有任何叶子页覆盖 —— 落在里面的键直接丢失。

不变式：
  (1) 同一层 Ref 的 key 非降序
  (2) 子树首个子节点的 key <= 该子树自己的分隔符
⇒ 分裂时左半页分隔符只允许变小
```

### 这两个坑在真 WT 里对应什么：suffix compression

WT 不是"碰巧"让 internal key 变成下界的，是**主动**做的。
`src/reconcile/rec_write.c:723`（mongodb-8.0 分支）：

```c
/*
 * Suffix compression shortens internal page keys by discarding trailing bytes
 * that aren't necessary for tree navigation. We don't do suffix compression if
 * there is a custom collator because we don't know what bytes a custom
 * collator might use.
 */
r->key_sfx_compress_conf = false;
if (btree->collator == NULL && btree->internal_key_truncate)
    r->key_sfx_compress_conf = true;
```

WT 里有两套 key 压缩，别混：

| | 作用对象 | 做法 |
|---|---|---|
| **prefix** compression（P1 实现） | **leaf** page keys | 压掉与前一个 key 的公共前缀 |
| **suffix** compression（未实现） | **internal** page keys | 丢掉尾部字节 → internal key 变成子树 range 的前缀/下界 |

也就是说：坑 1/坑 2 里的"分隔符必须是下界"，在 WT 里是一个**默认开启的正式特性**
（`internal_key_truncate` 默认 true，见 `dist/api_data.py`；有自定义 collator 时关闭）。

顺带一条：WT 的配置默认值别靠猜，去 `dist/api_data.py` 看，它是所有配置项
语义/默认值/范围的单一事实来源。例如 WT 默认 `leaf_page_max=32KB` /
`internal_page_max=4KB`，`split_pct` 默认就是 `90`。

---

## 五、错误码契约（P0 就要记住）

真 WT 的错误码在 `dist/api_err.py` 里定义，**一个字都不能改**：

| 常量 | 值 |
|---|---|
| `WT_ROLLBACK` | -31800 |
| `WT_DUPLICATE_KEY` | -31801 |
| `WT_ERROR` | -31802 |
| `WT_NOTFOUND` | -31803 |
| `WT_PANIC` | -31804 |
| `WT_RESTART` | -31805 |

`compat/probe.c` 里有 `_Static_assert(WT_NOTFOUND == -31803)` 之类的检查；
`include/twt_defs.h` 的 `Status` 必须逐个对齐。

为什么较真：真 wtperf 会拿返回值直接和 `WT_NOTFOUND` 比。如果 `S_NOTFOUND`
被写成 `-31800`，它就等于 `WT_ROLLBACK` —— 工具会把"键不存在"当成
"事务冲突，需要重试"，然后无限重试下去。
