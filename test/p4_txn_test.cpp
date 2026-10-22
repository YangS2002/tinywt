/*-
 * P4/T4.1–T4.4 事务、快照隔离、回滚、History Store、RTS
 * 对标 BusTub P4: Concurrency Control / MVCC
 *
 * 这一项的评分点是「隔离级别」而不是「能不能读出来」：
 *   同一个 key 在两个事务眼里必须可以是两个值，取决于它俩的开始时刻。
 *
 * 测试里的时间戳都是显式给的（read_ts / commit_ts），所以结果是确定的，
 * 不依赖真实时钟 —— 这一点很重要：靠 sleep 的并发测试既慢又不稳。
 * -*/
#define TWT_PROJECT "P4"

#include <string>
#include <vector>

#include "harness.hpp"
#include "twt_txn.h"

using namespace twt;

static void
test_txn_basic()
{
    CASE("P4/T4.1 事务内读写 + commit 后对新人可见");
    std::string p = dbpath("p4_basic.wt");
    rmpath(p);

    Btree bt;
    Config cfg;
    cfg.max_leaf_entries = 16;
    CHECK_ST(bt.open(p, cfg));

    TxnManager mgr;
    TxnSession s1(bt, mgr);

    CHECK_ST(s1.begin(/*read_ts=*/10));
    CHECK_ST(s1.put(Slice("k"), Slice("v1")));
    /* 事务内必须读得到自己刚写的值（读自己的写） */
    {
        Val v;
        CHECK_ST(s1.get(Slice("k"), v));
        CHECK(Slice(v.data(), v.size()).cmp(Slice("v1")) == 0);
    }
    /* 未提交时，另一个事务看不到 */
    {
        TxnSession s2(bt, mgr);
        CHECK_ST(s2.begin(11));
        Val v;
        CHECK_RC(s2.get(Slice("k"), v), S_NOTFOUND);
        CHECK_ST(s2.commit(11));
    }
    CHECK_ST(s1.commit(/*commit_ts=*/12));

    /* 提交之后新人可见 */
    {
        TxnSession s3(bt, mgr);
        CHECK_ST(s3.begin(13));
        Val v;
        CHECK_ST(s3.get(Slice("k"), v));
        CHECK(Slice(v.data(), v.size()).cmp(Slice("v1")) == 0);
        CHECK_ST(s3.commit(13));
    }

    CHECK_ST(bt.close());
    rmpath(p);
}

static void
test_rollback()
{
    CASE("P4/T4.1 rollback 撤销本事务的全部修改");
    std::string p = dbpath("p4_rb.wt");
    rmpath(p);

    Btree bt;
    CHECK_ST(bt.open(p, Config{}));

    /* 先落一个已提交的基线值 */
    TxnManager mgr;
    {
        TxnSession base(bt, mgr);
        CHECK_ST(base.begin(1));
        CHECK_ST(base.put(Slice("a"), Slice("base")));
        CHECK_ST(base.put(Slice("b"), Slice("base")));
        CHECK_ST(base.commit(2));
    }

    /* 事务改 a、删 b、插 c，然后回滚 */
    {
        TxnSession t(bt, mgr);
        CHECK_ST(t.begin(3));
        CHECK_ST(t.put(Slice("a"), Slice("changed")));
        CHECK_ST(t.remove(Slice("b")));
        CHECK_ST(t.put(Slice("c"), Slice("new")));
        CHECK_ST(t.rollback());
    }

    /* 全部回到基线：a=base，b=base，c 不存在 */
    {
        TxnSession r(bt, mgr);
        CHECK_ST(r.begin(4));
        Val v;
        CHECK_ST(r.get(Slice("a"), v));
        CHECK(Slice(v.data(), v.size()).cmp(Slice("base")) == 0);
        CHECK_ST(r.get(Slice("b"), v));
        CHECK(Slice(v.data(), v.size()).cmp(Slice("base")) == 0);
        CHECK_RC(r.get(Slice("c"), v), S_NOTFOUND);
        CHECK_ST(r.commit(4));
    }

    CHECK_ST(bt.close());
    rmpath(p);
}

