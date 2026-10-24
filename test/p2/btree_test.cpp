/*-
 * P2 —— Row-Store B-Tree（对标 BusTub P2 B+Tree Index）
 *
 *   Task 2.1  内存页三件套（entries / update 链 / insert 跳表）+ page_in
 *   Task 2.2  descend 二分下降                 ← BusTub Checkpoint 1: Search
 *   Task 2.3  插入 + 叶/内部分裂               ← BusTub Checkpoint 2: Insert
 *   Task 2.4  reconcile + LRU eviction
 *   Task 2.5  checkpoint + 重开恢复 + verify + 读可见性
 * -*/
#include <map>
#include <random>
#include <string>

#include "support/harness.hpp"
#include "tinywt/btree/btree.h"

using namespace twt;

/* ---------------------------------------------------------------- T2.1 */
TWT_TEST(P2_Btree, Basic, "P2/T2.1 内存页三件套 + page_in")
{
    std::string p = dbpath("p2_basic.wt");
    rmpath(p);

    Btree t;
    Config cfg;
    cfg.max_leaf_entries = 8;
    ASSERT_EQ(S_OK, t.open(p, cfg)) << "Btree::open 失败";

    ASSERT_EQ(S_OK, t.put(Slice("a"), Slice("1")));
    ASSERT_EQ(S_OK, t.put(Slice("b"), Slice("2")));

    Val v;
    ASSERT_EQ(S_OK, t.get(Slice("a"), v));
    EXPECT_TRUE(v.size() == 1 && v[0] == '1') << "a 的值不对";
    ASSERT_EQ(S_OK, t.get(Slice("b"), v));
    EXPECT_TRUE(v.size() == 1 && v[0] == '2') << "b 的值不对";
    EXPECT_EQ(S_NOTFOUND, t.get(Slice("zzz"), v)) << "不存在的键必须返回 NOTFOUND";

    /* 覆盖写 */
    ASSERT_EQ(S_OK, t.put(Slice("a"), Slice("111")));
    ASSERT_EQ(S_OK, t.get(Slice("a"), v));
    EXPECT_EQ(size_t(3), v.size()) << "覆盖写之后应读到新值";

    /* 删除 */
    ASSERT_EQ(S_OK, t.remove(Slice("a")));
    EXPECT_EQ(S_NOTFOUND, t.get(Slice("a"), v)) << "删除后必须读不到";

    EXPECT_EQ(S_OK, t.close());
    rmpath(p);
}

/* ---------------------------------------------------------------- T2.2 */
TWT_TEST(P2_Btree, Descend, "P2/T2.2 descend（5000 键回读）")
{
    std::string p = dbpath("p2_split.wt");
    rmpath(p);

    Btree t;
    Config cfg;
    cfg.max_leaf_entries = 16;
    cfg.max_intl_refs = 8;
    cfg.max_cached_pages = 64;
    ASSERT_EQ(S_OK, t.open(p, cfg)) << "Btree::open 失败";

    const int N = 5000;
    for (int i = 0; i < N; ++i) {
        std::string k = kstr(i), v = vstr(i);
        ASSERT_EQ(S_OK, t.put(Slice(k), Slice(v))) << "第 " << i << " 个键写失败";
    }
    std::printf("  splits=%llu writes=%llu evictions=%llu\n",
      (unsigned long long)t.stats().splits, (unsigned long long)t.stats().page_writes,
      (unsigned long long)t.stats().evictions);
    EXPECT_GT(t.stats().splits, 0u) << "5000 键必须撑出分裂，否则这条测试没测到东西";

    int bad = 0;
    for (int i = 0; i < N; ++i) {
        std::string k = kstr(i), want = vstr(i);
        Val v;
        Status s = t.get(Slice(k), v);
        if (s != S_OK || Slice(v.data(), v.size()).cmp(Slice(want)) != 0) {
            if (bad == 0)
                std::printf("  FAIL key=%s status=%s\n", k.c_str(), status_str(s));
            if (++bad >= 5)
                break;
        }
    }
    EXPECT_EQ(0, bad) << "5000 键回读有 " << bad << " 个不对";
    ASSERT_EQ(S_OK, t.checkpoint());
    std::string err;
    ASSERT_EQ(S_OK, t.verify(err)) << "verify: " << err;
    std::printf("  verify: %s\n", err.c_str());
    EXPECT_EQ(S_OK, t.close());
    rmpath(p);
}

/* ---------------------------------------------------------------- T2.3 评分器：对拍 std::map */

/*
 * 一组随机操作跑完，和 std::map 的终态逐键对拍。
 * 返回 0 = 全对；非 0 = 出错的次数（-1 表示连 open 都没成功）。
 *
 * ★ 这里**不**打断言：GoogleTest 的 EXPECT_* 只能用在 void 测例体里，
 *   辅助函数返回"错了几处"，由调用方统一 ASSERT。这样 4 组种子各占一条断言，
 *   失败信息也不会淹掉输出。
 */
