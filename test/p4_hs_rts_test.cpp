/*-
 * P4/T4.3–T4.5 History Store / RTS / Watermark
 *
 * 三者的依赖顺序（写代码前先看一遍）：
 *
 *     Watermark（T4.5）  决定「哪些旧版本可以回收」
 *          │
 *          ├──▶ History Store（T4.3）  被回收的版本搬到这里，旧快照还能读
 *          │
 *          └──▶ RTS（T4.4）            崩溃恢复时，从 stable 点之后的东西全部回滚
 *
 * 这三项是 P4 的收口：做完之后「长事务 + 淘汰 + 崩溃恢复」才能同时成立。
 *
 * 对标真 WT：
 *   src/history/hs.c                   __wt_hs_insert_updates / __wt_hs_cursor
 *   src/txn/txn_rollback_to_stable.c   __wt_rollback_to_stable
 *   src/include/time_window.h          WT_TIME_AGGREGATE
 *   src/include/txn.h                  WT_TXN_GLOBAL 的 stable / pinned
 *
 * 评分点（每项一个测试函数）：
 *   1. History Store            → 按 start_ts 找版本、tombstone、btree_id 维度、回收
 *   2. 未提交 reconcile + RTS   → 晚于 stable 的版本被摘掉，stable 之前的不动
 *   3. watermark               → min(stable, pinned)；长事务挂着时水位不动
 * -*/
#define TWT_PROJECT "P4"

#include <string>

#include "harness.hpp"
#include "twt_btree.h"
#include "twt_hs.h"
#include "twt_txn.h"

namespace {

using namespace twt;

Val
vv(const std::string &s)
{
    return Val(s.begin(), s.end());
}

std::string
sv(const Val &v)
{
    return std::string(v.begin(), v.end());
}

Config
small_cfg()
{
    Config c;
    c.max_leaf_entries = 8;
    c.max_intl_refs = 8;
    c.max_cached_pages = 8;
    c.alloc_size = 512;
    return c;
}

/* 造一条只有 upd 链、没有 base 的 entry（aggregate_stop_ts 只关心链） */
Update *
mk_upd(u64 start, u64 stop, u64 durable, Update *next = nullptr, bool prepared = false)
{
    Update *u = new Update();
    u->start_ts = start;
    u->stop_ts = stop;
    u->durable_ts = durable;
    u->type = UPD_STANDARD;
    u->next = next;
    (void)prepared; /* prepared 在 Update 上没有独立字段，这里只验窗口聚合 */
    return u;
}

} /* namespace */

