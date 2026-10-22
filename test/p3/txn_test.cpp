/*-
 * P3/T3.4 事务 API + 增量操作 —— 对标 BusTub P3 剩下的 executor + P4 的事务入口
 *
 * 这一项只测「单线程、显式事务」这一层，先把语义钉死：
 *   begin / commit / rollback 的状态机，以及快照隔离最朴素的那条规则。
 * 真正的时间戳水位、History Store、RTS 都在 P4（T4.3–T4.5）。
 *
 * 为什么先把这一层单独测：
 *   P4 的 HS / RTS 全部建立在「visible() 是对的」这个前提上。visible() 写错，
 *   后面所有测试都会以「值莫名其妙不对」的形式失败 —— 定位成本极高。
 *
 * 对标真 WT：
 *   src/txn/txn.c             __wt_txn_begin / __wt_txn_commit / __wt_txn_rollback
 *   src/include/txn_inline.h  __wt_txn_visible
 *   src/btree/row_modify.c    __wt_row_modify（挂 update 链）
 *
 * 评分点（每项一个测试函数）：
 *   1. 读自己的写              → 自己的 txnid 不能把自己挡在外面
 *   2. 未提交对别人不可见       → 提交之后新事务才看得到
 *   3. 回滚精确摘链            → 只摘自己那个版本，不整条丢
 *   4. 快照隔离                → begin 之后别人提交的，本事务看不到
 *   5. 写冲突                  → 别的事务留着未提交版本时，后写者 S_ROLLBACK
 *
 * ★ 回滚（第 3 项）是这里最容易写错的：
 *   链头可能是别人的版本，也可能这个键根本不在磁盘镜像里（只在 insert 跳表）。
 *   两种情况的摘法不同，且都要把 ninserts 之类的计数修回去。
 * -*/
#define TWT_PROJECT "P3"

#include <string>
#include <vector>

#include "support/harness.hpp"
#include "tinywt/btree/btree.h"
#include "tinywt/txn/txn.h"

namespace {

using namespace twt;

/* 字符串 → Val（Key 和 Val 都是 vector<u8>，可以直接比） */
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

/*
 * 故意把页做小：max_leaf_entries=4 让几乎每次插入都触发 split，
 * 这样回滚路径会跨页、会碰到内部页重新挂 Ref 的情况，而不是只在一个叶子里打转。
 */
Config
small_cfg()
{
    Config c;
    c.max_leaf_entries = 4;
    c.max_intl_refs = 4;
    c.max_cached_pages = 8;
    c.alloc_size = 512;
    return c;
}

} /* namespace */

/* ---------------------------------------------------------------- 1. 读自己的写 */
static void
test_read_own_write()
{
    CASE("P3/T3.4 事务内读得到自己的写");
    const std::string path = dbpath("p3_txn_own");
    rmpath(path);

    Btree bt;
    CHECK_ST(bt.open(path, small_cfg()));
    TxnManager mgr;
    TxnSession s(bt, mgr);

    CHECK_ST(s.begin());
    CHECK(s.in_txn());
    CHECK(s.txn().id != 0);  /* begin 必须分配 id */
    CHECK(s.txn().state == TXN_RUNNING);

    CHECK_ST(s.put(Slice("k1"), Slice("v1")));
    /* 关键断言：自己的 txnid 一定还在 active_ 里，不特判就会被判成不可见 */
    {
        Val out;
        CHECK_ST(s.get(Slice("k1"), out));
        CHECK(sv(out) == "v1");
    }

    /* 覆盖写 + 删除，同样要能立刻看到 */
    CHECK_ST(s.put(Slice("k1"), Slice("v2")));
    {
        Val out;
        CHECK_ST(s.get(Slice("k1"), out));
        CHECK(sv(out) == "v2");
    }
    CHECK_ST(s.put(Slice("k2"), Slice("vv")));
    CHECK_ST(s.remove(Slice("k2")));
    {
        Val out;
        CHECK_RC(s.get(Slice("k2"), out), S_NOTFOUND);
    }

    CHECK_ST(s.commit(1));
    CHECK(s.txn().state == TXN_COMMITTED);
    CHECK(!s.in_txn());

    CHECK_ST(bt.close());
    rmpath(path);
}

