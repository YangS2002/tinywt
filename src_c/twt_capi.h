/*-
 * src_c/twt_capi.h —— C ABI 层的内部结构
 *
 * 分层（对标真 WT 的 src/ 与 libwiredtiger 的关系）：
 *   compat/wiredtiger.h  公开 API（真头文件，不改）
 *   src_c 下的 .cpp      extern "C" 实现，包 C++ 内核（twt::Btree）
 *   include/ src/        C++17 内核
 *
 * 句柄转换技巧：iface 必须是结构体首成员，这样 WT_CONNECTION* 与
 * twt_conn* 是同一个地址，强转即可（真 WT 也是这个套路，见 conn->iface）。
 * -*/
#pragma once

#include <map>
#include <string>
#include <vector>

#include "twt_btree.h"
#include "twt_txn.h"
#include "wiredtiger.h"

struct twt_table {
    std::string uri;    /* "table:foo" */
    std::string file;   /* home/foo.wt */
    std::string config; /* 建表时的配置串，原样存进元数据 */
    std::string key_format = "S";
    std::string value_format = "S";
    bool overwrite = false;
    twt::Btree *bt = nullptr;
};

struct twt_conn;

struct twt_session {
    WT_SESSION iface; /* 必须首成员 */
    twt_conn *conn = nullptr;
    std::vector<twt_table *> open_cursors_tables; /* 简化：只记引用关系 */

    /*
     * --- P3/T3.4 事务上下文 ---
     * 真 WT 里 session 就是一个事务上下文：cursor 的每次操作都带上
     * session->txn 的快照。这里用 in_txn + txn 复刻同一个模型。
     *
     * ★ cursor 的 insert/update/remove 必须用 txn.id 作为写入者，
     *   并把它挂进 txn.writes（回滚要按这个精确摘链）。
     */
    twt::Txn txn;
    bool in_txn = false;
};

struct twt_cursor {
    WT_CURSOR iface; /* 必须首成员 */
    twt_session *ses = nullptr;
    twt_table *tab = nullptr;
    std::string key;
    std::string val;
    bool key_set = false;
    bool val_set = false;
    bool positioned = false;
    bool overwrite = false;
    twt::CursorState *it = nullptr; /* next/prev 的树位置（懒创建） */
};

struct twt_conn {
    WT_CONNECTION iface; /* 必须首成员 */
    std::string home;
    std::map<std::string, twt_table *> tables; /* uri → table */
    twt::Btree *meta = nullptr;                /* home/WiredTiger.wt：uri → 配置串 */
    twt::Config btree_cfg;

    /* --- P3/T3.4：全局事务表；P2/T2.7：已注册的压缩器 --- */
    twt::TxnManager *txn_mgr = nullptr;
    std::map<std::string, void *> compressors; /* name → WT_COMPRESSOR* */
};

/* ---- 句柄互转（iface 在首成员，指针值相同） ---- */
static inline twt_conn *
conn_of(WT_CONNECTION *c)
{
    return reinterpret_cast<twt_conn *>(c);
}
static inline twt_session *
session_of(WT_SESSION *s)
{
    return reinterpret_cast<twt_session *>(s);
}
static inline twt_cursor *
cursor_of(WT_CURSOR *c)
{
    return reinterpret_cast<twt_cursor *>(c);
}

/* ---- 内部码 → 真 WT 错误码（T3.0.1：绝不能让 -31800 冒充 WT_ROLLBACK） ---- */
int twt_wt_rc(twt::Status s);

/* ---- 内部配置串解析（config.cpp） ---- */
struct CfgItem {
    std::string key;
    std::string value; /* STRING 类型已去掉引号；STRUCT 保留括号原样 */
    uint32_t type;     /* WT_CONFIG_ITEM_* */
    int64_t num;
};

/* 解析 "a=1,b=(c=2,d=3),e" 形式；失败返回 false 并填 err */
bool cfg_parse(const char *s, size_t len, std::vector<CfgItem> &out, std::string &err);
/* 查值（不存在返回 nullptr） */
const CfgItem *cfg_find(const std::vector<CfgItem> &items, const char *key);
/* 解析字节数配置："50M" / "32K" / "4096" */
int64_t cfg_bytes(const CfgItem *it, int64_t dflt);
/* 从配置串里取字符串值 */
std::string cfg_str(const std::vector<CfgItem> &items, const char *key, const std::string &dflt);

/* ---- 工厂（conn.cpp / cursor.cpp 之间共享） ---- */
WT_CURSOR *twt_cursor_create(twt_session *ses, twt_table *tab, const std::string &uri, bool overwrite);
void twt_cursor_destroy(twt_cursor *c);
