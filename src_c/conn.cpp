/*-
 * src_c/conn.cpp —— P3 作业：wiredtiger_open / connection / session
 *
 * 对标真 WT：src/conn/conn_api.c、conn_dhandle.c、src/session/session_api.c
 *
 * 这一层的意义：真 wtperf / wt 工具只认 compat/wiredtiger.h 里的 C 结构体，
 * 所以「能不能被真工具链接」= 「这些函数指针有没有被填对」。
 *
 * ★ ABI 关键点（已经替你写好，别改）：
 *   twt_conn / twt_session / twt_cursor 的 iface 都必须是**第一个成员**，
 *   这样 WT_CONNECTION* 与 twt_conn* 是同一个地址，强转安全。
 *   真 WT 也是这么干的（conn->iface / session->iface）。
 *
 * 任务：
 *   T3.1  wiredtiger_open：分配句柄、解析配置、建/开元数据表、恢复已存在的表
 *   T3.2  session->create / drop / open_cursor、conn->close
 *   T3.4  begin/commit/rollback：真事务状态机（回滚要能撤销修改）
 * -*/
#include <cstdio>
#include <cstring>

#include "twt_capi.h"

/* -------------------------------------------------------------------------
 * 内部码 → 真 WT 错误码
 *
 * ★ P0/T0.1 已经把 Status 的数值对齐到真 WT（见 include/twt_defs.h），
 *   所以这里其实是恒等映射。保留这个函数是为了留一个「唯一边界」：
 *   以后内部想加自己的错误码（S_NOTSUP 等），只改这里，不污染契约。
 * ------------------------------------------------------------------------- */
int
twt_wt_rc(twt::Status s)
{
    switch (s) {
    case twt::S_OK:
        return 0;
    case twt::S_NOTFOUND:
        return WT_NOTFOUND;
    case twt::S_DUPLICATE:
        return WT_DUPLICATE_KEY;
    case twt::S_ROLLBACK:
        return WT_ROLLBACK;
    case twt::S_RESTART:
        return WT_RESTART;
    case twt::S_PANIC:
        return WT_PANIC;
    case twt::S_CORRUPT:
        return WT_TRY_SALVAGE;
    case twt::S_NOTSUP:
    case twt::S_IO:
    case twt::S_ERROR:
    default:
        return WT_ERROR;
    }
}

