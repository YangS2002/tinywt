/*-
 * include/twt_txn.h —— P4 作业：事务 / 并发 / History Store / RTS / WAL
 *
 * 对标真 WT:
 *   src/include/txn.h                 WT_TXN
 *   src/txn/txn.c                     __wt_txn_begin / commit / rollback
 *   src/include/txn_inline.h          __wt_txn_visible / __wt_txn_visible_all
 *   src/history/hs.c                  __wt_hs_insert_updates / __wt_hs_cursor
 *   src/txn/txn_rollback_to_stable.c  __wt_rollback_to_stable
 *   src/log/                          日志记录 + 恢复
 *
 * ---------------------------------------------------------------------------
 * ★ 为什么需要这一层：P2 的可见性规则是「txnid <= 读者 且 ts <= 读时间戳」，
 *   它只能回答「这个版本旧不旧」，回答不了三个真问题：
 *
 *   1. 快照隔离：事务 A 在 T1 开始，事务 B 在 T2 提交 —— A 必须看不到 B 的写，
 *      哪怕 B 的 txnid 比 A 小。所以要记「事务开始时的活跃集合」，不是比大小。
 *
 *   2. 回滚：事务改了一半要撤销。因为修改是挂在 update 链上的，
 *      必须记住「本事务挂了哪些 Update」才能精确摘链（不能整条链丢掉，
 *      别人的版本还在里面）。
 *
 *   3. 版本回收：一条 update 链不能无限长。要有一个「所有活跃读者都看不到」
 *      的判据（visible_all），才能把老版本挪进 History Store 或者丢掉。
 *
 * ---------------------------------------------------------------------------
 * 本版的简化（要如实标注）：
 *   - 单进程多线程，没有 prepare / 两阶段提交
 *   - 冲突策略是「先写者赢 + 后者 WT_ROLLBACK」，不做 WT 的写冲突队列
 *   - History Store 是内存里的 map（真 WT 是一棵独立的 B-Tree 文件）
 *   - WAL 是「表级 redo log」，没有 checkpoint LSN 管理
 * -*/
#pragma once

#include <map>
#include <string>
#include <vector>

#include "twt_btree.h"
#include "twt_defs.h"

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
 *
 * ★ snapshot 为什么不能省：
 *   只比 txnid 大小的话，「A 先开始、B 后开始但先提交」这种交错就会读错。
 *   WT 的做法是记下 begin 时刻的 snap_min / snap_max + 活跃集合，
 *   可见性 = 「txnid < snap_min」或「txnid 不在活跃集合里且已提交」。
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
 *
 *   分配 id、维护活跃集合、给 stable timestamp（RTS 的锚点）、
 *   以及最关键的 visible()。
 */
class TxnManager {
public:
    /*
     * begin --
     *     分配 id，固化快照（snap_min / snap_max / snapshot），
     *     read_ts 默认取当前 stable_ts（或调用方后来 set）。
     *     ★ 快照必须在 begin 时刻一次拍定；之后别人提交不影响这个事务看到的东西。
     */
    Status begin(Txn &t);
    Status begin(Txn &t, u64 read_ts);

    /*
     * commit --
     *     校验状态 → 标 COMMITTED → commit_ts = 参数 → 从活跃集合摘掉。
     *     注意：真 WT 在 commit 里还要做「冲突检测」，本版把冲突放在 put 时判。
     */
    Status commit(Txn &t, u64 commit_ts);

    /* rollback：标 ABORTED，清空 writes（真正摘链由 TxnSession 做） */
    Status rollback(Txn &t);

    /*
     * visible --
     *     判断某个 Update 对读者可见。规则（按顺序）：
     *       1. 读自己的写：u->txnid == reader.id → true
     *       2. 未提交（txnid 仍在活跃集合）→ false
     *       3. u->txnid >= reader.snap_max → false（begin 之后才开的都不可见）
     *       4. u->txnid 在 reader.snapshot 里 → false（begin 时还活着，可能没提交）
     *       5. 时间戳：u->start_ts > reader.read_ts → false
     *       6. 否则 true
     */
    bool visible(const Update *u, const Txn &reader) const;

    /*
     * visible_all --
     *     这个版本是不是「所有活跃读者都看不到 / 都已经能看到」？
     *     用来判断能否回收：低于 oldest_active() 且已提交的版本可以挪走。
     */
    bool visible_all(const Update *u) const;

