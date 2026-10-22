/*-
 * include/tinywt/txn/txn.h —— P3/T3.4 作业：事务 API
 *
 * 对标真 WT:
 *   src/include/txn.h                 WT_TXN
 *   src/txn/txn.c                     __wt_txn_begin / commit / rollback
 *   src/include/txn_inline.h          __wt_txn_visible
 *
 * ---------------------------------------------------------------------------
 * 这一层回答三个问题：
 *
 *   1. 事务内读得到自己刚写的吗？        → 读自己的写（visible 的第一条规则）
 *   2. 没提交的写别人看得见吗？          → 看不见（记录活跃事务集合）
 *   3. 回滚怎么撤销？                    → 精确摘链，不能整条链丢掉
 *
 * ★ 为什么不能靠「比 txnid 大小」：
 *     事务 A 先开始，事务 B 后开始但先提交。A 必须看不到 B 的写，
 *     但 B 的 id 可能比 A 小 —— 所以必须记 A 开始时「谁还活着」。
 *
 * P4 会把这里的朴素规则换成真正的 snapshot + watermark（见 tinywt/txn/history_store.h）。
 * -*/
#pragma once

#include <map>
#include <string>
#include <vector>

#include "tinywt/btree/btree.h"
#include "tinywt/common/defs.h"

namespace twt {

enum TxnState : u8 {
    TXN_NONE = 0,
    TXN_RUNNING = 1,
    TXN_COMMITTED = 2,
    TXN_ABORTED = 3,
};

/*
 * Txn --
 *     一个事务的全部状态。
 *
 *   id        : 单调递增的事务 ID（不是时间戳）
 *   read_ts   : 开始时的快照时间戳；读操作都用它
 *   commit_ts : 提交时间戳（commit 时给）
 *   snapshot  : 开始时仍活跃的事务 ID 集合 —— 快照隔离的核心
 *   writes    : 本事务挂到 update 链上的节点，回滚时按它精确摘链
 */
struct Txn {
    u64 id = 0;
    u64 read_ts = 0;
    u64 commit_ts = 0;
    u64 snap_min = 0;
    u64 snap_max = 0;
    std::vector<u64> snapshot; /* 开始时活跃的 txn id */
    TxnState state = TXN_NONE;

    /* 回滚簿记：(key, 挂上去的 Update*)，按挂链的逆序撤销 */
    std::vector<std::pair<Key, Update *>> writes;

    bool running() const { return state == TXN_RUNNING; }
};

/*
 * TxnManager --
 *     全局事务表。对标 WT 的 WT_TXN_GLOBAL（src/include/txn.h）。
 */
class TxnManager {
public:
    /*
     * begin --
     *     分配 id，固化快照（snap_min / snap_max / snapshot）。
     *     ★ 快照必须在 begin 时刻一次拍定；之后别人提交不影响这个事务看到的东西。
     */
    Status begin(Txn &t);
    Status begin(Txn &t, u64 read_ts);

    Status commit(Txn &t, u64 commit_ts);

    /* rollback：标 ABORTED，清空 writes（真正摘链由 TxnSession 做） */
    Status rollback(Txn &t);

    /*
     * visible --
     *     判断某个 Update 对读者可见。规则（按顺序，顺序不能换）：
     *       1. 读自己的写：u->txnid == reader.id → true
     *       2. 未提交（txnid 仍在活跃集合）→ false
     *       3. u->txnid >= reader.snap_max → false（begin 之后才开的都不可见）
     *       4. u->txnid 在 reader.snapshot 里 → false（begin 时还活着）
     *       5. 时间戳：u->start_ts > reader.read_ts → false
     *       6. 否则 true
     */
    bool visible(const Update *u, const Txn &reader) const;

    u64 oldest_active() const;
    u64 next_id() const { return next_id_; }

private:
    u64 next_id_ = 1;
    std::vector<Txn *> active_; /* 活跃事务（按 id 升序） */
};

/*
 * TxnSession --
 *     把 Txn 和一棵 Btree 绑在一起，提供事务内的读写。
 *     对标 WT_SESSION：所有数据操作都发生在某个事务上下文里。
 *
 *   commit   : 把本事务标成已提交（可见性靠 active_ 集合判断）
 *   rollback : 逆序把 writes 里的 Update 从 update 链上摘掉并释放
 *
 * ★ 回滚的要点：摘链时不能「把链头置空」，因为链上可能混着别的事务的版本；
 *   要按 (key, Update*) 精确找到那个节点的前驱，做单链表删除。
 */
class TxnSession {
public:
    TxnSession(Btree &bt, TxnManager &mgr) : bt_(bt), mgr_(mgr) {}

    Status begin();
    Status begin(u64 read_ts);
    Status commit(u64 commit_ts);
    Status rollback();

    Status put(Slice key, Slice val);
    Status get(Slice key, Val &out);
    Status remove(Slice key);

    /* 语义上的「插入」：键已存在则 S_DUPLICATE */
    Status insert(Slice key, Slice val);

    const Txn &txn() const { return txn_; }
    bool in_txn() const { return txn_.running(); }

    /* 冲突检测：写之前看有没有别的事务留下「未提交」的版本 */
    Status check_conflict(Slice key, Update *chain_head) const;

private:
    Btree &bt_;
    TxnManager &mgr_;
    Txn txn_;
};

} /* namespace twt */
