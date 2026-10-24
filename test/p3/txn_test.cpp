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
TWT_TEST(P3_Txn, ReadOwnWrite, "P3/T3.4 事务内读得到自己的写")
{
    const std::string path = dbpath("p3_txn_own");
    rmpath(path);

    Btree bt;
    ASSERT_EQ(S_OK, bt.open(path, small_cfg())) << "Btree::open 失败";
    TxnManager mgr;
    TxnSession s(bt, mgr);

    ASSERT_EQ(S_OK, s.begin());
    EXPECT_TRUE(s.in_txn());
    EXPECT_NE(u64(0), s.txn().id) << "begin 必须分配 id";
    EXPECT_EQ(TXN_RUNNING, s.txn().state);

    ASSERT_EQ(S_OK, s.put(Slice("k1"), Slice("v1")));
    /* 关键断言：自己的 txnid 一定还在 active_ 里，不特判就会被判成不可见 */
    {
        Val out;
        ASSERT_EQ(S_OK, s.get(Slice("k1"), out)) << "事务读不到自己刚写的值";
        EXPECT_EQ("v1", sv(out));
    }

    /* 覆盖写 + 删除，同样要能立刻看到 */
    ASSERT_EQ(S_OK, s.put(Slice("k1"), Slice("v2")));
    {
        Val out;
        ASSERT_EQ(S_OK, s.get(Slice("k1"), out));
        EXPECT_EQ("v2", sv(out));
    }
    ASSERT_EQ(S_OK, s.put(Slice("k2"), Slice("vv")));
    ASSERT_EQ(S_OK, s.remove(Slice("k2")));
    {
        Val out;
        EXPECT_EQ(S_NOTFOUND, s.get(Slice("k2"), out)) << "自己删掉的键必须立刻读不到";
    }

    EXPECT_EQ(S_OK, s.commit(1));
    EXPECT_EQ(TXN_COMMITTED, s.txn().state);
    EXPECT_FALSE(s.in_txn());

    EXPECT_EQ(S_OK, bt.close());
    rmpath(path);
}

/* ---------------------------------------------------------------- 2. 未提交不可见 */
TWT_TEST(P3_Txn, UncommittedInvisible, "P3/T3.4 未提交的写对别的事务不可见")
{
    const std::string path = dbpath("p3_txn_invis");
    rmpath(path);

    Btree bt;
    ASSERT_EQ(S_OK, bt.open(path, small_cfg()));
    TxnManager mgr;

    TxnSession writer(bt, mgr);
    ASSERT_EQ(S_OK, writer.begin());
    ASSERT_EQ(S_OK, writer.put(Slice("k1"), Slice("v-writer")));

    /* 另一个事务在 writer 还没提交时读 → 必须是 NOTFOUND */
    {
        TxnSession reader(bt, mgr);
        ASSERT_EQ(S_OK, reader.begin());
        Val out;
        EXPECT_EQ(S_NOTFOUND, reader.get(Slice("k1"), out)) << "未提交的写被别的事务读到了";
        EXPECT_EQ(S_OK, reader.commit(1));
    }

    /* 裸 get（无事务上下文）同样不能看到未提交的版本 */
    {
        Val out;
        EXPECT_EQ(S_NOTFOUND, bt.get(Slice("k1"), out))
          << "没有事务上下文时也不能看到未提交版本";
    }

    ASSERT_EQ(S_OK, writer.commit(2));

    /* 提交之后，新开的事务就能看到了 */
    {
        TxnSession reader(bt, mgr);
        ASSERT_EQ(S_OK, reader.begin());
        Val out;
        ASSERT_EQ(S_OK, reader.get(Slice("k1"), out)) << "提交之后新事务应该看得到";
        EXPECT_EQ("v-writer", sv(out));
        EXPECT_EQ(S_OK, reader.commit(3));
    }

    EXPECT_EQ(S_OK, bt.close());
    rmpath(path);
}

