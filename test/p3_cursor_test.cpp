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
#define TWT_PROJECT "P3"

#include <cstdlib>
#include <string>
#include <vector>

#include "harness.hpp"
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
static void
test_scan_forward()
{
    CASE("P3/T3.3 全序正向扫描（next）");
    DB db;
    if (!db.open("p3_cur_fwd")) {
        CHECK(false);
        return;
    }
    fill(db.cur, 800);

    /* 定位到第一条：search 一个比最小键还小的键，然后 next */
    db.cur->set_key(db.cur, "key-");
    int rc = db.cur->search(db.cur);
    CHECK(rc == WT_NOTFOUND);

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
    CHECK(rc == WT_NOTFOUND); /* 正常结束必须是 NOTFOUND */
    CHECK(n == 800);
    CHECK(order_ok);

    db.close();
}

/* ---------------------------------------------------------------- 2. 全序反向 */
static void
test_scan_reverse()
{
    CASE("P3/T3.3 全序反向扫描（prev）");
    DB db;
    if (!db.open("p3_cur_rev")) {
        CHECK(false);
        return;
    }
    fill(db.cur, 500);

    /* 定位到最后一条：search 一个比最大键还大的键，然后 prev */
    db.cur->set_key(db.cur, "key-~");
    CHECK(db.cur->search(db.cur) == WT_NOTFOUND);

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
    CHECK(rc == WT_NOTFOUND);
    CHECK(n == 500);
    CHECK(order_ok);

    db.close();
}

/* ---------------------------------------------------------------- 3. search 后 next */
static void
test_search_then_next()
{
    CASE("P3/T3.3 search 之后 next 从命中位置继续");
    DB db;
    if (!db.open("p3_cur_seek")) {
        CHECK(false);
        return;
    }
    fill(db.cur, 300);

    db.cur->set_key(db.cur, kstr(100).c_str());
    CHECK(db.cur->search(db.cur) == 0);

    /* 命中之后 next 必须走到 101，而不是回到 0 */
    CHECK(db.cur->next(db.cur) == 0);
    {
        char *k = nullptr;
        db.cur->get_key(db.cur, &k);
        CHECK(k != nullptr && std::string(k) == kstr(101));
    }
    /* prev 回到 100 */
    CHECK(db.cur->prev(db.cur) == 0);
    {
        char *k = nullptr;
        db.cur->get_key(db.cur, &k);
        CHECK(k != nullptr && std::string(k) == kstr(100));
    }

    /* 未定位的游标直接 next：真 WT 返回 WT_NOTFOUND（不会替你从头扫） */
    CHECK(db.cur->reset(db.cur) == 0);
    CHECK(db.cur->next(db.cur) == WT_NOTFOUND);

    db.close();
}

/* ---------------------------------------------------------------- 4. seek 三边界 */
static void
test_seek_boundaries()
{
    CASE("P3/T3.3 search_near 三边界");
    DB db;
    if (!db.open("p3_cur_near")) {
        CHECK(false);
        return;
    }
    fill(db.cur, 100);

    /* (a) 小于最小值 → 近似命中最小的那条，exact = 0 */
    {
        int exact = -1;
        db.cur->set_key(db.cur, "aaa");
        int rc = db.cur->search_near(db.cur, &exact);
        CHECK(rc == 0);
        CHECK(exact == 0);
        char *k = nullptr;
        db.cur->get_key(db.cur, &k);
        CHECK(k != nullptr && std::string(k) == kstr(0));
    }

    /* (b) 正好命中 → exact = 1 */
    {
        int exact = -1;
        db.cur->set_key(db.cur, kstr(50).c_str());
        CHECK(db.cur->search_near(db.cur, &exact) == 0);
        CHECK(exact == 1);
    }

    /* (c) 大于最大值 → 近似命中最末尾，exact = 0 */
    {
        int exact = -1;
        db.cur->set_key(db.cur, "zzz");
        CHECK(db.cur->search_near(db.cur, &exact) == 0);
        CHECK(exact == 0);
        char *k = nullptr;
        db.cur->get_key(db.cur, &k);
        CHECK(k != nullptr && std::string(k) == kstr(99));
    }

    db.close();
}

/* ---------------------------------------------------------------- 5. 删除后扫描 */
static void
test_scan_after_delete()
{
    CASE("P3/T3.3 删除后扫描（tombstone 不出现）");
    DB db;
    if (!db.open("p3_cur_del")) {
        CHECK(false);
        return;
    }
    fill(db.cur, 200);

    /* 删掉偶数键 */
    for (int i = 0; i < 200; i += 2) {
        db.cur->set_key(db.cur, kstr(i).c_str());
        if (db.cur->remove(db.cur) != 0) {
            CHECK(false);
            break;
        }
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
    CHECK(n == 100);
    CHECK(all_odd);

    db.close();
}

/* ---------------------------------------------------------------- 6. 跨页 + 淘汰 */
static void
test_scan_across_pages()
{
    CASE("P3/T3.3 跨页遍历 + cache 压力（页被淘汰后仍正确）");
    /* cache_size 故意很小，逼 eviction 在遍历过程中发生 */
    DB db;
    if (!db.open("p3_cur_pages", "cache_size=1M")) {
        CHECK(false);
        return;
    }
    fill(db.cur, 2000);

    int n = 0;
    bool ok = true;
    while (db.cur->next(db.cur) == 0) {
        char *k = nullptr, *v = nullptr;
        db.cur->get_key(db.cur, &k);
        db.cur->get_value(db.cur, &v);
        if (k == nullptr || v == nullptr || std::string(k) != kstr(n) ||
          std::string(v) != vstr(n))
            ok = false;
        ++n;
    }
    CHECK(n == 2000);
    CHECK(ok);

    /* 再来一遍反向，确认游标 reset 之后还能从头用 */
    CHECK(db.cur->reset(db.cur) == 0);
    int m = 0;
    while (db.cur->prev(db.cur) == 0)
        ++m;
    CHECK(m == 2000);

    db.close();
}

int
main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("tinywt P3: cursor traversal\n");
    RUN(test_scan_forward);
    RUN(test_scan_reverse);
    RUN(test_search_then_next);
    RUN(test_seek_boundaries);
    RUN(test_scan_after_delete);
    RUN(test_scan_across_pages);
    return harness_report();
}