static int
diff_run(unsigned seed, size_t cached, const char *tag)
{
    std::printf("\n--- P2/T2.3 对拍 std::map：%s\n", tag);

    std::string p = dbpath("p2_diff.wt");
    rmpath(p);

    Btree t;
    Config cfg;
    cfg.max_leaf_entries = 12;
    cfg.max_intl_refs = 6;
    cfg.max_cached_pages = cached;
    if (t.open(p, cfg) != S_OK)
        return -1;

    std::map<std::string, std::string> ref;
    std::mt19937 rng(seed);
    const int OPS = 3000;
    const int KEYSPACE = 1200;
    int bad = 0;

    for (int i = 0; i < OPS && bad == 0; ++i) {
        int op = rng() % 100;
        int ki = (int)(rng() % KEYSPACE);
        std::string k = kstr(ki);

        if (op < 60) {
            std::string v = vstr((int)rng());
            Status s = t.put(Slice(k), Slice(v));
            if (s != S_OK) {
                std::printf("  FAIL put %s: %s\n", k.c_str(), status_str(s));
                ++bad;
                break;
            }
            ref[k] = v;
        } else if (op < 80) {
            Status s = t.remove(Slice(k));
            if (s != S_OK) {
                std::printf("  FAIL remove %s: %s\n", k.c_str(), status_str(s));
                ++bad;
                break;
            }
            ref.erase(k);
        } else {
            Val v;
            Status s = t.get(Slice(k), v);
            auto it = ref.find(k);
            bool expect_found = (it != ref.end());
            bool got_found = (s == S_OK);
            if (expect_found != got_found) {
                std::printf("  FAIL get %s: expect_found=%d got=%s\n", k.c_str(), (int)expect_found,
                  status_str(s));
                ++bad;
                break;
            }
            if (expect_found && Slice(v.data(), v.size()).cmp(Slice(it->second)) != 0) {
                std::printf("  FAIL value mismatch %s\n", k.c_str());
                ++bad;
                break;
            }
        }
    }

    /* 全量终态比对 */
    int mismatch = 0;
    for (const auto &kv : ref) {
        Val v;
        Status s = t.get(Slice(kv.first), v);
        bool is_bad = (s != S_OK) || (Slice(v.data(), v.size()).cmp(Slice(kv.second)) != 0);
        if (is_bad) {
            ++mismatch;
            if (mismatch <= 5)
                std::printf("  MISMATCH key=%s status=%s got=%.*s want=%s\n", kv.first.c_str(),
                  status_str(s), (int)v.size(), (const char *)v.data(), kv.second.c_str());
        }
    }
    if (bad == 0)
        bad = mismatch;
    std::printf("  ref size=%zu mismatch=%d\n", ref.size(), mismatch);

    if (bad == 0 && t.checkpoint() != S_OK)
        bad = 1;
    std::string err;
    if (bad == 0 && t.verify(err) != S_OK)
        bad = 1;
    std::printf("  verify: %s\n", err.c_str());
    if (bad == 0 && t.close() != S_OK)
        bad = 1;
    rmpath(p);
    return bad;
}

TWT_TEST(P2_Btree, SplitVsStdMap, "P2/T2.3 分裂 + 对拍 std::map（4 组种子 × cache 压力）")
{
    /* 多组种子 × 两种 cache 压力（40 页会疯狂淘汰，1000 页基本不淘汰） */
    EXPECT_EQ(0, diff_run(20261020, 40, "#1 cache=40 seed=20261020"));
    EXPECT_EQ(0, diff_run(1, 40, "#2 cache=40 seed=1"));
    EXPECT_EQ(0, diff_run(2, 1000, "#3 cache=1000 seed=2"));
    EXPECT_EQ(0, diff_run(3, 1000, "#4 cache=1000 seed=3"));
}

