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
#include <string>
#include <unordered_map>
#include <vector>

#include "twt_block.h"
#include "twt_defs.h"
#include "twt_page.h"

namespace twt {

struct Config {
    size_t max_leaf_entries = 32; /* 叶子页 entries+inserts 超过这个数就分裂 */
    size_t max_intl_refs = 32;    /* 内部页 refs 上限 */
    size_t max_cached_pages = 8;  /* cache 里最多驻留多少页 → 触发 eviction */
    u32 alloc_size = 4096;
    bool verbose = false;
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

class Btree {
public:
    Btree() = default;
    ~Btree();

    Btree(const Btree &) = delete;
    Btree &operator=(const Btree &) = delete;

    Status open(const std::string &path, const Config &cfg = Config{});
    Status close();

    Status put(Slice key, Slice val, u64 txnid = 1, u64 ts = 1);
    Status get(Slice key, Val &out, u64 reader_txnid = UINT64_MAX, u64 read_ts = UINT64_MAX);
    Status remove(Slice key, u64 txnid = 1, u64 ts = 1);

    /* checkpoint：把所有脏页 reconcile 落盘，然后持久化超级块 */
    Status checkpoint();

    /* 调试 / 测试用 */
    const Stats &stats() const { return stats_; }
    size_t cached_pages() const { return cached_; }
    u64 root_page_id() const { return root_ref_ ? root_ref_->page_id : 0; }
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
};

} /* namespace twt */
