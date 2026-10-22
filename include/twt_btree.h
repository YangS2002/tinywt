/*-
 * 对标 WiredTiger: src/btree/ 下的 .c 文件 + src/include/btree.h
 *
 * 主要对标文件：
 *   bt_read.c   : __wt_page_in / __wt_page_release        → Btree::page_in / unpin
 *   bt_search.c : __wt_row_search / __wt_search_insert     → Btree::descend / ins_search
 *   bt_split.c  : __wt_leaf_split / __wt_split_insert      → Btree::split_leaf
 *   bt_write.c  : __wt_bt_write                            → Btree::write_page
 *   bt_sync.c   : __wt_bt_cache_sync / checkpoint          → Btree::checkpoint
 * -*/
#pragma once

#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "twt_block.h"
#include "twt_compress.h"
#include "twt_defs.h"
#include "twt_page.h"

namespace twt {

struct Config {
    size_t max_leaf_entries = 32; /* 叶子页 entries+inserts 超过这个数就分裂 */
    size_t max_intl_refs = 32;    /* 内部页 refs 上限 */
    size_t max_cached_pages = 8;  /* cache 里最多驻留多少页 → 触发 eviction */
    u32 alloc_size = 4096;
    bool verbose = false;

    /*
     * --- P3/T3.1 要接进来的建表配置（session->create 的 config 串） ---
     *
     * ★ 这里是「字节上限」和「条目上限」两种口径并存：
     *   P2 的测试用 max_leaf_entries 直接控制页大小（好断言、好对拍）；
     *   T3.1 的 `create` 拿到的是 leaf_page_max=32KB 这种字节数，必须换算。
     *   换算规则（T3.1 实现时写死并注释）：
     *     entries ≈ leaf_page_max / 平均 cell 大小（key + value + 4B 描述符）
     *   或者反过来：reconcile 时按累计字节数判满，让 max_leaf_entries 只做兜底。
     *   两种都行，但不能把 leaf_page_max 当条目数直接用 —— 差值一个数量级。
     */
    size_t leaf_page_max = 32 * 1024;     /* 默认与真 WT 一致 */
    size_t internal_page_max = 4 * 1024;  /* 默认与真 WT 一致 */
    u32 split_pct = 90;                   /* 分裂时左页占的百分比，真 WT 默认 90 */

    /* --- P2/T2.6 编码家族 --- */
    bool internal_key_truncate = true; /* suffix compression：internal key 截断 */
    bool use_overflow = true;          /* 超长 key/value 写独立块 */
    size_t value_overflow_size = 512;  /* 超过这个长度就 overflow */
    bool dictionary = true;            /* 页内重复 value 用 CELL_VALUE_COPY */

    /* --- P2/T2.7 页压缩（作用于块写入之前） --- */
    Compression compression = Compression::NONE;
};

struct Stats {
    u64 puts = 0;
    u64 gets = 0;
    u64 removes = 0;
    u64 splits = 0;
    u64 page_reads = 0;
    u64 page_writes = 0;
    u64 evictions = 0;
    u64 blocks_freed = 0;
};

/*
 * 游标遍历状态（对标真 WT 的 WT_CURSOR_BTREE：会话栈 + 当前叶位置）
 *
 *   pages/idx : 从 root 到叶子的内部页路径，以及每层选中的 ref 下标
 *   rows      : 当前叶子「已解析可见性」的行（materialize_leaf + entry_read 的结果）
 *   row       : 当前行下标
 *
 * 真 WT 不缓存整叶行，而是 insert 链与磁盘 entries 双流归并游走；
 * 这里每进入一个叶子物化一次（正确优先），P3.3 的性能优化留到后面。
 */
struct CursorState {
    std::vector<Page *> pages;
    std::vector<size_t> idx;
    Ref *leaf = nullptr;
    std::vector<std::pair<Key, Val>> rows;
    size_t row = 0;
    bool valid = false;
};

/*
 * HistoryReader —— P4/T4.3「读取下探」的钩子
 *
 * 为什么是钩子而不是直接引用 HistoryStore：
 *   twt_hs.h 需要 Btree（extract_old_versions 要遍历树），如果 Btree 反过来
 *   也要 HistoryStore，就是循环 include。真 WT 里没有这个问题 —— HS 是
 *   一棵普通的 B-Tree，读路径通过「再开一个 cursor」访问它，天然解耦。
 *
 * 语义（get 的实现必须照这个来）：
 *   1. 内存链上有对 read_ts 可见的版本 → 直接返回，**不调用**本接口
 *   2. 链上没有，且镜像里**没有这一行**（insert-only 键，base 为空）→ 调 read()
 *        S_OK + found=true   → 用 out（可能是 tombstone，此时调用方返回 NOTFOUND）
 *        S_OK + found=false  → 这个键在 read_ts 上不存在 → NOTFOUND
 *   3. 链上没有、但镜像里有 base → 回落 base
 *
 * ★ 第 3 条是本版的简化，也是它唯一的语义缺口：
 *   本版的 base 不携带 start_ts（真 WT 存在 cell 的 second_desc 里）。
 *   所以「reconcile 之后 base 比读者新」这种情况判不出来，只能返回 base。
 *   要修这个缺口 = 把时间窗口持久化进 cell，见可选池 O5。
 *   跨 checkpoint 的旧快照读因此只保证到「HS 里已有的版本」，不保证到 base。
 *
 * 不调用它的后果：insert-only 键的老版本一旦进 HS，旧快照就读不到了 ——
 * 静默返回 NOTFOUND，不报错。
 */
class HistoryReader {
public:
    virtual ~HistoryReader() = default;
    virtual Status read(const Key &key, u64 read_ts, Val &out, bool &found) = 0;
};

class Btree {
public:
    Btree() = default;
    ~Btree();

