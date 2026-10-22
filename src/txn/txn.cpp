/*-
 * src/txn/txn.cpp —— P3/T3.4 作业：事务状态机与回滚
 *
 * 对标真 WT:
 *   src/txn/txn.c              __wt_txn_begin / __wt_txn_commit / __wt_txn_rollback
 *   src/include/txn_inline.h   __wt_txn_visible
 *
 * ★ 建议顺序：先把 visible() 写对，再写 rollback。
 *   visible() 错了，所有测试都会以「值莫名其妙不对」的形式失败，很难定位。
 * -*/
#include "tinywt/txn/txn.h"

#include <algorithm>

namespace twt {

/* -------------------------------------------------------------------------
 * TxnManager
 * ------------------------------------------------------------------------- */

Status
TxnManager::begin(Txn &t)
{
    UNIMPLEMENTED("P3/T3.4 TxnManager::begin");
}

Status
TxnManager::begin(Txn &t, u64 read_ts)
{
    UNIMPLEMENTED("P3/T3.4 TxnManager::begin(read_ts)");
}

Status
TxnManager::commit(Txn &t, u64 commit_ts)
{
    UNIMPLEMENTED("P3/T3.4 TxnManager::commit");
}

Status
TxnManager::rollback(Txn &t)
{
    UNIMPLEMENTED("P3/T3.4 TxnManager::rollback");
}

/*
 * visible --
 *     规则见 tinywt/txn/txn.h。最容易写错的两点：
 *   - 「读自己的写」必须在最前面判：自己的 txnid 在 active_ 里，
 *     不特判就会被判成不可见，事务内读不到自己刚写的值。
 *   - snapshot 判的是「begin 那一刻别人是否还活着」，不是「现在是否还活着」。
 */
bool
TxnManager::visible(const Update *u, const Txn &reader) const
{
    UNIMPLEMENTED("P3/T3.4 TxnManager::visible");
}

u64
TxnManager::oldest_active() const
{
    UNIMPLEMENTED("P3/T3.4 TxnManager::oldest_active");
}

/* -------------------------------------------------------------------------
 * TxnSession
 * ------------------------------------------------------------------------- */

Status
TxnSession::begin()
{
    UNIMPLEMENTED("P3/T3.4 TxnSession::begin");
}

Status
TxnSession::begin(u64 read_ts)
{
    UNIMPLEMENTED("P3/T3.4 TxnSession::begin(read_ts)");
}

Status
TxnSession::commit(u64 commit_ts)
{
    UNIMPLEMENTED("P3/T3.4 TxnSession::commit");
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
    UNIMPLEMENTED("P3/T3.4 TxnSession::rollback");
}

Status
TxnSession::put(Slice key, Slice val)
{
    UNIMPLEMENTED("P3/T3.4 TxnSession::put");
}

Status
TxnSession::get(Slice key, Val &out)
{
    UNIMPLEMENTED("P3/T3.4 TxnSession::get");
}

Status
TxnSession::remove(Slice key)
{
    UNIMPLEMENTED("P3/T3.4 TxnSession::remove");
}

Status
TxnSession::insert(Slice key, Slice val)
{
    UNIMPLEMENTED("P3/T3.4 TxnSession::insert");
}

/*
 * check_conflict --
 *     写写冲突：链头如果是一个「别的事务写下的、还没提交」的版本 → S_ROLLBACK。
 *     真 WT 会把这个事务挂到冲突队列上等对方结束；本作业直接让后来者重试。
 */
Status
TxnSession::check_conflict(Slice key, Update *chain_head) const
{
    UNIMPLEMENTED("P3/T3.4 TxnSession::check_conflict");
}

} /* namespace twt */
