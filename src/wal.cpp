/*-
 * src/wal.cpp —— P3/T3.5 作业：WAL 与崩溃恢复
 *
 * 对标真 WT: src/log/log.c（__wt_log_write）+ src/log/log_recover.c
 *
 * 记录布局与三条恢复语义都在 twt_log.h 顶部，写代码前先读一遍。
 *
 * ★ 本作业最容易写错的三处：
 *   1. 写记录时不 fsync —— 崩溃后日志自己也没了，"先写日志"就白做了
 *   2. 重放时把「没有 COMMIT 的事务」也应用了 —— 脏读进了数据文件
 *   3. 把「尾部半截记录」当错误返回 —— 崩溃现场就是这样，越修越起不来
 * -*/
#include "twt_log.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace twt {

Status
LogManager::open(const std::string &path)
{
    UNIMPLEMENTED("P3/T3.5 LogManager::open");
}

Status
LogManager::close()
{
    UNIMPLEMENTED("P3/T3.5 LogManager::close");
}

/*
 * append --
 *     组一条记录写到文件尾并 flush 到磁盘：
 *       [u32 len][u32 crc32(payload)][u8 flags][payload]
 *     payload = [type][varint txnid][varint klen][key][varint vlen][val]
 *
 *     每写一条：records_++、offset_ += len、last_lsn_ = offset_。
 *
 *     ★ 真 WT 用 log slot 做组提交（O10 可选池）；本作业一条一 fsync，
 *       慢但确定性强，测试才能不靠 sleep。
 */
Status
LogManager::append(LogType type, u64 txnid, Slice key, const Val *val)
{
    (void)type;
    (void)txnid;
    (void)key;
    (void)val;
    UNIMPLEMENTED("P3/T3.5 LogManager::append");
}

Status
LogManager::txn_begin(u64 txnid)
{
    UNIMPLEMENTED("P3/T3.5 LogManager::txn_begin");
}

Status
LogManager::put(u64 txnid, Slice key, Slice val)
{
    UNIMPLEMENTED("P3/T3.5 LogManager::put");
}

Status
LogManager::remove(u64 txnid, Slice key)
{
    UNIMPLEMENTED("P3/T3.5 LogManager::remove");
}

Status
LogManager::txn_commit(u64 txnid)
{
    UNIMPLEMENTED("P3/T3.5 LogManager::txn_commit");
}

/*
 * replay --
 *     两遍扫描（或者一遍收集 + 一遍应用）：
 *       第一遍：解析记录，按 txnid 分组，记下哪些 txnid 出现过 COMMIT
 *       第二遍：只把「已 COMMIT 的 txn」的 PUT/REMOVE 按出现顺序应用到 bt
 *
 *     遇到尾部半截（len 超过剩余字节）或 checksum 不符 → 停止扫描，返回 S_OK。
 *     如果损坏出现在中间（后面还有完整记录）→ 说明不是崩溃截断，返回 S_CORRUPT。
 *
 *     ★ 同一个 key 在一个事务里被改多次，要按日志顺序重放（后写覆盖先写）。
 */
Status
LogManager::replay(Btree &bt)
{
    UNIMPLEMENTED("P3/T3.5 LogManager::replay");
}

Status
LogManager::truncate_upto(Lsn lsn)
{
    (void)lsn;
    UNIMPLEMENTED("P3/T3.5 LogManager::truncate_upto");
}

} /* namespace twt */