    Btree(const Btree &) = delete;
    Btree &operator=(const Btree &) = delete;

    Status open(const std::string &path, const Config &cfg = Config{});
    Status close();

    Status put(Slice key, Slice val, u64 txnid = 1, u64 ts = 1);

    /*
     * get --
     *     沿 descend 找到键，然后按「update 链 → HistoryStore → base」回落：
     *       1. 链上找第一个对 (reader_txnid, read_ts) 可见的版本
     *       2. 链上没有，且镜像里没有这一行 → 问 history_reader_（T4.3）
     *       3. 镜像里有 base → 回落 base（本版简化，见 HistoryReader 的注释）
     *
     *     ★ 第 2 步不能省。insert-only 键的老版本进 HS 之后，跳过它就会
     *       让旧快照读到「键不存在」（静默错误，不报错）。
     */
    Status get(Slice key, Val &out, u64 reader_txnid = UINT64_MAX, u64 read_ts = UINT64_MAX);
    Status remove(Slice key, u64 txnid = 1, u64 ts = 1);

    /* --- P4/T4.3：挂上历史版本的读取通道（nullptr = 不下探） --- */
    void set_history_reader(HistoryReader *r) { history_reader_ = r; }
    HistoryReader *history_reader() const { return history_reader_; }

    /* checkpoint：把所有脏页 reconcile 落盘，然后持久化超级块 */
    Status checkpoint();

    /* 调试 / 测试用 */
    const Stats &stats() const { return stats_; }
    size_t cached_pages() const { return cached_; }
    u64 root_page_id() const { return root_ref_ ? root_ref_->page_id : 0; }

    /*
     * --- P4/T4.2 观察点：根页当前所处状态 ---
     *
     * 契约（T4.2 要求真的维护这个状态机，测试按它断言）：
     *   open() 只根据超级块建立 root_ref_（page == nullptr, state == REF_DISK），
     *           **不**读页；第一次 descend 才 page_in → READING → MEM。
     *   eviction 抢到页时 MEM → LOCKED，淘汰完成后回到 DISK。
     *
     * 与真 WT 的差别：真 WT 的 WT_REF 在父页里，这里根 Ref 是 Btree 的成员，
     * 但状态迁移完全同构。
     */
    RefState root_ref_state() const { return root_ref_ ? root_ref_->state : REF_DISK; }
    void dump_tree(std::string &out) const;
    /* 校验：页内 key 有序、父页分隔符正确、addr 都能读回来 */
    Status verify(std::string &err);

    /*
     * 全树顺序扫描（内部 helper，不是公开 API）：
     *   - 元数据表靠它枚举（WiredTiger.wt 就是一张表）
     *   - P3-T3.3 的 cursor next/prev 会在它基础上做成状态机
     * 回调按 key 升序收到「对最新快照可见」的 (key, value)。
     */
    Status scan_all(const std::function<void(const Key &, const Val &)> &fn);

