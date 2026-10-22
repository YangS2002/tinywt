# P1 任务书 —— 磁盘格式：块 / extent / cell

> WT 的磁盘格式层（`src/block/` + `src/cell/`）。这一层回答「一个 `.wt` 文件长什么样」：
> 块怎么定位（addr cookie）、空间怎么回收（extent 三表）、一条记录怎么落字节（cell）。
> 注意它和内存页是两回事 —— B+Tree 的逻辑页到 P2 才出现，这里只有块和字节。
> 总纲与评分见 [PROJECTS.md](../PROJECTS.md)。验收：3 个测试项。

| 子项目 | 内容 | 函数 | 评分项 |
|---|---|---|---|
| T1.1 | 块读写 + 校验 | 4 | 1 |
| T1.2 | extent 三表 + 超级块 | 11 | 1 |
| T1.3 | cell 编解码 | 5 | 1 |

## 前置

P0 的 `crc32` 必须先对。块校验、页校验全建在它上面。

```bash
make p0-test
```

## 文件布局

```
  文件
  ┌──────────┬──────────┬──────────┬──────────┬─────
  │ block 0  │ block 1  │ block 2  │ block 3  │ ...
  │ 描述块   │ 数据块   │ 数据块   │ 数据块   │
  └──────────┴──────────┴──────────┴──────────┴─────
    off=0 非法   off=alloc_size

  每个数据块：
  ┌────────────────────┬──────────────────────────────┐
  │ BlockHeader (8B)   │ 页镜像（size 字节）           │
  │ disk_size, cksum   │  真正的页面内容               │
  └────────────────────┴──────────────────────────────┘
  └────────────── alloc（对齐后占用的字节数）──────────┘
```

**`off == 0` 是非法偏移**，可以当 `nullptr` 用。首块被文件描述块占住，
所以 `Addr::valid()` 判的是 `off != 0`。

## T1.1 块读写 + 校验（1 项）

> **真 WT**：`src/block/block_read.c: __wt_block_read_off`、`src/block/block_write.c: __wt_block_write_off`；校验和在 `src/checksum/`。

```cpp
Status BlockManager::write(const u8 *image, size_t size, Addr &out);
Status BlockManager::read(const Addr &a, std::vector<u8> &image);
```

### size 与 alloc 是两个数

| 字段 | 含义 | 谁用 |
|---|---|---|
| `size` | 镜像的**精确**字节数 | 读的时候只读这么多 |
| `alloc` | 按 `alloc_size` 对齐后**实际占用**的字节数 | 下一块的偏移、extent 表 |

混用会怎样：用 `size` 算下一块偏移 → 块未对齐 → 下一块跨在两个对齐边界上，
`write` 时覆盖上一块尾部；用 `alloc` 当镜像长度 → 读到填充字节，
页镜像解析出多余的 cell。

### 校验和存两份

```
  BlockHeader.checksum    ← salvage 用：整页被覆盖后仍能判断"这不是我的页"
  Addr.cksum              ← 父页引用里也存一份
  read 时三方比对：image 的 crc32 == a.cksum == header.checksum
```

只存一份的漏洞：如果一块被另一张**结构合法**的页覆盖了（比如误写），
header 里的 checksum 也是自洽的，读回来完全正常 —— 父页里的地址却指向了错的内容。
两份都存，才能在「父页说这里是 P，实际读到 Q」时发现。

### 测试在考什么

往返一致；**改坏 1 个字节必须报 `S_CORRUPT`**（不是 `S_ERROR`、不是崩溃）。

## T1.2 extent 三表 + 超级块（1 项）

```
  alloc    : 本 checkpoint 分配的 extent
  avail    : 可分配（空闲）的 extent，按 off 有序
  discard  : 本 checkpoint 释放的 extent
```

### 为什么需要 discard 表

```
  checkpoint N    : 页 P 的旧块 off=4096 被释放
  如果立刻回到 avail
  然后新页 Q 抢到 off=4096 并写入
  此时若还要用 checkpoint N 的数据恢复 → 读到的是 Q，P 没了
```

