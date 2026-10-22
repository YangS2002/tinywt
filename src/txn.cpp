/*-
 * src/txn.cpp —— P4 作业：事务 / 冲突 / History Store / RTS / WAL
 *
 * 对标真 WT:
 *   src/txn/txn.c                     __wt_txn_begin / __wt_txn_commit / __wt_txn_rollback
 *   src/include/txn_inline.h          __wt_txn_visible
 *   src/history/hs.c                  __wt_hs_insert_updates
 *   src/txn/txn_rollback_to_stable.c  __wt_rollback_to_stable
 *   src/log/log.c                     __wt_log_write / __wt_log_recover
 *
 * ★ 建议顺序：先把 visible() 写对（T4.1），再写 rollback（T4.2），
 *   然后 HS（T4.3）、RTS（T4.4），最后 WAL（T4.5）。
 *   visible() 错了，后面全部测试会以「莫名其妙的值不对」的形式失败。
 * -*/
#include "twt_txn.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace twt {

/* -------------------------------------------------------------------------
 * TxnManager
 * ------------------------------------------------------------------------- */

Status
TxnManager::begin(Txn &t)
{
    UNIMPLEMENTED("P4/T4.1 TxnManager::begin");
}

Status
TxnManager::begin(Txn &t, u64 read_ts)
{
    UNIMPLEMENTED("P4/T4.1 TxnManager::begin(read_ts)");
}

Status
TxnManager::commit(Txn &t, u64 commit_ts)
{
    UNIMPLEMENTED("P4/T4.1 TxnManager::commit");
}

Status
TxnManager::rollback(Txn &t)
{
    UNIMPLEMENTED("P4/T4.1 TxnManager::rollback");
}

/*
 * visible --
 *     规则见 twt_txn.h。这里再强调两个最容易写错的点：
 *   - 「读自己的写」必须在最前面判：自己的 txnid 在 active_ 里，
 *     不特判就会被规则 2 判成不可见，事务内读不到自己刚写的值。
 *   - snapshot 判的是「begin 那一刻别人是否还活着」，不是「现在是否还活着」。
 */
bool
TxnManager::visible(const Update *u, const Txn &reader) const
{
    UNIMPLEMENTED("P4/T4.1 TxnManager::visible");
}

bool
TxnManager::visible_all(const Update *u) const
{
    UNIMPLEMENTED("P4/T4.1 TxnManager::visible_all");
}

u64
TxnManager::oldest_active() const
{
    UNIMPLEMENTED("P4/T4.1 TxnManager::oldest_active");
}

/* -------------------------------------------------------------------------
 * TxnSession
 * ------------------------------------------------------------------------- */

Status
TxnSession::begin()
{
    UNIMPLEMENTED("P4/T4.1 TxnSession::begin");
}

Status
TxnSession::begin(u64 read_ts)
{
    UNIMPLEMENTED("P4/T4.1 TxnSession::begin(read_ts)");
}

/*
 * commit --
 *     提交前必须把 txn_.state 变成 COMMITTED，否则别人 visible() 永远看不到
 *     你写的版本（活跃集合里还留着你）。
 */
Status
TxnSession::commit(u64 commit_ts)
{
    UNIMPLEMENTED("P4/T4.1 TxnSession::commit");
}

/*
 * rollback --
 *     逆序遍历 txn_.writes：
 *       1. descend 到该 key 所在叶子；
 *       2. 如果是 entries 里的键，在 upd 链上找这个 Update 的前驱并摘掉；
 *          如果是 insert 跳表里的键，同样在节点链上摘；
 *       3. 链摘空之后，如果这个键本来就不在磁盘镜像里（insert 节点），
 *          要把节点也删掉，并把 ninserts 减回去 —— 否则回滚后还能查到空壳。
 *     ★ 只摘自己挂的那个节点，不能整条链丢掉（链上可能有别人的版本）。
 */
Status
TxnSession::rollback()
{
    UNIMPLEMENTED("P4/T4.2 TxnSession::rollback");
}

Status
TxnSession::put(Slice key, Slice val)
{
    UNIMPLEMENTED("P4/T4.2 TxnSession::put");
}

Status
TxnSession::get(Slice key, Val &out)
{
    UNIMPLEMENTED("P4/T4.2 TxnSession::get");
}

Status
TxnSession::remove(Slice key)
{
    UNIMPLEMENTED("P4/T4.2 TxnSession::remove");
}

Status
TxnSession::insert(Slice key, Slice val)
{
    UNIMPLEMENTED("P4/T4.2 TxnSession::insert");
}

/*
 * check_conflict --
 *     写写冲突：链头如果是一个「别的事务写下的、还没提交」的版本 → S_ROLLBACK。
 *     真 WT 会把这个事务挂到冲突队列上等对方结束；本版直接让后来者重试。
 */
Status
TxnSession::check_conflict(Slice key, Update *chain_head) const
{
    UNIMPLEMENTED("P4/T4.2 TxnSession::check_conflict");
}

/* -------------------------------------------------------------------------
 * History Store
 * ------------------------------------------------------------------------- */

Status
HistoryStore::insert(const Key &key, const Update &u)
{
    UNIMPLEMENTED("P4/T4.3 HistoryStore::insert");
}

/*
 * read --
 *     在 key 的历史版本里找 start_ts <= read_ts 的最新一个。
 *     找不到 → found=false（调用方再决定要不要回落到 base）。
 */
Status
HistoryStore::read(const Key &key, u64 read_ts, Val &out, bool &found)
{
    UNIMPLEMENTED("P4/T4.3 HistoryStore::read");
}

Status
HistoryStore::evict_older_than(u64 oldest_ts)
{
    UNIMPLEMENTED("P4/T4.3 HistoryStore::evict_older_than");
}

size_t
HistoryStore::versions(const Key &key) const
{
    UNIMPLEMENTED("P4/T4.3 HistoryStore::versions");
}

/* -------------------------------------------------------------------------
 * RTS
 * ------------------------------------------------------------------------- */

Status
rollback_to_stable(Btree &bt, u64 stable_ts)
{
    UNIMPLEMENTED("P4/T4.4 rollback_to_stable");
}

/* -------------------------------------------------------------------------
 * WAL
 * ------------------------------------------------------------------------- */

Status
LogManager::open(const std::string &path)
{
    UNIMPLEMENTED("P4/T4.5 LogManager::open");
}

Status
LogManager::close()
{
    UNIMPLEMENTED("P4/T4.5 LogManager::close");
}

/*
 * append --
 *     [type][varint key_len][key][varint val_len][val]
 *     追加后 fsync。val 为 nullptr 时表示没有值（REMOVE）。
 */
Status
LogManager::append(LogType type, const Key &key, const Val *val)
{
    UNIMPLEMENTED("P4/T4.5 LogManager::append");
}

Status
LogManager::append_commit(u64 txnid)
{
    UNIMPLEMENTED("P4/T4.5 LogManager::append_commit");
}

/*
 * replay --
 *     顺序扫日志，PUT → bt.put，REMOVE → bt.remove。
 *     ★ 尾部不完整的记录要当作「崩溃时写了一半」，直接停止而不是报错。
 *       判断依据：解析出的长度超过剩余字节。
 */
Status
LogManager::replay(Btree &bt)
{
    UNIMPLEMENTED("P4/T4.5 LogManager::replay");
}

Status
LogManager::truncate()
{
    UNIMPLEMENTED("P4/T4.5 LogManager::truncate");
}

} /* namespace twt */