namespace {

/* "table:foo" → "foo"（uri 前缀去掉） */
std::string
uri_name(const std::string &uri)
{
    (void)uri;
    UNIMPLEMENTED("P3/T3.2 uri_name");
}

/* "table:foo" → "<home>/foo.wt"（没有 .wt 后缀就补上） */
std::string
uri_to_file(const std::string &home, const std::string &uri)
{
    (void)home;
    (void)uri;
    UNIMPLEMENTED("P3/T3.2 uri_to_file");
}

/*
 * make_table --
 *     为一个 uri 建立 twt_table：算文件名、解析 key_format/value_format、
 *     new Btree 并 open。
 *     注意：建表时 config 里的 key_format 默认 "S"（string）。
 */
twt_table *
make_table(twt_conn *conn, const std::string &uri, const std::string &config)
{
    (void)conn;
    (void)uri;
    (void)config;
    UNIMPLEMENTED("P3/T3.2 make_table");
}

/* ---------------- session 方法 ---------------- */

int
ses_close(WT_SESSION *s, const char *config)
{
    (void)s;
    (void)config;
    UNIMPLEMENTED_RC("P3/T3.2 session.close", WT_ERROR);
}

/*
 * ses_create --
 *     uri 已存在 → WT_ERROR（真 WT 返回 EEXIST）。
 *     否则 make_table + 往元数据表 put(uri, config) + 记进 conn->tables。
 *     任何一步失败都要回滚已分配的资源（别泄漏 Btree / 文件）。
 */
int
ses_create(WT_SESSION *s, const char *uri, const char *config)
{
    (void)s;
    (void)uri;
    (void)config;
    UNIMPLEMENTED_RC("P3/T3.2 session.create", WT_ERROR);
}

/*
 * ses_drop --
 *     关闭并删除表文件，从 conn->tables 与元数据表里摘掉。
 *     uri 不存在 → WT_NOTFOUND。
 */
int
ses_drop(WT_SESSION *s, const char *uri, const char *config)
{
    (void)s;
    (void)uri;
    (void)config;
    UNIMPLEMENTED_RC("P3/T3.2 session.drop", WT_ERROR);
}

/*
 * ses_open_cursor --
 *     uri 形如 "table:foo"。表不存在 → WT_NOTFOUND（不是 WT_ERROR）。
 *     config 里可能有 overwrite=true；表本身的配置也可能带 overwrite。
 *     成功时 *cursorp 非空，返回 0。
 */
int
ses_open_cursor(WT_SESSION *s, const char *uri, WT_CURSOR *to_dup, const char *config,
  WT_CURSOR **cursorp)
{
    (void)s;
    (void)uri;
    (void)to_dup;
    (void)config;
    (void)cursorp;
    UNIMPLEMENTED_RC("P3/T3.2 session.open_cursor", WT_ERROR);
}

/*
 * ses_checkpoint --
 *     对连接下所有表 + 元数据表做 checkpoint。任一失败返回 WT_ERROR。
 */
int
ses_checkpoint(WT_SESSION *s, const char *config)
{
    (void)s;
    (void)config;
    UNIMPLEMENTED_RC("P3/T3.2 session.checkpoint", WT_ERROR);
}

/* ---------------- 事务（P3/T3.4） ---------------- */

/*
 * begin_transaction --
 *     真 WT 的语义：为这个 session 开一个事务，固定一个快照
 *     （snapshot = 当前活跃事务集合），后续所有读写都用这个快照。
 *     本版先用简单规则：分配 txn id，记录 read_ts。
 *     嵌套调用（已有事务时再 begin）→ WT_ERROR。
 */
int
ses_begin_txn(WT_SESSION *s, const char *config)
{
    (void)s;
    (void)config;
    UNIMPLEMENTED_RC("P3/T3.4 session.begin_transaction", WT_ERROR);
}

/*
 * commit_transaction --
 *     提交：把本事务的修改标成可见（更新 update 链上的 txnid / start_ts），
 *     释放快照。没有活跃事务 → WT_ERROR。
 */
int
ses_commit_txn(WT_SESSION *s, const char *config)
{
    (void)s;
    (void)config;
    UNIMPLEMENTED_RC("P3/T3.4 session.commit_transaction", WT_ERROR);
}

/*
 * rollback_transaction --
 *     回滚：撤销本事务写下的所有 Update（把它们从链上摘掉并释放），
 *     之后读到的必须还是事务开始前的值。
 *     ★ 这是 T3.4 的核心难点：需要记录「本事务改过哪些 key / 挂过哪些 Update」。
 */
int
ses_rollback_txn(WT_SESSION *s, const char *config)
{
    (void)s;
    (void)config;
    UNIMPLEMENTED_RC("P3/T3.4 session.rollback_transaction", WT_ERROR);
}

/*
 * ses_truncate --
 *     删除 [start, stop] 区间内的所有键。start/stop 为 NULL 表示从头/到尾。
 *     简化：先按 cursor 遍历 + remove 实现即可。
 */
int
ses_truncate(WT_SESSION *s, const char *uri, WT_CURSOR *start, WT_CURSOR *stop, const char *config)
{
    (void)s;
    (void)uri;
    (void)start;
    (void)stop;
    (void)config;
    UNIMPLEMENTED_RC("P3/T3.4 session.truncate", WT_ERROR);
}

/* ---------------- connection 方法 ---------------- */

/*
 * conn_open_session --
 *     分配 twt_session、回指 conn、绑定方法表。
 *     ★ 方法表绑定这段是「已给」的 ABI 样板：少绑一个，真工具调过来就是
 *       空指针崩溃。注意 WT_SESSION::close 的签名是 (WT_SESSION*, const char*)。
 */
int
conn_open_session(WT_CONNECTION *c, WT_EVENT_HANDLER *handler, const char *config,
  WT_SESSION **sessionp)
{
    (void)handler;
    (void)config;
    if (sessionp == nullptr)
        return WT_ERROR;

    auto *ses = new twt_session();
    ses->conn = conn_of(c);
    ses->iface.connection = c;
    ses->iface.close = ses_close;
    ses->iface.create = ses_create;
    ses->iface.drop = ses_drop;
    ses->iface.open_cursor = ses_open_cursor;
    ses->iface.checkpoint = ses_checkpoint;
    ses->iface.begin_transaction = ses_begin_txn;
    ses->iface.commit_transaction = ses_commit_txn;
    ses->iface.rollback_transaction = ses_rollback_txn;
    ses->iface.truncate = ses_truncate;
    *sessionp = &ses->iface;
    return 0;
}

/*
 * conn_close --
 *     干净的关闭语义：所有表 checkpoint + close，元数据表 checkpoint + close，
 *     最后 delete conn。
 */
int
conn_close(WT_CONNECTION *c, const char *config)
{
    (void)c;
    (void)config;
    UNIMPLEMENTED_RC("P3/T3.2 connection.close", WT_ERROR);
}

} /* namespace */

