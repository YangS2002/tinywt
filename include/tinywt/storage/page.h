/*-
 * 对标 WiredTiger: src/include/btmem.h
 *
 * ---------------------------------------------------------------------------
 * ★ 这是整个项目最该吃透的一张图 ★
 *
 * WiredTiger 的精髓在于：**内存页 ≠ 磁盘页**。
 *
 *                 磁盘镜像 (disk image, 不可变)
 *                 ┌───────────────────────────────────┐
 *                 │ PageHeader │ cell │ cell │ cell ... │
 *                 └───────────────────────────────────┘
 *                        │  page read：读入 + 建索引
 *                        ▼
 *   内存页 WT_PAGE（可变）
 *   ┌─────────────────────────────────────────────────────┐
 *   │ 1. pg_row[]        磁盘镜像里的键的索引（WT_ROW）     │  ← 对应下面的 entries[]
 *   │ 2. update chain    每个键挂一条 WT_UPDATE 链，头最新   │  ← 对应 Entry::upd
 *   │ 3. insert list     不在磁盘镜像里的新键（WT_INSERT    │  ← 对应 Page::ins_head
 *   │                    跳表，有序）                       │
 *   │ 4. mod->           脏字节记账 / 替换块地址 WT_ADDR    │
 *   └─────────────────────────────────────────────────────┘
 *                        │  reconcile：把「磁盘镜像 + 更新链 + insert 表」
 *                        │            归并成一份新的磁盘镜像
 *                        ▼
 *                 新的磁盘镜像 → block_write → 新的 Addr
 *
 * 由此直接推出一个关键结论（面试常问）：
 *   改 1 个字节 = 往 update chain 头挂一个 WT_UPDATE（内存里 O(1)），
 *   但 reconcile 时整页都要重写 → 这就是「写放大」的根源。
 *   而旧版本在 reconcile 时被挪进 History Store（Phase 7 实现）。
 * ---------------------------------------------------------------------------
 * 简化说明：
 *   - 真 WT 惰性解码磁盘镜像（WT_ROW 只存偏移，需要时才构造 WT_IKEY）；
 *     这里一次性解码成 entries[]，结构等价、可读性更好
 *   - WT_INSERT 跳表节点层级是可变的（next[0] 柔性数组），这里固定 SKIP_MAXLEVEL
 *   - 没有 hazard pointer / WT_REF 状态机 / split generation（单线程版不需要）
 * -*/
#pragma once

#include <cstdint>
#include <vector>

#include "tinywt/common/defs.h"

namespace twt {

/* 对标 WT_PAGE_HEADER (src/include/btmem.h:51) */
struct PageHeader {
    u64 recno = 0;      /* 00-07: column-store 起始 recno（row-store 不用） */
    u64 write_gen = 0;  /* 08-15: 写世代 */
    u32 mem_size = 0;   /* 16-19: 磁盘镜像实例化成内存后的字节数 */
    u32 entries = 0;    /* 20-23: 页内 cell 数 */
    u8 type = 0;        /* 24   : 页类型 */
    u8 flags = 0;       /* 25   : WT_PAGE_COMPRESSED / WT_PAGE_EMPTY_V_ALL ... */
};
constexpr size_t PAGE_HEADER_SIZE = 32;

/* 对标 src/include/btmem.h 里的 WT_PAGE_INVALID / WT_PAGE_ROW_INT / WT_PAGE_ROW_LEAF */
enum PageType : u8 {
    PAGE_INVALID = 0,
    PAGE_ROW_INT = 1,  /* row-store 内部页：key cell + addr cell 成对 */
    PAGE_ROW_LEAF = 2, /* row-store 叶子页：key cell + value cell 成对 */
};

/* 对标 WT_UPDATE_STANDARD / WT_UPDATE_TOMBSTONE (btmem.h) */
enum UpdateType : u8 {
    UPD_INVALID = 0,
    UPD_MODIFY = 1,
    UPD_RESERVE = 2,
    UPD_STANDARD = 3,
    UPD_TOMBSTONE = 4,
};

/*
 * Update：对标 WT_UPDATE (btmem.h:1386)
 *   txnid / start_ts / durable_ts / next / size / type / data
 *   链头是最新版本；可见性判断沿链往下找第一个对读者可见的版本。
 */
struct Update {
    u64 txnid = 0;      /* 写这个版本的事务 ID */
    u64 start_ts = 0;   /* 提交时间戳 */
    u64 durable_ts = 0;
    u64 stop_ts = 0;    /* 被下一个版本覆盖的时间戳（Phase 7 用） */
    Update *next = nullptr;
    u8 type = UPD_STANDARD;
    Val data;

