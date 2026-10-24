/*-
 * P3/T3.3 游标遍历 —— 对标 BusTub P2 Checkpoint: Iterator
 *
 * 这一项测的是「顺序访问」这条路：next / prev / search / search_near / reset，
 * 以及它们跨叶子页时的行为。真 wtperf 的 read 模式 100% 走这条路，
 * 所以它比点查更能暴露 B-Tree 的问题。
 *
 * 评分点（每项一个测试函数）：
 *   1. 全序正向扫描         → 顺序、条数、值都对
 *   2. 全序反向扫描         → 从尾到头，一条不差
 *   3. search 之后 next      → 从命中位置继续，不是从头开始
 *   4. seek 三边界          → 小于最小值 / 中间 / 大于最大值
 *   5. 删除后扫描           → tombstone 不出现
 *   6. 跨页 + cache 压力     → 叶子页被淘汰后遍历仍然正确
 * -*/
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "support/harness.hpp"
#include "wiredtiger.h"

namespace {

struct DB {
    std::string home;
    WT_CONNECTION *conn = nullptr;
    WT_SESSION *ses = nullptr;
    WT_CURSOR *cur = nullptr;

    bool open(const char *name, const char *cfg = "cache_size=16M")
    {
        home = dbpath(name);
        rmpath(home);
        fs::create_directories(home);
        if (wiredtiger_open(home.c_str(), nullptr, cfg, &conn) != 0 || conn == nullptr)
            return false;
        if (conn->open_session(conn, nullptr, nullptr, &ses) != 0)
            return false;
        if (ses->create(ses, "table:t", "key_format=S,value_format=S") != 0)
            return false;
        return ses->open_cursor(ses, "table:t", nullptr, nullptr, &cur) == 0;
    }

    void close()
    {
        if (cur != nullptr)
            (void)cur->close(cur);
        if (ses != nullptr)
            (void)ses->close(ses, nullptr);
        if (conn != nullptr)
            (void)conn->close(conn, nullptr);
        rmpath(home);
    }
};

/* 插入 N 条 key-000000 .. key-00NNNN */
void
fill(WT_CURSOR *cur, int n)
{
    for (int i = 0; i < n; ++i) {
        std::string k = kstr(i), v = vstr(i);
        cur->set_key(cur, k.c_str());
        cur->set_value(cur, v.c_str());
        if (cur->insert(cur) != 0)
            return;
    }
}

} /* namespace */

/* ---------------------------------------------------------------- 1. 全序正向 */
TWT_TEST(P3_Cursor, ScanForward, "P3/T3.3 全序正向扫描（next）")
{
    DB db;
    ASSERT_TRUE(db.open("p3_cur_fwd")) << "建库/建表/开游标失败";
    fill(db.cur, 800);

    /* 定位到第一条：search 一个比最小键还小的键，然后 next */
    db.cur->set_key(db.cur, "key-");
    int rc = db.cur->search(db.cur);
    EXPECT_EQ(WT_NOTFOUND, rc) << "比最小键还小的 key 必须 NOTFOUND";

    int n = 0;
    bool order_ok = true;
    while ((rc = db.cur->next(db.cur)) == 0) {
        char *k = nullptr;
        if (db.cur->get_key(db.cur, &k) != 0 || k == nullptr) {
            order_ok = false;
            break;
        }
        if (std::string(k) != kstr(n))
            order_ok = false;
        ++n;
    }
    EXPECT_EQ(WT_NOTFOUND, rc) << "正常结束必须是 NOTFOUND";
    EXPECT_EQ(800, n) << "正向扫描条数不对";
    EXPECT_TRUE(order_ok) << "正向扫描的顺序/值不对";

    db.close();
}

/* ---------------------------------------------------------------- 2. 全序反向 */
TWT_TEST(P3_Cursor, ScanReverse, "P3/T3.3 全序反向扫描（prev）")
{
    DB db;
    ASSERT_TRUE(db.open("p3_cur_rev")) << "建库/建表/开游标失败";
    fill(db.cur, 500);

    /* 定位到最后一条：search 一个比最大键还大的键，然后 prev */
    db.cur->set_key(db.cur, "key-~");
    EXPECT_EQ(WT_NOTFOUND, db.cur->search(db.cur));

    int n = 0;
    bool order_ok = true;
    int rc = 0;
    while ((rc = db.cur->prev(db.cur)) == 0) {
        char *k = nullptr;
        db.cur->get_key(db.cur, &k);
        if (k == nullptr || std::string(k) != kstr(499 - n))
            order_ok = false;
        ++n;
    }
    EXPECT_EQ(WT_NOTFOUND, rc);
    EXPECT_EQ(500, n) << "反向扫描条数不对";
    EXPECT_TRUE(order_ok) << "反向扫描的顺序/值不对";

    db.close();
}