所以释放先进 `discard`，**下一次** checkpoint 才合并回 `avail`。
真 WT 保留多个 checkpoint，所以这条规则是硬约束；本版只留单个 checkpoint，
`discard` 的语义仍然保留 —— 它是「释放后不能立刻复用」这条规则的载体。

### 分配与合并

- 分配：`avail` 上 first-fit（真 WT 是 by-size best-fit 跳表）
- 释放：插入 `discard`，checkpoint 时合并进 `avail`
- 合并：`coalesce` 要把**相邻**的空闲 extent 合成一块，否则反复分配释放之后
  文件里全是碎片，最终「有空间但没连续的块」

### 超级块

```cpp
struct SuperBlock {
    u64 magic, version, alloc_size, file_size, root_page_id, next_page_id;
    Addr root_addr;
    u32 flags;          // SBF_DIRTY_SHUTDOWN (P4/T4.4)
    u64 stable_ts;      // P4/T4.5
    u64 ckpt_lsn;       // P3/T3.5 日志截断点
    std::vector<Extent> avail;   // 持久化空闲表
};
```

超级块本身也走 `write`（一块），所以它是**最后一个**被写的 ——
写超级块成功 = checkpoint 成功。这是 WAL 之外的另一条"原子提交点"。

## T1.3 cell 编解码（1 项）

cell 是磁盘页里的记录格式，每个 cell 以 1 字节描述符开头：

```
  bit 1-2 : short 类型   00 非 short
                         01 short key
                         10 short key + 前缀
                         11 short value
            short 变体下其余 6 位是长度（<= 63）
  bit 3   : CELL_64V
  bit 4   : CELL_SECOND_DESC
  bit 5-8 : 类型
```

```
  非 short key cell：
  ┌──────┬──────────────────┬───────────────┐
  │ desc │ varint 后缀长度  │ 后缀字节      │
  └──────┴──────────────────┴───────────────┘

  前缀压缩 key cell（CELL_KEY_PFX）：
  ┌──────┬────────┬───────────────┐
  │ desc │ prefix │ 后缀字节      │
  └──────┴────────┴───────────────┘
          1 字节：与页内前一个 key 的公共前缀长度
```

三个坑：

1. **short 判定的边界是 63**（6 位能表示 0..63）。64 字节的 value 必须走 long 形式。
   真 WT 里 `CELL_SHORT_MAX = 63`。
2. **前缀压缩只作用于 leaf 页的 key**，且 prefix 是「与前一个 key」的公共前缀，
   第一个 key 的 prefix 恒为 0。internal 页是另一种压缩（suffix truncation，T2.6）。
3. **`cell_unpack` 必须做边界检查**（`avail`）。真 WT 的函数叫
   `__wt_cell_unpack_safe` —— 名字里的 safe 就是这个意思。磁盘数据可能是坏的，
   越界读会变成难以定位的随机崩溃。

### addr cookie

```
  cookie = varint(off) varint(size) varint(cksum) varint(page_id)
```

真 WT 的 `WT_ADDR` 对上层是**不透明字节串**（`__wt_block_addr_pack` 打包，
上层不解析）。本版展开成 4 个 varint，语义等价、便于调试。
`page_id` 是有意的简化：真 WT 的 `WT_REF` 用「父页里的槽位」标识子页，
没有全局 id。

## 评分测试

| 测试 | 覆盖 |
|---|---|
| `test/p1/block_cell_test.cpp` | 块往返 + 单比特损坏、extent 回收 + 超级块重开、cell 三种类型往返 + 前缀还原 |

## 复习题

1. 为什么 `offset 0` 非法？（答：首块是描述块）
2. `alloc` 与 `avail` 两张表，为什么 `discard` 必须跨 checkpoint 存活而 `avail` 不用？
3. addr cookie 为什么对 B-Tree 层是不透明的？
4. 校验和为什么存两份？只存 header 里会漏掉哪种损坏？