    bool is_tombstone() const { return type == UPD_TOMBSTONE; }
};

/* 对标 WT_SKIP_MAXLEVEL (btmem.h) */
constexpr int SKIP_MAXLEVEL = 4;

/*
 * Insert：对标 WT_INSERT (btmem.h)
 *   磁盘镜像里没有的新键，走跳表；每个节点同样挂一条 update 链。
 */
struct Insert {
    Key key;
    Update *upd = nullptr;
    Insert *next[SKIP_MAXLEVEL] = {nullptr, nullptr, nullptr, nullptr};
};

/*
 * Ref：对标 WT_REF (btmem.h)
 *   父页指向子页的引用。addr = 子页在磁盘上的位置；page = 子页的内存实例。
 *   page == nullptr 表示「只在磁盘上」，需要 page_in 读进来。
 */
/*
 * RefState：对标 WT_REF 的状态机（btmem.h: WT_REF_DISK / READING / MEM / LOCKED）
 *
 * 单向迁移：
 *     DISK ──page_in──▶ READING ──▶ MEM ◀──▶ LOCKED（eviction 抢占）
 *
 * ★ 为什么需要 LOCKED 而不直接加锁：
 *   eviction 要先把页从「可被别人 page_in」的状态摘出来，否则会出现
 *   「A 正在读、B 把它 reconcile 掉、A 拿着旧 Page*」的悬空。
 *   WT 用 CAS 把 MEM→LOCKED，赢的人才拥有这个 ref。
 *
 * P2 单线程版不用它（state 恒为 MEM 或 DISK）；P4/T4.2 才要求真正维护。
 */
enum RefState : u8 {
    REF_DISK = 0,
    REF_READING = 1,
    REF_MEM = 2,
    REF_LOCKED = 3,
};

struct Page;
struct Ref {
    Key key;       /* 该子树的最小键（父页里的分隔符） */
    Addr addr;     /* 磁盘地址（page == nullptr 时有效） */
    u64 page_id;   /* 稳定的页标识 */
    Page *page;    /* 内存页（nullptr = 未载入 / 已被 evict） */
    Page *home = nullptr; /* 持有这个 Ref 的父页（子页换 addr 后要把父页标脏） */
    bool deleted;
    RefState state = REF_DISK; /* P4/T4.2：并发 page_in / eviction 的状态机 */

    Ref() : page(nullptr), home(nullptr), deleted(false) {}
};

/*
 * Entry：叶子页里「磁盘镜像解出来的一行」
 *   对标 WT_ROW（键）+ 挂在它上面的 WT_UPDATE 链。
 *   base = 磁盘镜像里那个版本的值；upd = 之后发生的所有修改。
 */
struct Entry {
    Key key;
    Val base;
    Update *upd = nullptr;
};

struct Page {
    PageType type = PAGE_INVALID;
    u64 page_id = 0;
    bool dirty = false;
    u64 write_gen = 0;
    Ref *parent_ref = nullptr; /* 对标 WT_REF：回指父页里的引用 */

    /* --- 叶子页：磁盘镜像 + 更新链 + insert 跳表 --- */
    std::vector<Entry> entries;              /* 对标 pg_row[] */
    Insert *ins_head[SKIP_MAXLEVEL] = {};    /* 对标 mod->mod_row_insert[] */
    u32 ninserts = 0;                        /* insert 表里的键数 */

    /* --- 内部页 --- */
    std::vector<Ref *> refs; /* 用裸指针：内部页 split 移动时地址要稳定 */

    /* --- cache / LRU --- */
    Page *lru_prev = nullptr;
    Page *lru_next = nullptr;
    bool in_cache = false;
    bool pinned = false; /* 正在被某次 descend 路径引用，不可 evict */

    Page() = default;
    ~Page(); /* 释放 update 链 + insert 跳表 + refs */

    Page(const Page &) = delete;
    Page &operator=(const Page &) = delete;
};

/* -------------------------------------------------------------------------
 * insert 跳表操作（对标 src/btree/row_srch.c / row_ins.c 里的 __wt_insert_*）
 * ------------------------------------------------------------------------- */

/* 找到第一个 key >= target 的节点；同时可回填搜索路径 */
Insert *ins_search(Page *page, Slice target, Insert **path = nullptr);
/* 精确查找，返回节点或 nullptr */
Insert *ins_find(Page *page, Slice target);

/*
 * ins_append --
 *     往跳表里插入一个新键（或其 update 链上追加一个新版本）。
 *     对标 __wt_row_insert() / __wt_update_append() 的组合行为。
 */
Status ins_append(Page *page, Slice key, Update *upd);

/* -------------------------------------------------------------------------
 * MVCC 可见性（对标 src/include/txn_inline.h 的 __wt_txn_visible* 简化版）
 *
 * P2 只需要最朴素的规则：写事务 ID <= 读者快照上界，且提交时间戳 <= 读时间戳。
 * P4 会把它换成真正的 snapshot（TxnManager::visible），这条规则退化为
 * 「无事务时的默认可见性」。
 * ------------------------------------------------------------------------- */
bool update_visible(const Update *u, u64 reader_txnid, u64 read_ts);

/*
 * entry_read --
 *     沿 update 链找第一个对读者可见的版本。
 *     全链都不可见 → 回落到磁盘镜像里的 base。
 */
Status entry_read(const Entry &e, u64 reader_txnid, u64 read_ts, Val &out, bool &found);

/* 释放一条 update 链（对标 __wt_free_update_list） */
void free_update_chain(Update *u);

/*
 * materialize_leaf --
 *     把「磁盘镜像解出的 entries + update 链 + insert 跳表」归并成
 *     一份有序的 (key, value) 列表。reconcile 和 split 都用它。
 *     对标 src/reconcile/rec_row.c 的 __rec_row_leaf() 归并遍历。
 */
void materialize_leaf(Page *page, std::vector<Entry> &out);

/* insert 跳表节点的可见性读取（没有 base 版本，全链不可见 = 不存在） */
Status insert_read(Insert *n, u64 reader_txnid, u64 read_ts, Val &out, bool &found);

/* 计算两个 key 的公共前缀长度（reconcile 做前缀压缩时用） */
size_t common_prefix(const Key &a, const Key &b);

} /* namespace twt */
