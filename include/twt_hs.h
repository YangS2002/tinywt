/*-
 * include/twt_hs.h —— P4/T4.3–T4.5 作业：History Store / RTS / watermark
 *
 * 对标真 WT:
 *   src/history/hs.c                  __wt_hs_insert_updates / __wt_hs_cursor
 *   src/txn/txn_rollback_to_stable.c  __wt_rollback_to_stable
 *   src/include/txn.h                 WT_TXN_GLOBAL 的 stable / pinned timestamp
 *
 * ---------------------------------------------------------------------------
 * 三个概念的关系，先想清楚再写代码：
 *
 *   stable_ts  ──「所有副本都保证有」的时间点。RTS 从它回滚，日志从它截断。
 *   pinned_ts  ──「最老的活跃读者」。HS 只能回收 < pinned 的版本。
 *   oldest_ts  ── 两者取小，是真正的回收水位。
 *
 *        oldest = min(stable, pinned)
 *        ts < oldest 的旧版本 → 可以搬出 update 链（进 HS 或直接丢）
 *        ts > stable 的版本   → 崩溃恢复时必须回滚（RTS）
 *
 * ★ 为什么 update 链不能无限长：
 *     每改一行就在链头挂一个版本。长跑之后一条链可能有几百个版本，
 *     读要逐个判可见性，内存也扛不住。但「直接删老版本」会让旧快照读错，
 *     所以必须有一个安全水位 —— 这就是上面那个 min。
 * -*/
#pragma once

#include <map>
#include <string>
#include <vector>

#include "twt_btree.h"
#include "twt_defs.h"

namespace twt {

/*
 * TimeWindow --
 *     WT_TIME_AGGREGATE 的最小版（对标 src/include/time_window.h）。
 *     真 WT 把 start/stop/durable 三个时间戳按位打包进 8 字节（T4.5 可选加分）。
 */
struct TimeWindow {
    u64 start_ts = 0;
    u64 stop_ts = 0;
    u64 durable_ts = 0;
    bool prepared = false;
};

/*
 * Watermark --
 *     全局水位。T4.5 要求它在「有长事务挂着」时停下来，而不是盲目推进。
 */
struct Watermark {
    u64 stable_ts = 0;
    u64 pinned_ts = 0; /* 最老活跃读者 */
    u64 oldest() const { return stable_ts < pinned_ts ? stable_ts : pinned_ts; }
};

/*
 * HistoryStore --
 *     被挤出 update 链的老版本存这里，读的时候按时间戳回查。
 *
 *     真 WT：一棵独立的 B-Tree 文件（WiredTigerHS.wt），
 *            key = (btree_id, user_key, start_ts 降序)，value = 版本或 tombstone。
 *     本作业：内存 map，但**接口语义与真 WT 一致**（含 btree_id 维度）。
 */
class HistoryStore {
public:
    /* 插入一个历史版本；同一个 (btree_id, key) 内按 start_ts 降序保存 */
    Status insert(u64 btree_id, const Key &key, const TimeWindow &tw, const Val &val, bool tombstone);

    /*
     * read --
     *     找 start_ts <= read_ts 的最新版本。
     *     found=false 表示 HS 里没有更老的版本（调用方回落到 base）。
     */
    Status read(u64 btree_id, const Key &key, u64 read_ts, Val &out, bool &found,
      TimeWindow *tw = nullptr);

    /* 清掉 ts < oldest_ts 的版本（对应 __wt_hs_delete_key） */
    Status evict_older_than(u64 oldest_ts);

    size_t versions(u64 btree_id, const Key &key) const;
    size_t size() const { return entries_.size(); }

private:
    std::map<std::pair<u64, Key>, std::vector<std::pair<TimeWindow, std::pair<Val, bool>>>> entries_;
};

/*
 * reconcile_with_history --
 *     写入侧：把 Btree 里「所有活跃读者都看不到」的旧版本搬进 HS。
 *     对标 __wt_hs_insert_updates()，也是修掉「reconcile 直接丢弃旧版本」的补丁。
 */
Status reconcile_with_history(Btree &bt, HistoryStore &hs, const Watermark &wm);

/*
 * rollback_to_stable --
 *     把所有 start_ts > stable_ts 的修改回滚掉，并把 HS 里更老的版本一起清掉。
 *     对标 __wt_rollback_to_stable()。
 *
 *   为什么必须从 stable 点回滚：稳定点之前的数据是「所有副本都能看到」的
 *   公共历史；之后的东西在恢复时无法保证一致，只能一致地丢弃。
 */
Status rollback_to_stable(Btree &bt, HistoryStore &hs, u64 stable_ts);

/*
 * recover_after_crash --
 *     open 时若超级块带 SBF_DIRTY_SHUTDOWN 标志 → 说明上次没干净关闭，
 *     先做 RTS 再对外服务。返回 S_OK（无论有没有脏标志）。
 */
Status recover_after_crash(Btree &bt, HistoryStore &hs, u64 stable_ts);

/*
 * aggregate_stop_ts --
 *     T4.5：把一页里所有 update 的时间窗口聚合成一个「页级窗口」，
 *     让 reconcile / RTS 不用逐行判。
 *     对标 __wt_time_aggregate_merge()。
 */
Status aggregate_stop_ts(Page *page, TimeWindow &agg);

} /* namespace twt */