/* ---------------------------------------------------------------- 3. 回滚精确摘链 */
TWT_TEST(P3_Txn, RollbackUnlinksExactly, "P3/T3.4 回滚只摘自己的版本（不整条丢链）")
{
    const std::string path = dbpath("p3_txn_rb");
    rmpath(path);

    Btree bt;
    ASSERT_EQ(S_OK, bt.open(path, small_cfg()));
    TxnManager mgr;

    /* 先落一个已提交的基线版本 */
    {
        TxnSession s(bt, mgr);
        ASSERT_EQ(S_OK, s.begin());
        ASSERT_EQ(S_OK, s.put(Slice("k1"), Slice("base")));
        ASSERT_EQ(S_OK, s.commit(1));
    }

    /* 第二个事务改它 + 顺手插一个新键，然后整体回滚 */
    {
        TxnSession s(bt, mgr);
        ASSERT_EQ(S_OK, s.begin());
        ASSERT_EQ(S_OK, s.put(Slice("k1"), Slice("dirty")));
        ASSERT_EQ(S_OK, s.put(Slice("k2"), Slice("dirty2")));
        ASSERT_EQ(S_OK, s.rollback());
        EXPECT_EQ(TXN_ABORTED, s.txn().state);
    }

    /* k1 必须回到 base —— 而不是变成 NOTFOUND（那说明整条链被丢了） */
    {
        Val out;
        ASSERT_EQ(S_OK, bt.get(Slice("k1"), out)) << "回滚把整条链丢了（应该回到 base）";
        EXPECT_EQ("base", sv(out));
    }
    /* k2 本来就不在磁盘镜像里（insert 跳表节点），回滚后节点也要消失 */
    {
        Val out;
        EXPECT_EQ(S_NOTFOUND, bt.get(Slice("k2"), out)) << "回滚没有摘掉 insert 跳表里的节点";
    }

    /* 多版本叠三层，摘中间那层 */
    {
        TxnSession a(bt, mgr);
        ASSERT_EQ(S_OK, a.begin());
        ASSERT_EQ(S_OK, a.put(Slice("k1"), Slice("v2")));
        ASSERT_EQ(S_OK, a.commit(2));

        TxnSession b(bt, mgr);
        ASSERT_EQ(S_OK, b.begin());
        ASSERT_EQ(S_OK, b.put(Slice("k1"), Slice("v3")));
        ASSERT_EQ(S_OK, b.commit(3));

        TxnSession c(bt, mgr);
        ASSERT_EQ(S_OK, c.begin());
        ASSERT_EQ(S_OK, c.put(Slice("k1"), Slice("v4")));
        ASSERT_EQ(S_OK, c.rollback());
    }
    {
        Val out;
        ASSERT_EQ(S_OK, bt.get(Slice("k1"), out));
        EXPECT_EQ("v3", sv(out)) << "回滚应该露出上一层版本 v3";
    }

    EXPECT_EQ(S_OK, bt.close());
    rmpath(path);
}

