/*-
 * include/tinywt/wal/log.h —— P3/T3.5 作业：WAL 与崩溃恢复
 *
 * 对标真 WT:
 *   src/include/log.h        WT_LOG_RECORD / WT_LSN
 *   src/log/log.c            __wt_log_write
 *   src/log/log_recover.c    __wt_log_recover
 *
 * ---------------------------------------------------------------------------
 * 规则只有一句：**先写日志，再改内存，最后（checkpoint）写数据页。**
 * 反过来就会丢：内存改了、日志没写、进程崩了 → 无法重做。
 *
 * 记录布局（本作业，对标 config 头里的真实布局，砍掉对齐与加密）：
 *
 *   [u32 len][u32 checksum][u8 flags][payload]
 *     len      : 整条记录长度（含头）
 *     checksum : crc32(payload)
 *     flags    : 0（真实 WT 里是 COMPRESSED 0x01 / ENCRYPTED 0x02）
 *   payload  : [u8 type][varint txnid][varint key_len][key]
 *              [varint val_len][val]（REMOVE 时 val_len = 0）
 *
 * 真 WT 还有：LOG_ALIGN=128 对齐（log.h:51）、LSN=(file<<32)|offset（:60）、
 * 首块 WT_LOG_DESC{magic,version,allocsize}。这些是 Capstone 的可选加分项。
 * ---------------------------------------------------------------------------
 * 恢复语义（三条，缺一条都会「越崩溃越起不来」）：
 *   1. 扫描遇到「长度超过剩余字节」或「checksum 不符」→ 停在那里，**不报错**
 *      （崩溃时最后一条写了一半是正常现象）
 *   2. 只重放**有 COMMIT 记录**的事务（没提交的必须丢弃，否则就是脏读）
 *   3. 重放完立刻做一次 checkpoint，然后才能对外服务
 * -*/
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "tinywt/btree/btree.h"
#include "tinywt/common/defs.h"

namespace twt {

enum LogType : u8 {
    LOG_TXN_BEGIN = 1,
    LOG_ROW_PUT = 2,
    LOG_ROW_REMOVE = 3,
    LOG_TXN_COMMIT = 4,
};

constexpr u32 LOG_ALIGN = 128;
constexpr u32 LOG_REC_HEADER = 9; /* len + checksum + flags */

using Lsn = u64; /* (file << 32) | offset */

struct LogRecord {
    LogType type = LOG_TXN_BEGIN;
    u64 txnid = 0;
    Key key;
    Val val;
};

class LogManager {
public:
    Status open(const std::string &path);
    Status close();

    /* --- 写路径 --- */
    Status txn_begin(u64 txnid);
    Status put(u64 txnid, Slice key, Slice val);
    Status remove(u64 txnid, Slice key);
    Status txn_commit(u64 txnid);

    /*
     * replay --
     *     扫日志 → 收集每个事务的记录 → 只把「有 COMMIT」的事务按顺序应用到 bt。
     *
     *     返回 S_OK：即使尾部有半截记录（那是正常的崩溃残留）。
     *     返回 S_CORRUPT：只有记录**中间**出现损坏才算（说明不是尾部截断）。
     */
    Status replay(Btree &bt);

    /*
     * truncate_upto --
     *     checkpoint 之后调用：丢掉「结束位置 <= lsn」的记录（它们已经落进
     *     数据页了，留着只会让恢复变慢）。真 WT 对应「日志只保留 >= ckpt_lsn」。
     *
     *     ★ 边界是**闭区间**：lsn 取 last_lsn()（= 某条记录写完之后的偏移），
     *       则那条记录及其之前的全部被丢弃。测试按这个语义断言条数。
     *     ★ 必须重写文件（或从下一个记录边界起算覆盖写），不能只改内存计数。
     */
    Status truncate_upto(Lsn lsn);

    Lsn last_lsn() const { return last_lsn_; }
    u64 record_count() const { return records_; }

private:
    Status append(LogType type, u64 txnid, Slice key, const Val *val);

    std::string path_;
    bool open_ = false;
    Lsn last_lsn_ = 0;
    u64 records_ = 0;
    u64 offset_ = 0;
};

} /* namespace twt */