/* ---------------------------------------------------------------- 3. search 后 next */
TWT_TEST(P3_Cursor, SearchThenNext, "P3/T3.3 search 之后 next 从命中位置继续")
{
    DB db;
    ASSERT_TRUE(db.open("p3_cur_seek")) << "建库/建表/开游标失败";
    fill(db.cur, 300);

    db.cur->set_key(db.cur, kstr(100).c_str());
    EXPECT_EQ(0, db.cur->search(db.cur));

    /* 命中之后 next 必须走到 101，而不是回到 0 */
    EXPECT_EQ(0, db.cur->next(db.cur));
    {
        char *k = nullptr;
        db.cur->get_key(db.cur, &k);
        ASSERT_NE(nullptr, k);
        EXPECT_EQ(kstr(101), std::string(k)) << "next 没有从命中位置继续";
    }
    /* prev 回到 100 */
    EXPECT_EQ(0, db.cur->prev(db.cur));
    {
        char *k = nullptr;
        db.cur->get_key(db.cur, &k);
        ASSERT_NE(nullptr, k);
        EXPECT_EQ(kstr(100), std::string(k));
    }

    /* 未定位的游标直接 next：真 WT 返回 WT_NOTFOUND（不会替你从头扫） */
    EXPECT_EQ(0, db.cur->reset(db.cur));
    EXPECT_EQ(WT_NOTFOUND, db.cur->next(db.cur)) << "reset 之后直接 next 必须 NOTFOUND";

    db.close();
}

/* ---------------------------------------------------------------- 4. seek 三边界 */
TWT_TEST(P3_Cursor, SearchNearBoundaries, "P3/T3.3 search_near 三边界")
{
    DB db;
    ASSERT_TRUE(db.open("p3_cur_near")) << "建库/建表/开游标失败";
    fill(db.cur, 100);

    /* (a) 小于最小值 → 近似命中最小的那条，exact = 0 */
    {
        int exact = -1;
        db.cur->set_key(db.cur, "aaa");
        int rc = db.cur->search_near(db.cur, &exact);
        EXPECT_EQ(0, rc);
        EXPECT_EQ(0, exact) << "小于最小值时 exact 必须是 0";
        char *k = nullptr;
        db.cur->get_key(db.cur, &k);
        ASSERT_NE(nullptr, k);
        EXPECT_EQ(kstr(0), std::string(k)) << "小于最小值应近似命中最小的那条";
    }

    /* (b) 正好命中 → exact = 1 */
    {
        int exact = -1;
        db.cur->set_key(db.cur, kstr(50).c_str());
        EXPECT_EQ(0, db.cur->search_near(db.cur, &exact));
        EXPECT_EQ(1, exact) << "正好命中时 exact 必须是 1";
    }

    /* (c) 大于最大值 → 近似命中最末尾，exact = 0 */
    {
        int exact = -1;
        db.cur->set_key(db.cur, "zzz");
        EXPECT_EQ(0, db.cur->search_near(db.cur, &exact));
        EXPECT_EQ(0, exact);
        char *k = nullptr;
        db.cur->get_key(db.cur, &k);
        ASSERT_NE(nullptr, k);
        EXPECT_EQ(kstr(99), std::string(k)) << "大于最大值应近似命中最后一条";
    }

    db.close();
}

/* ---------------------------------------------------------------- 5. 删除后扫描 */
TWT_TEST(P3_Cursor, ScanAfterDelete, "P3/T3.3 删除后扫描（tombstone 不出现）")
{
    DB db;
    ASSERT_TRUE(db.open("p3_cur_del")) << "建库/建表/开游标失败";
    fill(db.cur, 200);

    /* 删掉偶数键 */
    for (int i = 0; i < 200; i += 2) {
        db.cur->set_key(db.cur, kstr(i).c_str());
        ASSERT_EQ(0, db.cur->remove(db.cur)) << "删除 " << kstr(i) << " 失败";
    }

    int n = 0;
    bool all_odd = true;
    while (db.cur->next(db.cur) == 0) {
        char *k = nullptr;
        db.cur->get_key(db.cur, &k);
        int idx = (k != nullptr) ? std::atoi(std::string(k).c_str() + 4) : -1;
        if (idx % 2 == 0)
            all_odd = false;
        ++n;
    }
    EXPECT_EQ(100, n) << "删掉一半之后应该只剩 100 条";
    EXPECT_TRUE(all_odd) << "tombstone（被删的偶数键）不能出现在扫描里";

    db.close();
}