    u64 oldest_active() const;
    u64 next_id() const { return next_id_; }
    u64 stable_ts() const { return stable_ts_; }
    void set_stable_ts(u64 ts) { stable_ts_ = ts; }

private:
    u64 next_id_ = 1;
    u64 stable_ts_ = 0;
    std::vector<Txn *> active_; /* 活跃事务（按 id 升序） */
};

/*
 * TxnSession --
 *     把 Txn 和一棵 Btree 绑在一起，提供事务内的读写。
 *     对标 WT_SESSION：所有数据操作都发生在某个事务上下文里。
 *
 *   commit   : 把本事务的 Update 标成已提交（可见性靠 TxnManager 的集合判断）
 *   rollback : 逆序把 writes 里的 Update 从 update 链上摘掉并释放
 *
 * ★ 回滚的实现要点：摘链时不能「把链头置空」，因为链上可能混着别的事务的版本；
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

    /*
     * insert --
     *     语义上的「插入」：键已存在则 WT_DUPLICATE_KEY。
     *     与 put（覆盖）区分开，这是 SQL 层 INSERT / UPSERT 的分界。
     */
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

/*
 * HistoryStore --
 *     被挤出 update 链的老版本存这里，读的时候按时间戳回查。
 *     对标真 WT 的 History Store（一棵按 (key, ts) 排序的独立 B-Tree）。
 *
 * ★ 为什么必须有 HS：
 *   内存里一条链不能无限长；但「删掉老版本」会让一个还在跑的旧快照读到错的值。
 *   所以「链上只留最近的，老的进 HS」是可回收性的唯一出路。
 */
class HistoryStore {
public:
    /* 插入一个历史版本（同一 key 按 start_ts 降序保存） */
    Status insert(const Key &key, const Update &u);

    /* 按时间戳回查：找 <= read_ts 的最新版本 */
    Status read(const Key &key, u64 read_ts, Val &out, bool &found);

    /* 清掉 ts < oldest_ts 的版本（对应 __wt_hs_delete_key） */
    Status evict_older_than(u64 oldest_ts);

    size_t size() const { return entries_.size(); }
    size_t versions(const Key &key) const;

private:
    std::map<Key, std::vector<Update>> entries_;
};

/*
 * rollback_to_stable --
 *     把所有 start_ts > stable_ts 的修改回滚掉（未提交的、或者时间戳太新的），
 *     并把已提交但时间戳过新的版本一起清掉。
 *     对标 __wt_rollback_to_stable()。
 *
 *   为什么需要它：稳定点之前的数据是「所有副本都能看到」的公共历史；
 *   恢复时只能保证到稳定点，之后的东西必须一致地丢弃，否则会出现
 *   「A 节点有、B 节点没有」的分裂。
 */
Status rollback_to_stable(Btree &bt, u64 stable_ts);

/* -------------------------------------------------------------------------
 * WAL（Write-Ahead Log）
 *
 * 规则：先写日志、再改内存、最后（checkpoint 时）写数据页。
 *      所以崩溃后只要日志还在，就能把「已提交但没落盘」的修改重放出来。
 *
 * 日志记录（每条）：
 *   [u8 type][varint key_len][key][varint val_len][val]
 *   type: LOG_PUT / LOG_REMOVE / LOG_COMMIT
 * ------------------------------------------------------------------------- */
enum LogType : u8 {
    LOG_PUT = 1,
    LOG_REMOVE = 2,
    LOG_COMMIT = 3,
};

class LogManager {
public:
    Status open(const std::string &path);
    Status close();

    /* 追加一条记录并 fsync（真 WT 默认不每条 fsync，这里为了测试确定性选择同步） */
    Status append(LogType type, const Key &key, const Val *val);
    Status append_commit(u64 txnid);

    /*
     * replay --
     *     从头扫日志，把 PUT / REMOVE 重放到 bt 上。
     *     校验和/截断的记录直接停止（日志尾部可能被崩溃截断，这是正常的）。
     */
    Status replay(Btree &bt);

    /* checkpoint 之后调用：日志内容已经落进数据文件，可以清空 */
    Status truncate();

    u64 records() const { return records_; }

private:
    std::string path_;
    u64 records_ = 0;
    bool open_ = false;
};

} /* namespace twt */