/* ---------------------------------------------------------------- 2. 未提交不可见 */
static void
test_uncommitted_invisible()
{
    CASE("P3/T3.4 未提交的写对别的事务不可见");
    const std::string path = dbpath("p3_txn_invis");
    rmpath(path);

    Btree bt;
    CHECK_ST(bt.open(path, small_cfg()));
    TxnManager mgr;

    TxnSession writer(bt, mgr);
    CHECK_ST(writer.begin());
    CHECK_ST(writer.put(Slice("k1"), Slice("v-writer")));

    /* 另一个事务在 writer 还没提交时读 → 必须是 NOTFOUND */
    {
        TxnSession reader(bt, mgr);
        CHECK_ST(reader.begin());
        Val out;
        CHECK_RC(reader.get(Slice("k1"), out), S_NOTFOUND);
        CHECK_ST(reader.commit(1));
    }

    /* 裸 get（无事务上下文）同样不能看到未提交的版本 */
    {
        Val out;
        CHECK_RC(bt.get(Slice("k1"), out), S_NOTFOUND);
    }

    CHECK_ST(writer.commit(2));

    /* 提交之后，新开的事务就能看到了 */
    {
        TxnSession reader(bt, mgr);
        CHECK_ST(reader.begin());
        Val out;
        CHECK_ST(reader.get(Slice("k1"), out));
        CHECK(sv(out) == "v-writer");
        CHECK_ST(reader.commit(3));
    }

    CHECK_ST(bt.close());
    rmpath(path);
}

/* ---------------------------------------------------------------- 3. 回滚精确摘链 */
static void
test_rollback_unlinks_exactly()
{
    CASE("P3/T3.4 回滚只摘自己的版本（不整条丢链）");
    const std::string path = dbpath("p3_txn_rb");
    rmpath(path);

    Btree bt;
    CHECK_ST(bt.open(path, small_cfg()));
    TxnManager mgr;

    /* 先落一个已提交的基线版本 */
    {
        TxnSession s(bt, mgr);
        CHECK_ST(s.begin());
        CHECK_ST(s.put(Slice("k1"), Slice("base")));
        CHECK_ST(s.commit(1));
    }

    /* 第二个事务改它 + 顺手插一个新键，然后整体回滚 */
    {
        TxnSession s(bt, mgr);
        CHECK_ST(s.begin());
        CHECK_ST(s.put(Slice("k1"), Slice("dirty")));
        CHECK_ST(s.put(Slice("k2"), Slice("dirty2")));
        CHECK_ST(s.rollback());
        CHECK(s.txn().state == TXN_ABORTED);
    }

    /* k1 必须回到 base —— 而不是变成 NOTFOUND（那说明整条链被丢了） */
    {
        Val out;
        CHECK_ST(bt.get(Slice("k1"), out));
        CHECK(sv(out) == "base");
    }
    /* k2 本来就不在磁盘镜像里（insert 跳表节点），回滚后节点也要消失 */
    {
        Val out;
        CHECK_RC(bt.get(Slice("k2"), out), S_NOTFOUND);
    }

    /* 多版本叠三层，摘中间那层 */
    {
        TxnSession a(bt, mgr);
        CHECK_ST(a.begin());
        CHECK_ST(a.put(Slice("k1"), Slice("v2")));
        CHECK_ST(a.commit(2));

        TxnSession b(bt, mgr);
        CHECK_ST(b.begin());
        CHECK_ST(b.put(Slice("k1"), Slice("v3")));
        CHECK_ST(b.commit(3));

        TxnSession c(bt, mgr);
        CHECK_ST(c.begin());
        CHECK_ST(c.put(Slice("k1"), Slice("v4")));
        CHECK_ST(c.rollback());
    }
    {
        Val out;
        CHECK_ST(bt.get(Slice("k1"), out));
        CHECK(sv(out) == "v3");
    }

    CHECK_ST(bt.close());
    rmpath(path);
}

