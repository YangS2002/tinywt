/*-
 * P3 测试 —— C ABI：配置解析 / 建表 / 写路径 / 点查 / 元数据持久化
 *
 * 这一层是"链接器评分器"的第一块拼图：所有调用都走 third_party/wiredtiger/include/wiredtiger.h
 * 声明的 C ABI（varargs、函数表、错误码），不碰 C++ 内核。
 * -*/
#include <cstring>
#include <string>
#include <vector>

#include "support/harness.hpp"
#include "wiredtiger.h"

/* C ABI 边界的唯一错误码出口（定义在 src/api/conn.cpp） */
int twt_wt_rc(twt::Status s);

/*
 * T3.0：C ABI 的错误码契约。
 * 这一项不需要实现，但如果 P0 的 Status 数值被动过，这里会立刻红。
 */
TWT_TEST(P3_Api, ErrorContract, "P3/T3.0 错误码契约（Status -> WT_*）")
{
    EXPECT_EQ(0, twt_wt_rc(twt::S_OK));
    EXPECT_EQ(WT_NOTFOUND, twt_wt_rc(twt::S_NOTFOUND));
    EXPECT_EQ(WT_DUPLICATE_KEY, twt_wt_rc(twt::S_DUPLICATE));
    EXPECT_EQ(WT_ROLLBACK, twt_wt_rc(twt::S_ROLLBACK));
    EXPECT_EQ(WT_TRY_SALVAGE, twt_wt_rc(twt::S_CORRUPT)) << "损坏必须映射成 WT_TRY_SALVAGE";
    EXPECT_NE(WT_NOTFOUND, WT_ROLLBACK);
}

TWT_TEST(P3_Api, ConfigParser, "P3/T3.1 config parser（真 ABI）")
{
    const char *cfg = "cache_size=50M,eviction=(threads_max=8),overwrite=true,verbose";
    WT_CONFIG_PARSER *cp = nullptr;
    EXPECT_EQ(0, wiredtiger_config_parser_open(nullptr, cfg, std::strlen(cfg), &cp));
    ASSERT_NE(nullptr, cp) << "config parser 没分配出来";

    WT_CONFIG_ITEM k, v;
    int n = 0;
    std::string keys;
    while (cp->next(cp, &k, &v) == 0) {
        keys.append(std::string(k.str, k.len)).append(";");
        if (std::string(k.str, k.len) == "cache_size") {
            EXPECT_EQ("50M", std::string(v.str, v.len));
            EXPECT_TRUE(v.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_ID ||
              v.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_NUM);
        }
        if (std::string(k.str, k.len) == "eviction") {
            EXPECT_EQ(WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT, v.type) << "嵌套配置必须是 STRUCT";
            EXPECT_EQ("(threads_max=8)", std::string(v.str, v.len));
        }
        ++n;
    }
    EXPECT_EQ(4, n) << "顶层应该有 4 个 key";
    EXPECT_EQ("cache_size;eviction;overwrite;verbose;", keys);

    WT_CONFIG_ITEM got;
    EXPECT_EQ(0, cp->get(cp, "overwrite", &got));
    EXPECT_EQ("true", std::string(got.str, got.len));
    EXPECT_EQ(WT_CONFIG_ITEM::WT_CONFIG_ITEM_BOOL, got.type);
    EXPECT_EQ(WT_NOTFOUND, cp->get(cp, "nope", &got));
    EXPECT_EQ(0, cp->close(cp));
}

TWT_TEST(P3_Api, Roundtrip, "P3/T3.2 open/create/insert/search（与 T3.1 串成一条链）")
{
    std::string home = dbpath("p3_api");
    rmpath(home);
    fs::create_directories(home);

    WT_CONNECTION *conn = nullptr;
    EXPECT_EQ(0, wiredtiger_open(home.c_str(), nullptr, "cache_size=16M", &conn));
    ASSERT_NE(nullptr, conn) << "wiredtiger_open 没返回连接";

    WT_SESSION *ses = nullptr;
    EXPECT_EQ(0, conn->open_session(conn, nullptr, nullptr, &ses));
    ASSERT_NE(nullptr, ses);

    EXPECT_EQ(0, ses->create(ses, "table:t1", "key_format=S,value_format=S"));
    /* 重复建表必须失败 */
    EXPECT_EQ(WT_ERROR, ses->create(ses, "table:t1", "key_format=S,value_format=S"))
      << "重复建表必须报 WT_ERROR";

    WT_CURSOR *cur = nullptr;
    EXPECT_EQ(0, ses->open_cursor(ses, "table:t1", nullptr, nullptr, &cur));
    ASSERT_NE(nullptr, cur);

    /* 写 500 条 */
    char kbuf[32], vbuf[64];
    for (int i = 0; i < 500; ++i) {
        std::snprintf(kbuf, sizeof(kbuf), "key-%05d", i);
        std::snprintf(vbuf, sizeof(vbuf), "value-%05d", i);
        cur->set_key(cur, kbuf);
        cur->set_value(cur, vbuf);
        ASSERT_EQ(0, cur->insert(cur)) << "第 " << i << " 条插入失败";
    }

    /* 点查命中 */
    cur->set_key(cur, "key-00042");
    EXPECT_EQ(0, cur->search(cur));
    {
        char *got = nullptr;
        EXPECT_EQ(0, cur->get_value(cur, &got));
        ASSERT_NE(nullptr, got);
        EXPECT_EQ("value-00042", std::string(got));
    }

    /* 点查未命中：必须是 WT_NOTFOUND(-31803)，绝不能变成 WT_ROLLBACK(-31800) */
    cur->set_key(cur, "key-99999");
    int rc = cur->search(cur);
    EXPECT_EQ(WT_NOTFOUND, rc);
    EXPECT_NE(WT_ROLLBACK, rc);
    EXPECT_EQ(-31803, rc);

    /* 重复插入 → WT_DUPLICATE_KEY */
    cur->set_key(cur, "key-00042");
    cur->set_value(cur, "dup");
    EXPECT_EQ(WT_DUPLICATE_KEY, cur->insert(cur));

    /* update / remove */
    cur->set_key(cur, "key-00042");
    cur->set_value(cur, "updated");
    EXPECT_EQ(0, cur->update(cur));
    cur->set_key(cur, "key-00042");
    EXPECT_EQ(0, cur->search(cur));
    {
        char *got = nullptr;
        cur->get_value(cur, &got);
        ASSERT_NE(nullptr, got);
        EXPECT_EQ("updated", std::string(got));
    }
    cur->set_key(cur, "key-00042");
    EXPECT_EQ(0, cur->remove(cur));
    cur->set_key(cur, "key-00042");
    EXPECT_EQ(WT_NOTFOUND, cur->search(cur));

    EXPECT_EQ(0, cur->close(cur));
    EXPECT_EQ(0, ses->checkpoint(ses, nullptr));
    EXPECT_EQ(0, ses->close(ses, NULL));
    EXPECT_EQ(0, conn->close(conn, nullptr));

    /* 元数据本身是一张 tinywt Btree */
    EXPECT_TRUE(fs::exists(home + "/WiredTiger.wt")) << "缺 WiredTiger.wt（元数据表没落盘）";
    EXPECT_TRUE(fs::exists(home + "/t1.wt")) << "缺 t1.wt";
    rmpath(home);
}

