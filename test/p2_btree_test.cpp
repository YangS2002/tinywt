/*-
 * P2 —— Row-Store B-Tree（对标 BusTub P2 B+Tree Index）
 *
 *   Task 2.1  内存页三件套（entries / update 链 / insert 跳表）+ page_in
 *   Task 2.2  descend 二分下降                 ← BusTub Checkpoint 1: Search
 *   Task 2.3  插入 + 叶/内部分裂               ← BusTub Checkpoint 2: Insert
 *   Task 2.4  reconcile + LRU eviction
 *   Task 2.5  checkpoint + 重开恢复 + verify + 读可见性
 * -*/
#define TWT_PROJECT "P2"

#include <map>
#include <random>
#include <string>

#include "harness.hpp"
#include "twt_btree.h"

using namespace twt;

/* ---------------------------------------------------------------- T2.1–T2.3 */
static void
test_btree_basic()
{
    CASE("P2/T2.1 内存页三件套 + page_in");
    std::string p = dbpath("p2_basic.wt");
    rmpath(p);

    Btree t;
    Config cfg;
    cfg.max_leaf_entries = 8;
    CHECK_ST(t.open(p, cfg));

    CHECK_ST(t.put(Slice("a"), Slice("1")));
    CHECK_ST(t.put(Slice("b"), Slice("2")));

    Val v;
    CHECK_ST(t.get(Slice("a"), v));
    CHECK(v.size() == 1 && v[0] == '1');
    CHECK_ST(t.get(Slice("b"), v));
    CHECK(v.size() == 1 && v[0] == '2');
    CHECK(t.get(Slice("zzz"), v) == S_NOTFOUND);

    /* 覆盖写 */
    CHECK_ST(t.put(Slice("a"), Slice("111")));
    CHECK_ST(t.get(Slice("a"), v));
    CHECK(v.size() == 3);

    /* 删除 */
    CHECK_ST(t.remove(Slice("a")));
    CHECK(t.get(Slice("a"), v) == S_NOTFOUND);

    CHECK_ST(t.close());
    rmpath(p);
}

/* ---------------------------------------------------------------- T2.3 */
static void
test_split_and_many()
{
    CASE("P2/T2.2 descend（5000 键回读）");
    std::string p = dbpath("p2_split.wt");
    rmpath(p);

    Btree t;
    Config cfg;
    cfg.max_leaf_entries = 16;
    cfg.max_intl_refs = 8;
    cfg.max_cached_pages = 64;
    CHECK_ST(t.open(p, cfg));

    const int N = 5000;
    for (int i = 0; i < N; ++i) {
        std::string k = kstr(i), v = vstr(i);
        CHECK_ST(t.put(Slice(k), Slice(v)));
    }
    printf("  splits=%llu writes=%llu evictions=%llu\n", (unsigned long long)t.stats().splits,
      (unsigned long long)t.stats().page_writes, (unsigned long long)t.stats().evictions);
    CHECK(t.stats().splits > 0);

    for (int i = 0; i < N; ++i) {
        std::string k = kstr(i), want = vstr(i);
        Val v;
        Status s = t.get(Slice(k), v);
        if (s != S_OK || Slice(v.data(), v.size()).cmp(Slice(want)) != 0) {
            printf("  FAIL [%s] key=%s status=%s\n", g_case, k.c_str(), status_str(s));
            ++g_fail;
            break;
        }
    }
    CHECK_ST(t.checkpoint());
    std::string err;
    CHECK_ST(t.verify(err));
    printf("  verify: %s\n", err.c_str());
    CHECK_ST(t.close());
    rmpath(p);
}