/* ---------------------------------------------------------------- 1. History Store */
static void
test_history_store()
{
    CASE("P4/T4.3 History Store");
    const std::string path = dbpath("p4_hs");
    rmpath(path);

    /* --- 1a. 单元语义：按 start_ts 找「<= read_ts 的最新版本」 --- */
    {
        HistoryStore hs;
        const Key k = key_of("k1");

        TimeWindow w1;
        w1.start_ts = 10;
        w1.stop_ts = 20;
        w1.durable_ts = 10;
        TimeWindow w2;
        w2.start_ts = 20;
        w2.stop_ts = 30;
        w2.durable_ts = 20;
        TimeWindow w3;
        w3.start_ts = 30;
        w3.durable_ts = 30;

        CHECK_ST(hs.insert(1, k, w1, vv("v10"), false));
        CHECK_ST(hs.insert(1, k, w2, vv("v20"), false));
        CHECK_ST(hs.insert(1, k, w3, vv("v30"), false));
        CHECK(hs.versions(1, k) == 3);

        {
            Val out;
            bool found = false;
            TimeWindow got;
            CHECK_ST(hs.read(1, k, 25, out, found, &got));
            CHECK(found);
            CHECK(sv(out) == "v20"); /* <=25 的最新版本是 20 */
            CHECK(got.start_ts == 20);
        }
        {
            Val out;
            bool found = true;
            CHECK_ST(hs.read(1, k, 5, out, found));
            CHECK(!found); /* 比最老的版本还老 → HS 里没有 */
        }
        {
            Val out;
            bool found = false;
            CHECK_ST(hs.read(1, k, 35, out, found));
            CHECK(found && sv(out) == "v30");
        }
        /* btree_id 是 key 的一部分：另一棵树里同名 key 不能串味 */
        {
            Val out;
            bool found = true;
            CHECK_ST(hs.read(2, k, 35, out, found));
            CHECK(!found);
            CHECK(hs.versions(2, k) == 0);
        }

        /* tombstone：读到它就是「这个时间点之后键没了」，不能再往下找更老的 */
        {
            TimeWindow w4;
            w4.start_ts = 40;
            w4.durable_ts = 40;
            CHECK_ST(hs.insert(1, k, w4, Val{}, true));
            CHECK(hs.versions(1, k) == 4);

            Val out;
            bool found = true;
            CHECK_ST(hs.read(1, k, 45, out, found));
            CHECK(!found);
        }

        /* 回收：清掉 start_ts < 25 的版本，剩下 30 和 40 */
        CHECK_ST(hs.evict_older_than(25));
        CHECK(hs.versions(1, k) == 2);
        {
            Val out;
            bool found = false;
            CHECK_ST(hs.read(1, k, 25, out, found));
            CHECK(!found); /* 20 已被回收 */
        }
        CHECK(hs.size() > 0);
    }

    /* --- 1b. reconcile 写入侧：旧版本从 update 链搬进 HS，最新值不受影响 --- */
    {
        Btree bt;
        CHECK_ST(bt.open(path, small_cfg()));

        CHECK_ST(bt.put(Slice("k1"), Slice("old"), 1, 10));
        CHECK_ST(bt.put(Slice("k1"), Slice("new"), 2, 20));
        CHECK_ST(bt.put(Slice("k2"), Slice("only"), 1, 10));

        Watermark wm;
        wm.stable_ts = 20;
        wm.pinned_ts = 20; /* 没有长事务 → oldest = 20 */

        HistoryStore hs;
        CHECK_ST(reconcile_with_history(bt, hs, wm));
        CHECK(hs.size() > 0); /* k1 的 ts=10 版本被搬走了 */

        {
            Val out;
            CHECK_ST(bt.get(Slice("k1"), out));
            CHECK(sv(out) == "new"); /* 搬走旧版本不影响最新值 */
            CHECK_ST(bt.get(Slice("k2"), out));
            CHECK(sv(out) == "only");
        }

        /* 搬过之后树的读一致性不能被破坏 */
        std::string err;
        CHECK_ST(bt.verify(err));

        CHECK_ST(bt.close());
    }

    rmpath(path);
}

/* ---------------------------------------------------------------- 2. RTS */
static void
test_rollback_to_stable()
{
    CASE("P4/T4.4 未提交 reconcile + RTS");
    const std::string path = dbpath("p4_rts");
    rmpath(path);

    HistoryStore hs;
    Btree bt;
    CHECK_ST(bt.open(path, small_cfg()));

    /* stable 之前的版本：必须留下 */
    CHECK_ST(bt.put(Slice("k1"), Slice("stable"), 1, 10));
    /* stable 之后的版本：模拟「崩溃时还没稳定」的写 */
    CHECK_ST(bt.put(Slice("k1"), Slice("future"), 2, 100));
    /* 只存在于 stable 之后的键：回滚后必须整个消失 */
    CHECK_ST(bt.put(Slice("k2"), Slice("ghost"), 2, 100));

    {
        Val out;
        CHECK_ST(bt.get(Slice("k1"), out));
        CHECK(sv(out) == "future"); /* 回滚前看到的是最新版本 */
    }

    CHECK_ST(rollback_to_stable(bt, hs, 50));

    {
        Val out;
        CHECK_ST(bt.get(Slice("k1"), out));
        CHECK(sv(out) == "stable"); /* 回到 stable 点 */
        CHECK_RC(bt.get(Slice("k2"), out), S_NOTFOUND);
    }

    /* 回滚必须幂等：再来一次不能把 stable 版本也啃掉 */
    CHECK_ST(rollback_to_stable(bt, hs, 50));
    {
        Val out;
        CHECK_ST(bt.get(Slice("k1"), out));
        CHECK(sv(out) == "stable");
    }

    /*
     * 更低的 stable 点：30 号版本的 k3 在 stable 之后 → 摘掉；
     * k1 剩下的 10 号版本 <= 20，必须留着（回滚是「> stable 才回滚」，不是「< 就留」）。
     */
    CHECK_ST(bt.put(Slice("k3"), Slice("v3"), 3, 30));
    CHECK_ST(rollback_to_stable(bt, hs, 20));
    {
        Val out;
        CHECK_RC(bt.get(Slice("k3"), out), S_NOTFOUND);
        CHECK_ST(bt.get(Slice("k1"), out));
        CHECK(sv(out) == "stable");
    }

    /* 空日志/已干净的库上 recovery 是幂等的，且不能吃掉数据 */
    {
        CHECK_ST(bt.put(Slice("k4"), Slice("keep"), 4, 10));
        CHECK_ST(recover_after_crash(bt, hs, 50));
        Val out;
        CHECK_ST(bt.get(Slice("k4"), out));
        CHECK(sv(out) == "keep");
        /* dirty_shutdown 标志没置位时 RTS 不该动数据；置位的路径由 capstone 的 kill -9 流程覆盖 */
        CHECK_ST(recover_after_crash(bt, hs, 50));
    }

    CHECK_ST(bt.close());
    rmpath(path);
}