/*
 * 元数据持久化：WiredTiger.wt 就是一张普通的表（uri -> 配置串），
 * 所以「重开连接能恢复所有表」这件事完全依赖 P2 的 checkpoint 正确性。
 */
TWT_TEST(P3_Api, ReopenMetadata, "P3/T3.1 重开连接：元数据恢复 + 数据仍在")
{
    std::string home = dbpath("p3_api");
    rmpath(home);
    fs::create_directories(home);

    /* 先建库写数据 */
    {
        WT_CONNECTION *conn = nullptr;
        ASSERT_EQ(0, wiredtiger_open(home.c_str(), nullptr, "cache_size=16M", &conn));
        ASSERT_NE(nullptr, conn);
        WT_SESSION *ses = nullptr;
        ASSERT_EQ(0, conn->open_session(conn, nullptr, nullptr, &ses));
        ASSERT_EQ(0, ses->create(ses, "table:t1", "key_format=S,value_format=S"));
        WT_CURSOR *cur = nullptr;
        ASSERT_EQ(0, ses->open_cursor(ses, "table:t1", nullptr, nullptr, &cur));
        char kbuf[32], vbuf[64];
        for (int i = 0; i < 200; ++i) {
            std::snprintf(kbuf, sizeof(kbuf), "key-%05d", i);
            std::snprintf(vbuf, sizeof(vbuf), "value-%05d", i);
            cur->set_key(cur, kbuf);
            cur->set_value(cur, vbuf);
            ASSERT_EQ(0, cur->insert(cur)) << "第 " << i << " 条插入失败";
        }
        cur->set_key(cur, "key-00042");
        ASSERT_EQ(0, cur->remove(cur));
        ASSERT_EQ(0, cur->close(cur));
        ASSERT_EQ(0, conn->close(conn, nullptr));
    }

    WT_CONNECTION *conn = nullptr;
    WT_SESSION *ses = nullptr;
    EXPECT_EQ(0, wiredtiger_open(home.c_str(), nullptr, "cache_size=16M", &conn));
    ASSERT_NE(nullptr, conn) << "重开连接失败";
    EXPECT_EQ(0, conn->open_session(conn, nullptr, nullptr, &ses));
    ASSERT_NE(nullptr, ses);
    WT_CURSOR *cur = nullptr;
    EXPECT_EQ(0, ses->open_cursor(ses, "table:t1", nullptr, nullptr, &cur))
      << "重开后表不见了（元数据没恢复）";
    ASSERT_NE(nullptr, cur);

    cur->set_key(cur, "key-00007");
    EXPECT_EQ(0, cur->search(cur));
    {
        char *got = nullptr;
        cur->get_value(cur, &got);
        ASSERT_NE(nullptr, got);
        EXPECT_EQ("value-00007", std::string(got));
    }
    /* 删掉的那条重开后仍然是 WT_NOTFOUND */
    cur->set_key(cur, "key-00042");
    EXPECT_EQ(WT_NOTFOUND, cur->search(cur));

    EXPECT_EQ(0, cur->close(cur));
    EXPECT_EQ(0, ses->close(ses, NULL));
    EXPECT_EQ(0, conn->close(conn, nullptr));
    rmpath(home);
}

TWT_TEST(P3_Api, Strerror, "P3/T3.0 strerror 与错误码")
{
    EXPECT_NE(nullptr, std::strstr(wiredtiger_strerror(0), "WT_OK"));
    EXPECT_NE(nullptr, std::strstr(wiredtiger_strerror(WT_NOTFOUND), "WT_NOTFOUND"));
    EXPECT_NE(nullptr, std::strstr(wiredtiger_strerror(WT_ROLLBACK), "WT_ROLLBACK"));
    EXPECT_EQ(-31803, WT_NOTFOUND);
    EXPECT_EQ(-31800, WT_ROLLBACK);
}