/* ---------------------------------------------------------------- 评分器：对拍 std::map */
static void
diff_run(unsigned seed, size_t cached, const std::string &tag)
{
    CASE(tag.c_str());
    std::string p = dbpath("p2_diff.wt");
    rmpath(p);

    Btree t;
    Config cfg;
    cfg.max_leaf_entries = 12;
    cfg.max_intl_refs = 6;
    cfg.max_cached_pages = cached;
    CHECK_ST(t.open(p, cfg));

    std::map<std::string, std::string> ref;
    std::mt19937 rng(seed);
    const int OPS = 3000;
    const int KEYSPACE = 1200;

    for (int i = 0; i < OPS; ++i) {
        int op = rng() % 100;
        int ki = (int)(rng() % KEYSPACE);
        std::string k = kstr(ki);

        if (op < 60) {
            std::string v = vstr((int)rng());
            Status s = t.put(Slice(k), Slice(v));
            if (s != S_OK) {
                printf("  FAIL put %s: %s\n", k.c_str(), status_str(s));
                ++g_fail;
                break;
            }
            ref[k] = v;
        } else if (op < 80) {
            Status s = t.remove(Slice(k));
            if (s != S_OK) {
                printf("  FAIL remove %s: %s\n", k.c_str(), status_str(s));
                ++g_fail;
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
                printf("  FAIL get %s: expect_found=%d got=%s\n", k.c_str(), (int)expect_found,
                  status_str(s));
                ++g_fail;
                break;
            }
            if (expect_found && Slice(v.data(), v.size()).cmp(Slice(it->second)) != 0) {
                printf("  FAIL value mismatch %s\n", k.c_str());
                ++g_fail;
                break;
            }
        }
        ++g_pass;
    }

    /* 全量终态比对 */
    int mismatch = 0;
    for (const auto &kv : ref) {
        Val v;
        Status s = t.get(Slice(kv.first), v);
        bool bad = (s != S_OK) || (Slice(v.data(), v.size()).cmp(Slice(kv.second)) != 0);
        if (bad) {
            ++mismatch;
            if (mismatch <= 5)
                printf("  MISMATCH key=%s status=%s got=%.*s want=%s\n", kv.first.c_str(),
                  status_str(s), (int)v.size(), (const char *)v.data(), kv.second.c_str());
        }
    }
    CHECK(mismatch == 0);
    printf("  ref size=%zu mismatch=%d\n", ref.size(), mismatch);

    CHECK_ST(t.checkpoint());
    std::string err;
    CHECK_ST(t.verify(err));
    printf("  verify: %s\n", err.c_str());
    CHECK_ST(t.close());
    rmpath(p);
}

static void
test_diff_against_stdmap()
{
    /* 多组种子 × 两种 cache 压力（40 页会疯狂淘汰，1000 页基本不淘汰） */
    diff_run(20261020, 40, "P2/T2.3 分裂 + 对拍 std::map #1 (cache=40)");
    diff_run(1, 40, "P2/T2.3 分裂 + 对拍 std::map #2 (cache=40)");
    diff_run(2, 1000, "P2/T2.3 分裂 + 对拍 std::map #3 (cache=1000)");
    diff_run(3, 1000, "P2/T2.3 分裂 + 对拍 std::map #4 (cache=1000)");
}

/* ---------------------------------------------------------------- T2.5 */
static void
test_persistence()
{
    CASE("P2/T2.5 checkpoint + 重开 + verify");
    std::string p = dbpath("p2_persist.wt");
    rmpath(p);

    const int N = 800;
    {
        Btree t;
        Config cfg;
        cfg.max_leaf_entries = 10;
        cfg.max_cached_pages = 24;
        CHECK_ST(t.open(p, cfg));
        for (int i = 0; i < N; ++i) {
            std::string k = kstr(i), v = vstr(i);
            CHECK_ST(t.put(Slice(k), Slice(v)));
        }
        /* 删掉一部分，验证 tombstone 也能持久化 */
        for (int i = 0; i < N; i += 7)
            CHECK_ST(t.remove(Slice(kstr(i))));
        CHECK_ST(t.checkpoint());
        std::string err;
        CHECK_ST(t.verify(err));
        CHECK_ST(t.close());
    }

    {
        Btree t;
        Config cfg;
        cfg.max_leaf_entries = 10;
        cfg.max_cached_pages = 24;
        CHECK_ST(t.open(p, cfg));
        printf("  reopen: page_reads=%llu\n", (unsigned long long)t.stats().page_reads);

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
        CHECK(bad == 0);
        printf("  reopen 回读: ok=%d bad=%d\n", ok, bad);

        std::string err;
        CHECK_ST(t.verify(err));
        printf("  verify: %s\n", err.c_str());
        CHECK_ST(t.close());
    }
    rmpath(p);
}