/* ---------------------------------------------------------------- 4. 快照隔离 */
TWT_TEST(P3_Txn, SnapshotIsolation, "P3/T3.4 快照隔离（begin 之后提交的看不到）")
{
    const std::string path = dbpath("p3_txn_snap");
    rmpath(path);

    Btree bt;
    ASSERT_EQ(S_OK, bt.open(path, small_cfg()));
    TxnManager mgr;

    TxnSession long_reader(bt, mgr);
    ASSERT_EQ(S_OK, long_reader.begin(10)); /* 长事务先开，快照在这一刻定格 */
    const u64 reader_id = long_reader.txn().id;

    /* 后开的事务写并提交 */
    {
        TxnSession w(bt, mgr);
        ASSERT_EQ(S_OK, w.begin(10));
        ASSERT_EQ(S_OK, w.put(Slice("k1"), Slice("late")));
        ASSERT_EQ(S_OK, w.commit(20));
    }

    /* 长事务必须看不到 —— 这是快照隔离的定义，不是 bug */
    {
        Val out;
        EXPECT_EQ(S_NOTFOUND, long_reader.get(Slice("k1"), out))
          << "长事务看到了自己快照之后才提交的版本";
    }

    /*
     * 但「现在才开始」的事务看得到：说明上面那条不是数据没写进去，
     * 而是真的按快照判的。
     */
    {
        TxnSession fresh(bt, mgr);
        ASSERT_EQ(S_OK, fresh.begin(30));
        Val out;
        ASSERT_EQ(S_OK, fresh.get(Slice("k1"), out)) << "新事务应该看得到已提交的 late";
        EXPECT_EQ("late", sv(out));
        EXPECT_EQ(S_OK, fresh.commit(31));
    }

    /* 长事务还挂着 → oldest_active 必须停在它身上，不能被推进 */
    EXPECT_EQ(reader_id, mgr.oldest_active()) << "有长事务挂着时 oldest_active 不能前进";

    ASSERT_EQ(S_OK, long_reader.commit(32));
    /* 释放之后 oldest 追上来（没有活跃事务时返回 next_id，即「无」） */
    EXPECT_TRUE(mgr.oldest_active() >= mgr.next_id() || mgr.oldest_active() == 0);

    EXPECT_EQ(S_OK, bt.close());
    rmpath(path);
}

/* ---------------------------------------------------------------- 5. 写冲突 */
TWT_TEST(P3_Txn, WriteConflict, "P3/T3.4 写写冲突（未提交版本挡住后来者）")
{
    const std::string path = dbpath("p3_txn_conf");
    rmpath(path);

    Btree bt;
    ASSERT_EQ(S_OK, bt.open(path, small_cfg()));
    TxnManager mgr;

    TxnSession a(bt, mgr);
    ASSERT_EQ(S_OK, a.begin());
    ASSERT_EQ(S_OK, a.put(Slice("k1"), Slice("from-a")));

    /* a 还没提交，b 来改同一个键 → 必须 S_ROLLBACK，而不是默默覆盖 */
    {
        TxnSession b(bt, mgr);
        ASSERT_EQ(S_OK, b.begin());
        EXPECT_EQ(S_ROLLBACK, b.put(Slice("k1"), Slice("from-b")))
          << "未提交版本必须挡住后来者（不能默默覆盖）";
        EXPECT_EQ(S_OK, b.rollback());
    }

    /* 不同键不冲突 */
    {
        TxnSession b(bt, mgr);
        ASSERT_EQ(S_OK, b.begin());
        EXPECT_EQ(S_OK, b.put(Slice("k2"), Slice("from-b")));
        EXPECT_EQ(S_OK, b.commit(1));
    }

    ASSERT_EQ(S_OK, a.commit(2));

    /* a 提交之后，新事务再改同一个键就没有冲突了 */
    {
        TxnSession c(bt, mgr);
        ASSERT_EQ(S_OK, c.begin());
        EXPECT_EQ(S_OK, c.put(Slice("k1"), Slice("from-c")));
        EXPECT_EQ(S_OK, c.commit(3));
    }

    /* insert 语义：键已存在 → S_DUPLICATE（不是 S_ROLLBACK） */
    {
        TxnSession d(bt, mgr);
        ASSERT_EQ(S_OK, d.begin());
        EXPECT_EQ(S_DUPLICATE, d.insert(Slice("k1"), Slice("dup")))
          << "insert 已存在的键必须是 DUPLICATE，不是 ROLLBACK";
        EXPECT_EQ(S_OK, d.insert(Slice("k3"), Slice("new")));
        EXPECT_EQ(S_OK, d.commit(4));
    }
    {
        Val out;
        ASSERT_EQ(S_OK, bt.get(Slice("k1"), out));
        EXPECT_EQ("from-c", sv(out));
        ASSERT_EQ(S_OK, bt.get(Slice("k3"), out));
        EXPECT_EQ("new", sv(out));
    }

    EXPECT_EQ(S_OK, bt.close());
    rmpath(path);
}