/* ---------------------------------------------------------------- 3. watermark */
static void
test_watermark()
{
    CASE("P4/T4.5 watermark（stable 点）+ 时间窗口聚合");

    /* --- 3a. oldest = min(stable, pinned) --- */
    {
        Watermark wm;
        wm.stable_ts = 30;
        wm.pinned_ts = 20;
        CHECK(wm.oldest() == 20);

        wm.stable_ts = 10;
        CHECK(wm.oldest() == 10);

        wm.stable_ts = 40;
        wm.pinned_ts = 40;
        CHECK(wm.oldest() == 40);
    }

    /* --- 3b. aggregate_stop_ts：窗口必须覆盖所有成员 --- */
    {
        Page page;
        page.type = PAGE_ROW_LEAF;

        Entry e1;
        e1.key = key_of("a");
        /* 链头是最新的：20，下面还有 10 */
        e1.upd = mk_upd(20, 0, 20, mk_upd(10, 20, 5));
        page.entries.push_back(e1);

        Entry e2;
        e2.key = key_of("b");
        e2.upd = mk_upd(30, 40, 30);
        page.entries.push_back(e2);

        TimeWindow agg;
        CHECK_ST(aggregate_stop_ts(&page, agg));
        CHECK(agg.start_ts == 10);   /* 最小 start：窗口要覆盖最老的版本 */
        CHECK(agg.stop_ts == 40);    /* 最大 stop：要覆盖最晚的失效点 */
        CHECK(agg.durable_ts == 5);  /* 最小 durable：最保守的持久化点 */
    }

    /* --- 3c. 空页：不报错，窗口留 0 --- */
    {
        Page empty;
        empty.type = PAGE_ROW_LEAF;
        TimeWindow agg;
        agg.start_ts = 999;
        CHECK_ST(aggregate_stop_ts(&empty, agg));
        CHECK(agg.start_ts == 0);
        CHECK(agg.stop_ts == 0);
    }

    /* --- 3d. 长事务把水位钉住 → HS 不能回收 --- */
    {
        TxnManager mgr;
        Txn long_reader;
        CHECK_ST(mgr.begin(long_reader, 10)); /* 读时间戳停在 10 的长事务 */

        Watermark wm;
        wm.stable_ts = 50;
        wm.pinned_ts = long_reader.read_ts; /* 最老活跃读者 */
        CHECK(wm.oldest() == 10);

        /* oldest_active 用 id 表示，别和时间戳混用；这里只确认长事务还挂着 */
        CHECK(mgr.oldest_active() == long_reader.id);

        HistoryStore hs;
        const Key k = key_of("blocked");
        TimeWindow tw;
        tw.start_ts = 30;
        tw.durable_ts = 30;
        CHECK_ST(hs.insert(1, k, tw, vv("v30"), false));

        /* 水位被钉在 10 → 30 号版本还不能回收 */
        CHECK_ST(hs.evict_older_than(wm.oldest()));
        CHECK(hs.versions(1, k) == 1);

        /* 长事务结束，水位追上 stable → 可以回收 */
        CHECK_ST(mgr.commit(long_reader, 50));
        wm.pinned_ts = 50;
        CHECK(wm.oldest() == 50);
        CHECK_ST(hs.evict_older_than(wm.oldest()));
        CHECK(hs.versions(1, k) == 0);
    }
}

int
main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("tinywt P4: history store / RTS / watermark\n");
    RUN(test_history_store);
    RUN(test_rollback_to_stable);
    RUN(test_watermark);
    return harness_report();
}