    /* --- 游标遍历（T3.3；供 C ABI 的 next/prev 使用） --- */
    Status cursor_first(CursorState &st);
    Status cursor_last(CursorState &st);
    Status cursor_next(CursorState &st);
    Status cursor_prev(CursorState &st);
    /*
     * 定位到「第一个 >= key 的位置」（对标 __wt_row_search）：
     * 停在 st.row 上；若 st.row == st.rows.size() 表示本叶没有 >= key 的行，
     * 下一次 cursor_next 会走到下一个兄弟叶。空树返回 S_NOTFOUND。
     */
    Status cursor_seek(CursorState &st, const Key &key);
    void cursor_release(CursorState &st);
    /* 当前行（仅在 cursor_* 返回 S_OK 后有效） */
    const std::pair<Key, Val> *cursor_current(const CursorState &st) const;

    /* --- P2/T2.6 overflow page（超长 key/value 的落盘位置） --- */
    Status write_overflow(Slice data, Addr &out);
    Status read_overflow(const Addr &a, Val &out);

    /* --- P4/T4.3 + T4.4：版本回收与回滚 --- */

    /*
     * extract_old_versions --
     *     遍历全树，把 start_ts < oldest_ts 且「所有活跃读者都看不到」的
     *     Update 从链上摘下来，交给回调（History Store 会存起来），
     *     并尽量回落到更旧的版本或 base。
     *
     *     ★ 用回调而不是直接依赖 HistoryStore，是为了让 Btree 不认识 HS，
     *       避免 twt_btree.h 与 twt_hs.h 互相 include。
     *     moved 返回实际搬走的条数。
     */
    Status extract_old_versions(
      u64 oldest_ts, const std::function<void(const Key &, const Update &)> &fn, u64 &moved);

    /*
     * rollback_versions_after --
     *     RTS：把所有 start_ts > stable_ts 的 Update 摘掉（未提交的、太新的），
     *     链摘空后回落到 base；如果键本来只存在于 insert 跳表里，整条删掉。
     *     removed 返回摘掉的条数。
     */
    Status rollback_versions_after(u64 stable_ts, u64 &removed);

    /*
     * --- P4/T4.1 并发 L1 ---
     * tree 级读写锁：读（get/scan/cursor）走 shared，写（put/remove/checkpoint/
     * eviction）走 unique。这是「先能并发」的最低配，hazard pointer 是 T4.2。
     *
     * ★ 不要把这个锁加进 split / evict 的内部路径：递归加锁会自锁。
     *   正确做法是「公开入口加锁，内部实现假定已持有」。
     */
    std::shared_mutex &latch() { return latch_; }

private:
    /* --- page cache（对标 src/cache + src/btree/bt_read.c） --- */
    Status page_in(Ref &ref, Page *&out);
    Page *instantiate(const std::vector<u8> &img, u64 page_id);
    void lru_touch(Page *p);
    void lru_remove(Page *p);
    void lru_push_front(Page *p);
    void evict_if_needed();
    void evict_one();

    /* --- 树操作 ---
     * path   : 从 root 到目标叶子页经过的 Ref（split 时用来定位父页）
     * pinned : 路径上的内存页，操作结束前不允许被 evict
     */
    Status descend(Slice key, std::vector<Ref *> &path, std::vector<Page *> &pinned, Page *&leaf);
    Status scan_rec(Ref *ref, const std::function<void(const Key &, const Val &)> &fn);

    /* --- 游标内部 --- */
    Status leaf_load(CursorState &st);                     /* 物化 st.leaf 的可见行 */
    Status descend_left(CursorState &st, Ref *start, size_t depth);
    Status descend_right(CursorState &st, Ref *start, size_t depth);
    Status descend_seek(CursorState &st, const Key &key, Ref *start, size_t depth);
    void unpin_deeper(CursorState &st, size_t keep_levels);
    Status split_leaf(Page *leaf, std::vector<Ref *> &path);
    Status split_intl(Page *parent, std::vector<Ref *> &path);
    Ref *new_ref(const Key &key, u64 page_id);

    /* --- reconcile（对标 src/reconcile/rec_write.c） --- */
    Status reconcile_page(Page *page, Addr &out);
    Status reconcile_tree(Ref *ref); /* 后序遍历：先子后父 */

    Config cfg_;
    BlockManager bm_;
    Stats stats_;

    std::unique_ptr<Ref> root_ref_;
    std::unordered_map<u64, std::unique_ptr<Page>> owned_; /* page_id → Page */
    u64 next_page_id_ = 1;

    Page *lru_head_ = nullptr;
    Page *lru_tail_ = nullptr;
    size_t cached_ = 0;

    mutable std::shared_mutex latch_; /* P4/T4.1 */

    HistoryReader *history_reader_ = nullptr; /* P4/T4.3：非拥有指针 */
};

} /* namespace twt */