/* ---------------------------------------------------------------- T2.4 */
static void
test_eviction()
{
    CASE("P2/T2.4 reconcile + eviction");
    std::string p = dbpath("p2_evict.wt");
    rmpath(p);

    Btree t;
    Config cfg;
    cfg.max_leaf_entries = 8;
    cfg.max_intl_refs = 4;
    cfg.max_cached_pages = 3; /* 故意极小，强制频繁淘汰 */
    CHECK_ST(t.open(p, cfg));

    const int N = 1500;
    for (int i = 0; i < N; ++i) {
        std::string k = kstr(i), v = vstr(i);
        CHECK_ST(t.put(Slice(k), Slice(v)));
    }
    printf("  evictions=%llu page_writes=%llu blocks_freed=%llu\n",
      (unsigned long long)t.stats().evictions, (unsigned long long)t.stats().page_writes,
      (unsigned long long)t.stats().blocks_freed);
    CHECK(t.stats().evictions > 0);

    int bad = 0;
    for (int i = 0; i < N; ++i) {
        Val v;
        Status s = t.get(Slice(kstr(i)), v);
        if (s != S_OK || Slice(v.data(), v.size()).cmp(Slice(vstr(i))) != 0)
            ++bad;
    }
    CHECK(bad == 0);
    printf("  回读 bad=%d\n", bad);

    CHECK_ST(t.checkpoint());
    std::string err;
    CHECK_ST(t.verify(err));
    printf("  verify: %s\n", err.c_str());
    CHECK_ST(t.close());
    rmpath(p);
}

/* ---------------------------------------------------------------- T2.5 读可见性 */
static void
test_mvcc()
{
    CASE("P2/T2.5 读可见性（update chain）");
    std::string p = dbpath("p2_mvcc.wt");
    rmpath(p);

    Btree t;
    Config cfg;
    cfg.max_leaf_entries = 32;
    CHECK_ST(t.open(p, cfg));

    /* txn 10 写下 v1 */
    CHECK_ST(t.put(Slice("k"), Slice("v1"), /*txnid=*/10, /*ts=*/10));
    /* txn 20 写下 v2 */
    CHECK_ST(t.put(Slice("k"), Slice("v2"), /*txnid=*/20, /*ts=*/20));

    Val v;
    /* 快照在 txn 15：只看到 v1 */
    CHECK_ST(t.get(Slice("k"), v, /*reader_txnid=*/15, /*read_ts=*/15));
    CHECK(Slice(v.data(), v.size()).cmp(Slice("v1")) == 0);

    /* 快照在 txn 25：看到 v2 */
    CHECK_ST(t.get(Slice("k"), v, 25, 25));
    CHECK(Slice(v.data(), v.size()).cmp(Slice("v2")) == 0);

    /* 时间戳早于 v2 的提交时间 */
    CHECK_ST(t.get(Slice("k"), v, UINT64_MAX, 19));
    CHECK(Slice(v.data(), v.size()).cmp(Slice("v1")) == 0);

    CHECK_ST(t.close());
    rmpath(p);
}

int
main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("tinywt P2: row-store B-Tree\n");
    RUN(test_btree_basic);
    RUN(test_split_and_many);
    RUN(test_diff_against_stdmap);
    RUN(test_persistence);
    RUN(test_eviction);
    RUN(test_mvcc);
    return harness_report();
}