/* ---------------------------------------------------------------- 4. 快照隔离 */
static void
test_snapshot_isolation()
{
    CASE("P3/T3.4 快照隔离（begin 之后提交的看不到）");
    const std::string path = dbpath("p3_txn_snap");
    rmpath(path);

    Btree bt;
    CHECK_ST(bt.open(path, small_cfg()));
    TxnManager mgr;

    TxnSession long_reader(bt, mgr);
    CHECK_ST(long_reader.begin(10)); /* 长事务先开，快照在这一刻定格 */
    const u64 reader_id = long_reader.txn().id;

    /* 后开的事务写并提交 */
    {
        TxnSession w(bt, mgr);
        CHECK_ST(w.begin(10));
        CHECK_ST(w.put(Slice("k1"), Slice("late")));
        CHECK_ST(w.commit(20));
    }

    /* 长事务必须看不到 —— 这是快照隔离的定义，不是 bug */
    {
        Val out;
        CHECK_RC(long_reader.get(Slice("k1"), out), S_NOTFOUND);
    }

    /*
     * 但「现在才开始」的事务看得到：说明上面那条不是数据没写进去，
     * 而是真的按快照判的。
     */
    {
        TxnSession fresh(bt, mgr);
        CHECK_ST(fresh.begin(30));
        Val out;
        CHECK_ST(fresh.get(Slice("k1"), out));
        CHECK(sv(out) == "late");
        CHECK_ST(fresh.commit(31));
    }

    /* 长事务还挂着 → oldest_active 必须停在它身上，不能被推进 */
    CHECK(mgr.oldest_active() == reader_id);

    CHECK_ST(long_reader.commit(32));
    /* 释放之后 oldest 追上来（没有活跃事务时返回 next_id，即「无」） */
    CHECK(mgr.oldest_active() >= mgr.next_id() || mgr.oldest_active() == 0);

    CHECK_ST(bt.close());
    rmpath(path);
}

/* ---------------------------------------------------------------- 5. 写冲突 */
static void
test_write_conflict()
{
    CASE("P3/T3.4 写写冲突（未提交版本挡住后来者）");
    const std::string path = dbpath("p3_txn_conf");
    rmpath(path);

    Btree bt;
    CHECK_ST(bt.open(path, small_cfg()));
    TxnManager mgr;

    TxnSession a(bt, mgr);
    CHECK_ST(a.begin());
    CHECK_ST(a.put(Slice("k1"), Slice("from-a")));

    /* a 还没提交，b 来改同一个键 → 必须 S_ROLLBACK，而不是默默覆盖 */
    {
        TxnSession b(bt, mgr);
        CHECK_ST(b.begin());
        CHECK_RC(b.put(Slice("k1"), Slice("from-b")), S_ROLLBACK);
        CHECK_ST(b.rollback());
    }

    /* 不同键不冲突 */
    {
        TxnSession b(bt, mgr);
        CHECK_ST(b.begin());
        CHECK_ST(b.put(Slice("k2"), Slice("from-b")));
        CHECK_ST(b.commit(1));
    }

    CHECK_ST(a.commit(2));

    /* a 提交之后，新事务再改同一个键就没有冲突了 */
    {
        TxnSession c(bt, mgr);
        CHECK_ST(c.begin());
        CHECK_ST(c.put(Slice("k1"), Slice("from-c")));
        CHECK_ST(c.commit(3));
    }

    /* insert 语义：键已存在 → S_DUPLICATE（不是 S_ROLLBACK） */
    {
        TxnSession d(bt, mgr);
        CHECK_ST(d.begin());
        CHECK_RC(d.insert(Slice("k1"), Slice("dup")), S_DUPLICATE);
        CHECK_ST(d.insert(Slice("k3"), Slice("new")));
        CHECK_ST(d.commit(4));
    }
    {
        Val out;
        CHECK_ST(bt.get(Slice("k1"), out));
        CHECK(sv(out) == "from-c");
        CHECK_ST(bt.get(Slice("k3"), out));
        CHECK(sv(out) == "new");
    }

    CHECK_ST(bt.close());
    rmpath(path);
}

int
main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("tinywt P3: transaction API\n");
    RUN(test_read_own_write);
    RUN(test_uncommitted_invisible);
    RUN(test_rollback_unlinks_exactly);
    RUN(test_snapshot_isolation);
    RUN(test_write_conflict);
    return harness_report();
}