/*
 * wiredtiger_strerror --
 *     已给：错误码 → 人话。数值必须与 compat/wiredtiger.h 一致。
 */
extern "C" const char *
wiredtiger_strerror(int error)
{
    switch (error) {
    case 0:
        return "WT_OK";
    case WT_ROLLBACK:
        return "WT_ROLLBACK: conflict between concurrent operations";
    case WT_DUPLICATE_KEY:
        return "WT_DUPLICATE_KEY: attempt to insert an existing key";
    case WT_ERROR:
        return "WT_ERROR: non-specific WiredTiger error";
    case WT_NOTFOUND:
        return "WT_NOTFOUND: item not found";
    case WT_PANIC:
        return "WT_PANIC: WiredTiger library panic";
    case WT_RESTART:
        return "WT_RESTART: restart the operation";
    case WT_CACHE_FULL:
        return "WT_CACHE_FULL: operation would overflow cache";
    case WT_PREPARE_CONFLICT:
        return "WT_PREPARE_CONFLICT: conflict with a prepared update";
    case WT_TRY_SALVAGE:
        return "WT_TRY_SALVAGE: database corruption detected";
    default:
        return "unknown WiredTiger error";
    }
}

/*
 * wiredtiger_open --
 *     1. connectionp 为空 → WT_ERROR；成功时先置 *connectionp = nullptr。
 *     2. home 为空 → "."。
 *     3. cfg_parse 配置；cache_size 换算成 cfg.max_cached_pages
 *        （本版按 32KB/页估算，下限 8 页，默认 8MB）。
 *     4. 打开元数据表 <home>/WiredTiger.wt（它就是一张普通的 tinywt 表，
 *        key = uri，value = 建表配置串）。
 *     5. scan_all 扫元数据，为每个 uri 调 make_table 恢复进 conn->tables。
 *     6. 绑定 iface.close / iface.open_session，*connectionp = &conn->iface。
 *
 * 真 WT: wiredtiger_open() (src/conn/conn_api.c)
 */
extern "C" int
wiredtiger_open(const char *home, WT_EVENT_HANDLER *event_handler, const char *config,
  WT_CONNECTION **connectionp)
{
    (void)home;
    (void)event_handler;
    (void)config;
    (void)connectionp;
    UNIMPLEMENTED_RC("P3/T3.1 wiredtiger_open", WT_ERROR);
}
