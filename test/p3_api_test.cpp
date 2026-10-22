/*-
 * P3 测试 —— C ABI：配置解析 / 建表 / 写路径 / 点查 / 元数据持久化
 *
 * 这一层是"链接器评分器"的第一块拼图：所有调用都走 compat/wiredtiger.h
 * 声明的 C ABI（varargs、函数表、错误码），不碰 C++ 内核。
 * -*/
#define TWT_PROJECT "P3"

#include <cstring>
#include <string>
#include <vector>

#include "harness.hpp"
#include "wiredtiger.h"

/* C ABI 边界的唯一错误码出口（定义在 src_c/conn.cpp） */
int twt_wt_rc(twt::Status s);

/*
 * T3.0：C ABI 的错误码契约。
 * 这一项不需要实现，但如果 P0 的 Status 数值被动过，这里会立刻红。
 */
static void
test_error_contract()
{
    CASE("P3/T3.0 错误码契约（Status -> WT_*）");
    CHECK(twt_wt_rc(twt::S_OK) == 0);
    CHECK(twt_wt_rc(twt::S_NOTFOUND) == WT_NOTFOUND);
    CHECK(twt_wt_rc(twt::S_DUPLICATE) == WT_DUPLICATE_KEY);
    CHECK(twt_wt_rc(twt::S_ROLLBACK) == WT_ROLLBACK);
    CHECK(twt_wt_rc(twt::S_CORRUPT) == WT_TRY_SALVAGE);
    CHECK(WT_NOTFOUND != WT_ROLLBACK);
}

static void
test_config_parser()
{
    CASE("P3/T3.1 config parser（真 ABI）");
    const char *cfg = "cache_size=50M,eviction=(threads_max=8),overwrite=true,verbose";
    WT_CONFIG_PARSER *cp = nullptr;
    CHECK(wiredtiger_config_parser_open(nullptr, cfg, std::strlen(cfg), &cp) == 0);
    CHECK(cp != nullptr);
    if (cp == nullptr)
        return;

    WT_CONFIG_ITEM k, v;
    int n = 0;
    std::string keys;
    while (cp->next(cp, &k, &v) == 0) {
        keys.append(std::string(k.str, k.len)).append(";");
        if (std::string(k.str, k.len) == "cache_size") {
            CHECK(std::string(v.str, v.len) == "50M");
            CHECK(v.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID || v.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM);
        }
        if (std::string(k.str, k.len) == "eviction") {
            CHECK(v.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);
            CHECK(std::string(v.str, v.len) == "(threads_max=8)");
        }
        ++n;
    }
    CHECK(n == 4);
    CHECK(keys == "cache_size;eviction;overwrite;verbose;");

    WT_CONFIG_ITEM got;
    CHECK(cp->get(cp, "overwrite", &got) == 0);
    CHECK(std::string(got.str, got.len) == "true");
    CHECK(got.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL);
    CHECK(cp->get(cp, "nope", &got) == WT_NOTFOUND);
    CHECK(cp->close(cp) == 0);
}

static void
test_api_roundtrip()
{
    CASE("P3/T3.1+T3.2 open/create/insert/search");
    std::string home = dbpath("p3_api");
    rmpath(home);
    fs::create_directories(home);

    WT_CONNECTION *conn = nullptr;
    CHECK(wiredtiger_open(home.c_str(), nullptr, "cache_size=16M", &conn) == 0);
    CHECK(conn != nullptr);
    if (conn == nullptr)
        return;

    WT_SESSION *ses = nullptr;
    CHECK(conn->open_session(conn, nullptr, nullptr, &ses) == 0);

    CHECK(ses->create(ses, "table:t1", "key_format=S,value_format=S") == 0);
    /* 重复建表必须失败 */
    CHECK(ses->create(ses, "table:t1", "key_format=S,value_format=S") == WT_ERROR);

    WT_CURSOR *cur = nullptr;
    CHECK(ses->open_cursor(ses, "table:t1", nullptr, nullptr, &cur) == 0);

    /* 写 500 条 */
    char kbuf[32], vbuf[64];
    for (int i = 0; i < 500; ++i) {
        std::snprintf(kbuf, sizeof(kbuf), "key-%05d", i);
        std::snprintf(vbuf, sizeof(vbuf), "value-%05d", i);
        cur->set_key(cur, kbuf);
        cur->set_value(cur, vbuf);
        CHECK(cur->insert(cur) == 0);
    }

    /* 点查命中 */
    cur->set_key(cur, "key-00042");
    CHECK(cur->search(cur) == 0);
    {
        char *got = nullptr;
        CHECK(cur->get_value(cur, &got) == 0);
        CHECK(got != nullptr && std::string(got) == "value-00042");
    }

    /* 点查未命中：必须是 WT_NOTFOUND(-31803)，绝不能变成 WT_ROLLBACK(-31800) */
    cur->set_key(cur, "key-99999");
    int rc = cur->search(cur);
    CHECK(rc == WT_NOTFOUND);
    CHECK(rc != WT_ROLLBACK);
    CHECK(rc == -31803);

    /* 重复插入 → WT_DUPLICATE_KEY */
    cur->set_key(cur, "key-00042");
    cur->set_value(cur, "dup");
    CHECK(cur->insert(cur) == WT_DUPLICATE_KEY);

    /* update / remove */
    cur->set_key(cur, "key-00042");
    cur->set_value(cur, "updated");
    CHECK(cur->update(cur) == 0);
    cur->set_key(cur, "key-00042");
    CHECK(cur->search(cur) == 0);
    {
        char *got = nullptr;
        cur->get_value(cur, &got);
        CHECK(std::string(got) == "updated");
    }
    cur->set_key(cur, "key-00042");
    CHECK(cur->remove(cur) == 0);
    cur->set_key(cur, "key-00042");
    CHECK(cur->search(cur) == WT_NOTFOUND);

    CHECK(cur->close(cur) == 0);
    CHECK(ses->checkpoint(ses, nullptr) == 0);
    CHECK(ses->close(ses, NULL) == 0);
    CHECK(conn->close(conn, nullptr) == 0);

    /* 元数据本身是一张 tinywt Btree */
    CHECK(fs::exists(home + "/WiredTiger.wt"));
    CHECK(fs::exists(home + "/t1.wt"));
    rmpath(home);
}