/* ---------------------------------------------------------------- 6. 跨页 + 淘汰 */
TWT_TEST(P3_Cursor, ScanAcrossPages, "P3/T3.3 跨页遍历 + cache 压力（页被淘汰后仍正确）")
{
    /* cache_size 故意很小，逼 eviction 在遍历过程中发生 */
    DB db;
    ASSERT_TRUE(db.open("p3_cur_pages", "cache_size=1M")) << "建库/建表/开游标失败";
    fill(db.cur, 2000);

    int n = 0;
    bool ok = true;
    while (db.cur->next(db.cur) == 0) {
        char *k = nullptr, *v = nullptr;
        db.cur->get_key(db.cur, &k);
        db.cur->get_value(db.cur, &v);
        if (k == nullptr || v == nullptr || std::string(k) != kstr(n) || std::string(v) != vstr(n))
            ok = false;
        ++n;
    }
    EXPECT_EQ(2000, n) << "跨页遍历条数不对（页被淘汰后丢了位置？）";
    EXPECT_TRUE(ok) << "跨页遍历的顺序/值不对";

    /* 再来一遍反向，确认游标 reset 之后还能从头用 */
    EXPECT_EQ(0, db.cur->reset(db.cur));
    int m = 0;
    while (db.cur->prev(db.cur) == 0)
        ++m;
    EXPECT_EQ(2000, m) << "reset 之后反向再走一遍条数不对";

    db.close();
}

/* ---------------------------------------------------------------- 7. 遍历中 split */
TWT_TEST(P3_Cursor, ScanDuringSplit, "P3/T3.3 遍历中 split（游标位置不丢、不重、不乱序）")
{
    DB db;
    ASSERT_TRUE(db.open("p3_cur_split")) << "建库/建表/开游标失败";
    const int kOrig = 400;
    fill(db.cur, kOrig);

    /* 定位到中间，先走 50 步 —— 游标现在停在第 250 条附近 */
    db.cur->set_key(db.cur, kstr(200).c_str());
    EXPECT_EQ(0, db.cur->search(db.cur));

    int advanced = 0;
    while (advanced < 50) {
        if (db.cur->next(db.cur) != 0)
            break;
        ++advanced;
    }
    EXPECT_EQ(50, advanced);

    /*
     * 用第二个游标插入 200 条「夹在现有键中间」的键（后缀 x），
     * 强制把游标正在走的那些叶子拆开。
     *
     * 这是遍历路径最容易被踩坏的时刻：游标缓存着叶子页的指针/位置，
     * split 之后那个页可能已经被拆、被 reconcile、被淘汰。
     * 正确实现要么改成「按上一个 key 重新 seek」，要么在 split 时修游标栈。
     */
    WT_CURSOR *ins = nullptr;
    EXPECT_EQ(0, db.ses->open_cursor(db.ses, "table:t", nullptr, nullptr, &ins));
    ASSERT_NE(nullptr, ins);

    int inserted = 0;
    for (int i = 200; i < 400; ++i) {
        std::string k = kstr(i) + "x"; /* 字典序：key-000251 < key-000251x < key-000252 */
        ins->set_key(ins, k.c_str());
        ins->set_value(ins, "interleaved");
        if (ins->insert(ins) == 0)
            ++inserted;
    }
    (void)ins->close(ins);
    EXPECT_EQ(200, inserted) << "中间插入的条数不对";

    /* 继续原游标：严格升序、不重复，且原本存在的键一个都不能丢 */
    std::string prev;
    std::set<std::string> got;
    bool ascending = true;
    int seen = 0;

    while (db.cur->next(db.cur) == 0) {
        char *k = nullptr;
        if (db.cur->get_key(db.cur, &k) != 0 || k == nullptr) {
            ascending = false;
            break;
        }
        std::string key(k);
        if (!prev.empty() && key <= prev)
            ascending = false;
        prev = key;
        got.insert(key);
        ++seen;
    }
    EXPECT_TRUE(ascending) << "split 之后游标发生了重复或逆序";

    int missing = 0;
    for (int i = 251; i < kOrig; ++i)
        if (got.find(kstr(i)) == got.end())
            ++missing;
    EXPECT_EQ(0, missing) << "split 之后丢了 " << missing << " 个原本存在的键";

    /* 至少要走过没走完的 149 条原始键；新插入的 x 键看实现，不作要求 */
    EXPECT_GE(seen, kOrig - 251);

    db.close();
}