static void
test_snapshot_isolation()
{
    CASE("P4/T4.1 快照隔离：旧事务看不到新提交");
    std::string p = dbpath("p4_snap.wt");
    rmpath(p);

    Btree bt;
    CHECK_ST(bt.open(p, Config{}));

    TxnManager mgr;
    {
        TxnSession w(bt, mgr);
        CHECK_ST(w.begin(1));
        CHECK_ST(w.put(Slice("k"), Slice("v1")));
        CHECK_ST(w.commit(2));
    }

    /* 长事务：在 v2 提交之前就开始了 */
    TxnSession long_txn(bt, mgr);
    CHECK_ST(long_txn.begin(/*read_ts=*/3));

    {
        TxnSession w(bt, mgr);
        CHECK_ST(w.begin(4));
        CHECK_ST(w.put(Slice("k"), Slice("v2")));
        CHECK_ST(w.commit(/*commit_ts=*/5));
    }

    /* 长事务读到的仍然是 v1 —— 这是快照隔离的全部意义 */
    {
        Val v;
        CHECK_ST(long_txn.get(Slice("k"), v));
        CHECK(Slice(v.data(), v.size()).cmp(Slice("v1")) == 0);
    }
    CHECK_ST(long_txn.commit(6));

    /* 新事务看到 v2 */
    {
        TxnSession n(bt, mgr);
        CHECK_ST(n.begin(7));
        Val v;
        CHECK_ST(n.get(Slice("k"), v));
        CHECK(Slice(v.data(), v.size()).cmp(Slice("v2")) == 0);
        CHECK_ST(n.commit(7));
    }

    CHECK_ST(bt.close());
    rmpath(p);
}

static void
test_write_conflict()
{
    CASE("P4/T4.2 写写冲突：后写者拿到 S_ROLLBACK");
    std::string p = dbpath("p4_conflict.wt");
    rmpath(p);

    Btree bt;
    CHECK_ST(bt.open(p, Config{}));
    TxnManager mgr;

    TxnSession a(bt, mgr), b(bt, mgr);
    CHECK_ST(a.begin(1));
    CHECK_ST(b.begin(1));

    CHECK_ST(a.put(Slice("k"), Slice("from-a")));
    /* b 写同一个键：a 还没提交 → 冲突 */
    CHECK_RC(b.put(Slice("k"), Slice("from-b")), S_ROLLBACK);

    CHECK_ST(a.commit(2));
    CHECK_RC(b.rollback(), S_OK);

    CHECK_ST(bt.close());
    rmpath(p);
}

static void
test_history_store()
{
    CASE("P4/T4.3 History Store：老版本挪走后仍能按旧快照读");
    std::string p = dbpath("p4_hs.wt");
    rmpath(p);

    Btree bt;
    CHECK_ST(bt.open(p, Config{}));

    HistoryStore hs;
    /* 一个版本链：ts=10 v1，ts=20 v2，ts=30 v3 */
    {
        Update u1;
        u1.txnid = 1;
        u1.start_ts = 10;
        u1.type = UPD_STANDARD;
        u1.data = {'v', '1'};
        CHECK_ST(hs.insert(key_of("k"), u1));

        Update u2;
        u2.txnid = 2;
        u2.start_ts = 20;
        u2.type = UPD_STANDARD;
        u2.data = {'v', '2'};
        CHECK_ST(hs.insert(key_of("k"), u2));

        Update u3;
        u3.txnid = 3;
        u3.start_ts = 30;
        u3.type = UPD_STANDARD;
        u3.data = {'v', '3'};
        CHECK_ST(hs.insert(key_of("k"), u3));
    }
    CHECK(hs.versions(key_of("k")) == 3);

    /* 按时间戳回查：ts=25 应该看到 v2 */
    {
        Val v;
        bool found = false;
        CHECK_ST(hs.read(key_of("k"), 25, v, found));
        CHECK(found);
        CHECK(Slice(v.data(), v.size()).cmp(Slice("v2")) == 0);
    }
    /* ts=5 早于所有版本 → 查不到 */
    {
        Val v;
        bool found = true;
        CHECK_ST(hs.read(key_of("k"), 5, v, found));
        CHECK(!found);
    }
    /* 回收 ts < 15 的版本 → 只剩 2 条 */
    CHECK_ST(hs.evict_older_than(15));
    CHECK(hs.versions(key_of("k")) == 2);

    CHECK_ST(bt.close());
    rmpath(p);
}

static void
test_rollback_to_stable()
{
    CASE("P4/T4.4 RTS：stable_ts 之后的修改被回滚");
    std::string p = dbpath("p4_rts.wt");
    rmpath(p);

    Btree bt;
    CHECK_ST(bt.open(p, Config{}));

    /* ts=10 写 A（稳定），ts=20 写 B（不稳定） */
    CHECK_ST(bt.put(Slice("a"), Slice("stable"), 1, 10));
    CHECK_ST(bt.put(Slice("b"), Slice("unstable"), 2, 20));

    CHECK_ST(rollback_to_stable(bt, /*stable_ts=*/10));

    Val v;
    CHECK_ST(bt.get(Slice("a"), v));
    CHECK(Slice(v.data(), v.size()).cmp(Slice("stable")) == 0);
    CHECK_RC(bt.get(Slice("b"), v), S_NOTFOUND);

    CHECK_ST(bt.close());
    rmpath(p);
}

int
main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("tinywt P4: transactions / MVCC / HS / RTS\n");
    RUN(test_txn_basic);
    RUN(test_rollback);
    RUN(test_snapshot_isolation);
    RUN(test_write_conflict);
    RUN(test_history_store);
    RUN(test_rollback_to_stable);
    return harness_report();
}
