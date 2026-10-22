/*-
 * src/txn/history_store.cpp —— P4/T4.3–T4.5 作业：History Store / RTS / watermark
 *
 * 对标真 WT:
 *   src/history/hs.c                  __wt_hs_insert_updates / __wt_hs_cursor
 *   src/txn/txn_rollback_to_stable.c  __wt_rollback_to_stable
 *   src/include/time_window.h         WT_TIME_AGGREGATE
 *
 * ★ 三个东西的顺序是有依赖的：
 *     watermark（T4.5）决定「哪些版本可以回收」
 *       → 回收动作把版本搬进 HS（T4.3）
 *       → 崩溃恢复时按 stable 点回滚（T4.4）
 *   先做 T4.5 的 Watermark 计算，再做 T4.3，最后才是 T4.4。
 * -*/
#include "tinywt/txn/history_store.h"

#include <algorithm>

namespace twt {

/* -------------------------------------------------------------------------
 * T4.3 History Store
 * ------------------------------------------------------------------------- */

Status
HistoryStore::insert(
  u64 btree_id, const Key &key, const TimeWindow &tw, const Val &val, bool tombstone)
{
    (void)btree_id;
    (void)key;
    (void)tw;
    (void)val;
    (void)tombstone;
    UNIMPLEMENTED("P4/T4.3 HistoryStore::insert");
}

/*
 * read --
 *     在 (btree_id, key) 的历史里找 start_ts <= read_ts 的最新一个。
 *     同一个 start_ts 可能有多个版本（同 ts 多次写），取最后写入的。
 *
 *     ★ tombstone 版本也要能被读到：它表示「这个时间点之后键被删了」，
 *       调用方拿到 tombstone 要返回 found=false，而不是继续往下找更老的版本。
 */
Status
HistoryStore::read(
  u64 btree_id, const Key &key, u64 read_ts, Val &out, bool &found, TimeWindow *tw)
{
    (void)btree_id;
    (void)key;
    (void)read_ts;
    (void)out;
    (void)found;
    (void)tw;
    UNIMPLEMENTED("P4/T4.3 HistoryStore::read");
}

Status
HistoryStore::evict_older_than(u64 oldest_ts)
{
    (void)oldest_ts;
    UNIMPLEMENTED("P4/T4.3 HistoryStore::evict_older_than");
}

size_t
HistoryStore::versions(u64 btree_id, const Key &key) const
{
    (void)btree_id;
    (void)key;
    UNIMPLEMENTED("P4/T4.3 HistoryStore::versions");
}

/*
 * reconcile_with_history --
 *     写入侧。用 Btree::extract_old_versions 把旧版本摘下来，逐条塞进 HS：
 *
 *       bt.extract_old_versions(wm.oldest(), [&](const Key &k, const Update &u) {
 *           hs.insert(btree_id, k, TimeWindow{u.start_ts, u.stop_ts, u.durable_ts}, u.data,
 *                     u.is_tombstone());
 *       }, moved);
 *
 *     ★ 能不能搬的判据不是「旧不旧」，而是「所有活跃读者都看不到」：
 *       用水位 wm.oldest() 而不是 wm.stable_ts。
 */
Status
reconcile_with_history(Btree &bt, HistoryStore &hs, const Watermark &wm)
{
    (void)bt;
    (void)hs;
    (void)wm;
    UNIMPLEMENTED("P4/T4.3 reconcile_with_history");
}

/* -------------------------------------------------------------------------
 * T4.4 RTS
 * ------------------------------------------------------------------------- */

Status
rollback_to_stable(Btree &bt, HistoryStore &hs, u64 stable_ts)
{
    (void)bt;
    (void)hs;
    (void)stable_ts;
    UNIMPLEMENTED("P4/T4.4 rollback_to_stable");
}

Status
recover_after_crash(Btree &bt, HistoryStore &hs, u64 stable_ts)
{
    (void)bt;
    (void)hs;
    (void)stable_ts;
    UNIMPLEMENTED("P4/T4.4 recover_after_crash");
}

/* -------------------------------------------------------------------------
 * T4.5 watermark
 * ------------------------------------------------------------------------- */

/*
 * aggregate_stop_ts --
 *     把页里所有 update 的时间窗口合成一个：
 *       agg.start_ts   = 最小 start_ts
 *       agg.stop_ts    = 最大 stop_ts
 *       agg.durable_ts = 最小 durable_ts
 *       agg.prepared   = 有任何一个 prepared
 *
 *     ★ 为什么用「最小 start / 最大 stop」：聚合窗口必须**覆盖**所有成员，
 *       否则用它做判断会漏掉某些版本（漏判 = 数据错，宁可保守）。
 *     ★ 空页：窗口留全 0 并返回 S_OK（调用方靠 entries 数为 0 判断空页）。
 */
Status
aggregate_stop_ts(Page *page, TimeWindow &agg)
{
    (void)page;
    (void)agg;
    UNIMPLEMENTED("P4/T4.5 aggregate_stop_ts");
}

} /* namespace twt */