/* ---------------------------------------------------------------- T2.5 */
TWT_TEST(P2_Btree, Persistence, "P2/T2.5 checkpoint + 重开 + verify")
{
    std::string p = dbpath("p2_persist.wt");
    rmpath(p);

    const int N = 800;
    {
        Btree t;
        Config cfg;
        cfg.max_leaf_entries = 10;
        cfg.max_cached_pages = 24;
        ASSERT_EQ(S_OK, t.open(p, cfg));
        for (int i = 0; i < N; ++i) {
            std::string k = kstr(i), v = vstr(i);
            ASSERT_EQ(S_OK, t.put(Slice(k), Slice(v))) << "第 " << i << " 个键写失败";
        }
        /* 删掉一部分，验证 tombstone 也能持久化 */
        for (int i = 0; i < N; i += 7)
            ASSERT_EQ(S_OK, t.remove(Slice(kstr(i))));
        ASSERT_EQ(S_OK, t.checkpoint());
        std::string err;
        ASSERT_EQ(S_OK, t.verify(err)) << "verify: " << err;
        ASSERT_EQ(S_OK, t.close());
    }

    {
        Btree t;
        Config cfg;
        cfg.max_leaf_entries = 10;
        cfg.max_cached_pages = 24;
        ASSERT_EQ(S_OK, t.open(p, cfg)) << "重开失败";
        std::printf("  reopen: page_reads=%llu\n", (unsigned long long)t.stats().page_reads);

        int ok = 0, bad = 0;
        for (int i = 0; i < N; ++i) {
            std::string k = kstr(i);
            Val v;
            Status s = t.get(Slice(k), v);
            bool should_exist = (i % 7 != 0);
            if (should_exist) {
                if (s == S_OK && Slice(v.data(), v.size()).cmp(Slice(vstr(i))) == 0)
                    ++ok;
                else
                    ++bad;
            } else {
                if (s == S_NOTFOUND)
                    ++ok;
                else
                    ++bad;
            }
        }
        EXPECT_EQ(0, bad) << "重开后回读：ok=" << ok << " bad=" << bad;
        std::printf("  reopen 回读: ok=%d bad=%d\n", ok, bad);

        std::string err;
        ASSERT_EQ(S_OK, t.verify(err)) << "verify: " << err;
        std::printf("  verify: %s\n", err.c_str());
        EXPECT_EQ(S_OK, t.close());
    }
    rmpath(p);
}

/* ---------------------------------------------------------------- T2.4 */
TWT_TEST(P2_Btree, Eviction, "P2/T2.4 reconcile + eviction")
{
    std::string p = dbpath("p2_evict.wt");
    rmpath(p);

    Btree t;
    Config cfg;
    cfg.max_leaf_entries = 8;
    cfg.max_intl_refs = 4;
    cfg.max_cached_pages = 3; /* 故意极小，强制频繁淘汰 */
    ASSERT_EQ(S_OK, t.open(p, cfg));

    const int N = 1500;
    for (int i = 0; i < N; ++i) {
        std::string k = kstr(i), v = vstr(i);
        ASSERT_EQ(S_OK, t.put(Slice(k), Slice(v))) << "第 " << i << " 个键写失败";
    }
    std::printf("  evictions=%llu page_writes=%llu blocks_freed=%llu\n",
      (unsigned long long)t.stats().evictions, (unsigned long long)t.stats().page_writes,
      (unsigned long long)t.stats().blocks_freed);
    EXPECT_GT(t.stats().evictions, 0u) << "cache 只给 3 页，不可能没淘汰";

    int bad = 0;
    for (int i = 0; i < N; ++i) {
        Val v;
        Status s = t.get(Slice(kstr(i)), v);
        if (s != S_OK || Slice(v.data(), v.size()).cmp(Slice(vstr(i))) != 0)
            ++bad;
    }
    EXPECT_EQ(0, bad) << "淘汰后回读有 " << bad << " 个不对";
    std::printf("  回读 bad=%d\n", bad);

    ASSERT_EQ(S_OK, t.checkpoint());
    std::string err;
    ASSERT_EQ(S_OK, t.verify(err)) << "verify: " << err;
    std::printf("  verify: %s\n", err.c_str());
    EXPECT_EQ(S_OK, t.close());
    rmpath(p);
}

/* ---------------------------------------------------------------- T2.5 读可见性 */
TWT_TEST(P2_Btree, ReadVisibility, "P2/T2.5 读可见性（update chain）")
{
    std::string p = dbpath("p2_mvcc.wt");
    rmpath(p);

    Btree t;
    Config cfg;
    cfg.max_leaf_entries = 32;
    ASSERT_EQ(S_OK, t.open(p, cfg));

    /* txn 10 写下 v1 */
    ASSERT_EQ(S_OK, t.put(Slice("k"), Slice("v1"), /*txnid=*/10, /*ts=*/10));
    /* txn 20 写下 v2 */
    ASSERT_EQ(S_OK, t.put(Slice("k"), Slice("v2"), /*txnid=*/20, /*ts=*/20));

    Val v;
    /* 快照在 txn 15：只看到 v1 */
    ASSERT_EQ(S_OK, t.get(Slice("k"), v, /*reader_txnid=*/15, /*read_ts=*/15));
    EXPECT_EQ(0, Slice(v.data(), v.size()).cmp(Slice("v1"))) << "快照 15 应读到 v1";

    /* 快照在 txn 25：看到 v2 */
    ASSERT_EQ(S_OK, t.get(Slice("k"), v, 25, 25));
    EXPECT_EQ(0, Slice(v.data(), v.size()).cmp(Slice("v2"))) << "快照 25 应读到 v2";

    /* 时间戳早于 v2 的提交时间 */
    ASSERT_EQ(S_OK, t.get(Slice("k"), v, UINT64_MAX, 19));
    EXPECT_EQ(0, Slice(v.data(), v.size()).cmp(Slice("v1"))) << "read_ts=19 应读不到 ts=20 的版本";

    EXPECT_EQ(S_OK, t.close());
    rmpath(p);
}