/*
 * 元数据持久化：WiredTiger.wt 就是一张普通的表（uri -> 配置串），
 * 所以「重开连接能恢复所有表」这件事完全依赖 P2 的 checkpoint 正确性。
 */
static void
test_reopen_metadata()
{
    CASE("P3/T3.1 重开连接：元数据恢复 + 数据仍在");
    std::string home = dbpath("p3_api");
    rmpath(home);
    fs::create_directories(home);

    /* 先建库写数据 */
    {
        WT_CONNECTION *conn = nullptr;
        CHECK(wiredtiger_open(home.c_str(), nullptr, "cache_size=16M", &conn) == 0);
        if (conn == nullptr)
            return;
        WT_SESSION *ses = nullptr;
        CHECK(conn->open_session(conn, nullptr, nullptr, &ses) == 0);
        CHECK(ses->create(ses, "table:t1", "key_format=S,value_format=S") == 0);
        WT_CURSOR *cur = nullptr;
        CHECK(ses->open_cursor(ses, "table:t1", nullptr, nullptr, &cur) == 0);
        char kbuf[32], vbuf[64];
        for (int i = 0; i < 200; ++i) {
            std::snprintf(kbuf, sizeof(kbuf), "key-%05d", i);
            std::snprintf(vbuf, sizeof(vbuf), "value-%05d", i);
            cur->set_key(cur, kbuf);
            cur->set_value(cur, vbuf);
            CHECK(cur->insert(cur) == 0);
        }
        cur->set_key(cur, "key-00042");
        CHECK(cur->remove(cur) == 0);
        CHECK(cur->close(cur) == 0);
        CHECK(conn->close(conn, nullptr) == 0);
    }

    WT_CONNECTION *conn = nullptr;
    WT_SESSION *ses = nullptr;
    CHECK(wiredtiger_open(home.c_str(), nullptr, "cache_size=16M", &conn) == 0);
    CHECK(conn != nullptr);
    if (conn == nullptr)
        return;
    CHECK(conn->open_session(conn, nullptr, nullptr, &ses) == 0);
    WT_CURSOR *cur = nullptr;
    CHECK(ses->open_cursor(ses, "table:t1", nullptr, nullptr, &cur) == 0);

    cur->set_key(cur, "key-00007");
    CHECK(cur->search(cur) == 0);
    {
        char *got = nullptr;
        cur->get_value(cur, &got);
        CHECK(got != nullptr && std::string(got) == "value-00007");
    }
    /* 删掉的那条重开后仍然是 WT_NOTFOUND */
    cur->set_key(cur, "key-00042");
    CHECK(cur->search(cur) == WT_NOTFOUND);

    CHECK(cur->close(cur) == 0);
    CHECK(ses->close(ses, NULL) == 0);
    CHECK(conn->close(conn, nullptr) == 0);
    rmpath(home);
}

static void
test_strerror()
{
    CASE("P3/T3.0 strerror 与错误码");
    CHECK(std::strstr(wiredtiger_strerror(0), "WT_OK") != nullptr);
    CHECK(std::strstr(wiredtiger_strerror(WT_NOTFOUND), "WT_NOTFOUND") != nullptr);
    CHECK(std::strstr(wiredtiger_strerror(WT_ROLLBACK), "WT_ROLLBACK") != nullptr);
    CHECK(WT_NOTFOUND == -31803);
    CHECK(WT_ROLLBACK == -31800);
}

int
main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("tinywt P3: C ABI\n");
    RUN(test_error_contract);
    RUN(test_config_parser);
    RUN(test_api_roundtrip);
    RUN(test_reopen_metadata);
    RUN(test_strerror);
    return harness_report();
}
