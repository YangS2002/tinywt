/*-
 * P4/T4.1 并发 L1 + T4.2 WT_REF 状态机 / hazard pointer
 *
 * 顺序说明（和计划一致）：先让「多线程不崩」成立（T4.1 一把 tree 级读写锁），
 * 再把读路径上的锁去掉、换成 hazard pointer（T4.2）。
 *
 * 这两项之所以放在一个二进制里，是因为它们测的是同一条读路径的两端：
 *   T4.1 关心**结果对不对**（4 线程随机写 == std::map 终态）
 *   T4.2 关心**页的生命周期**（有人在读时，淘汰方不能 free 它）
 *
 * 对标真 WT：
 *   src/include/hazard.h        WT_HAZARD_POINTER / WT_HAZARD（16 槽）
 *   src/support/hazard.c        __wt_hazard_set / __wt_hazard_clear
 *   src/include/btmem.h         WT_REF 状态机 DISK→LOCKED(reading)→MEM，淘汰 MEM→LOCKED→DISK
 *
 * 评分点（每项一个测试函数）：
 *   1. 并发 L1（4 线程对拍 std::map，×2 种子）
 *   2. WT_REF 状态机 + hazard pointer
 *   3. 读线程 + 疯狂淘汰（无悬空，ASan 下跑）
 * -*/
#include <atomic>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "support/harness.hpp"
#include "tinywt/btree/btree.h"
#include "tinywt/concurrency/hazard.h"

namespace {

using namespace twt;

constexpr int kThreads = 4;
constexpr int kShard = 400; /* 每个线程独占的键区间 → 期望值不用跨线程共享 */

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
cfg_for(size_t leaf, size_t pages)
{
    Config c;
    c.max_leaf_entries = leaf;
    c.max_intl_refs = 8;
    c.max_cached_pages = pages;
    c.alloc_size = 512;
    return c;
}

} /* namespace */

/* ---------------------------------------------------------------- 1. 并发 L1 */
TWT_TEST(P4_Concurrency, ConcurrentL1, "P4/T4.1 并发 L1（4 线程对拍 std::map）")
{
    for (int seed = 1; seed <= 2; ++seed) {
        const std::string path = dbpath("p4_cc_l1_s" + std::to_string(seed));
        rmpath(path);

        Btree bt;
        ASSERT_EQ(S_OK, bt.open(path, cfg_for(16, 32)));

        /*
         * 每个线程只碰自己那一段键，期望值放各自的 map 里 ——
         * 这样断言不需要额外同步，失败也能定位到线程。
         */
        std::vector<std::map<std::string, std::string>> expect(kThreads);
        std::vector<std::thread> ths;
        std::atomic<int> mismatches{0};

        for (int t = 0; t < kThreads; ++t) {
            ths.emplace_back([&bt, &expect, &mismatches, t, seed]() {
                /*
                 * 线程里不能抛异常（std::terminate 会带走整个进程，
                 * 后面的测试项就全没了）。所以整段包一层，失败记数。
                 */
                try {
                    std::mt19937 rng((unsigned)(seed * 1000 + t));
                    const int lo = t * kShard;

                    for (int i = 0; i < 1200; ++i) {
                        const int k = lo + (int)(rng() % (unsigned)kShard);
                        const std::string key = kstr(k);
                        const unsigned op = rng() % 100u;

                        if (op < 55) { /* 写 */
                            std::string val = vstr(k) + "-r" + std::to_string(i);
                            if (bt.put(Slice(key), Slice(val)) == S_OK)
                                expect[(size_t)t][key] = val;
                            else
                                ++mismatches;
                        } else if (op < 75) { /* 删 */
                            if (bt.remove(Slice(key)) == S_OK)
                                expect[(size_t)t].erase(key);
                            else
                                ++mismatches;
                        } else { /* 读：必须等于自己写过的值，或者确实没写过 */
                            Val out;
                            Status s = bt.get(Slice(key), out);
                            auto it = expect[(size_t)t].find(key);
                            if (it == expect[(size_t)t].end()) {
                                if (s != S_NOTFOUND)
                                    ++mismatches;
                            } else if (s != S_OK || sv(out) != it->second) {
                                ++mismatches;
                            }
                        }
                    }
                } catch (const std::exception &) {
                    ++mismatches;
                }
            });
        }

        for (auto &th : ths)
            th.join();

        EXPECT_EQ(0, mismatches.load()) << "seed=" << seed << "：并发操作与各自预期值不符";

        /* 终态全量对拍 */
        std::map<std::string, std::string> all;
        for (const auto &m : expect)
            all.insert(m.begin(), m.end());

        int wrong = 0;
        for (int k = 0; k < kThreads * kShard; ++k) {
            const std::string key = kstr(k);
            Val out;
            Status s = bt.get(Slice(key), out);
            auto it = all.find(key);
            if (it == all.end()) {
                if (s != S_NOTFOUND)
                    ++wrong;
            } else if (s != S_OK || sv(out) != it->second) {
                ++wrong;
            }
        }
        EXPECT_EQ(0, wrong) << "seed=" << seed << "：终态全量对拍有 " << wrong << " 个不对";

        /* 并发写必须真的把树撑到多页，否则这条测试等于什么都没测 */
        EXPECT_GT(bt.stats().splits, 0u);
        EXPECT_GT(bt.root_page_id(), u32(1)) << "根页还是 1，树根本没长起来";

        EXPECT_EQ(S_OK, bt.close());
        rmpath(path);
    }
}

/* ---------------------------------------------------------------- 2. 状态机 + hazard */
TWT_TEST(P4_Concurrency, RefStateAndHazard, "P4/T4.2 WT_REF 状态机 + hazard pointer")
{
    /* --- 2a. hazard pointer 单元语义 --- */
    {
        Page p1, p2, p3;
        HazardPointer hp;

        EXPECT_FALSE(hp.holds(&p1));
        hp.set(0, &p1);
        EXPECT_TRUE(hp.holds(&p1));
        EXPECT_FALSE(hp.holds(&p2));

        /* 不同槽互不影响：真 WT 一个线程可能同时持有多个页 */
        hp.set(1, &p2);
        EXPECT_TRUE(hp.holds(&p1));
        EXPECT_TRUE(hp.holds(&p2));

        hp.clear(0);
        EXPECT_FALSE(hp.holds(&p1));
        EXPECT_TRUE(hp.holds(&p2)) << "清 0 号槽不能把 1 号一起清掉";

        hp.set(0, &p3); /* 槽可复用 */
        EXPECT_TRUE(hp.holds(&p3));
        hp.clear(0);
        hp.clear(1);
        EXPECT_FALSE(hp.holds(&p2));
        EXPECT_FALSE(hp.holds(&p3));
    }

    /* --- 2b. 登记表：attach / detach / any_holds / threads --- */
    {
        HazardRegistry &reg = hazard_registry();
        EXPECT_EQ(&reg, &hazard_registry()) << "必须是同一个单例";

        Page target, other;
        const size_t base = reg.threads();

        HazardPointer hp;
        reg.attach(&hp);
        EXPECT_EQ(base + 1, reg.threads());
        EXPECT_FALSE(reg.any_holds(&target)) << "没登记就不能拦住淘汰";

        hp.set(0, &target);
        EXPECT_TRUE(hp.holds(&target));
        /* 淘汰方的视角：有人在用 → 不能 free */
        EXPECT_TRUE(reg.any_holds(&target));
        EXPECT_FALSE(reg.any_holds(&other));

        hp.set(5, &other); /* 登记在非 0 号槽也必须被扫到 */
        EXPECT_TRUE(reg.any_holds(&other));

        hp.clear(0);
        EXPECT_FALSE(reg.any_holds(&target));
        EXPECT_TRUE(reg.any_holds(&other));

        hp.clear(5);
        reg.detach(&hp);
        EXPECT_EQ(base, reg.threads());

        /* detach 之后，即使指针没清也不能再影响淘汰判断 */
        EXPECT_FALSE(reg.any_holds(&other));
    }

    /* --- 2c. Btree::root_ref_state：DISK → MEM 的迁移 --- */
    {
        const std::string path = dbpath("p4_cc_ref");
        rmpath(path);

        {
            Btree bt;
            ASSERT_EQ(S_OK, bt.open(path, cfg_for(8, 8)));
            for (int i = 0; i < 120; ++i)
                ASSERT_EQ(S_OK, bt.put(Slice(kstr(i)), Slice(vstr(i))));
            ASSERT_EQ(S_OK, bt.checkpoint());
            ASSERT_EQ(S_OK, bt.close());
        }

        Btree bt;
        ASSERT_EQ(S_OK, bt.open(path, cfg_for(8, 8)));
        /*
         * 契约：open 只从超级块建立 Ref，不读页。
         * 第一次 descend 才 DISK → LOCKED(reading=true) → MEM。
         */
        EXPECT_EQ(REF_DISK, bt.root_ref_state()) << "open 不该顺手把根页读进来";

        {
            Val out;
            ASSERT_EQ(S_OK, bt.get(Slice(kstr(0)), out));
            EXPECT_EQ(vstr(0), sv(out));
        }
        EXPECT_EQ(REF_MEM, bt.root_ref_state()) << "descend 之后根页应该已经在内存里";

        EXPECT_EQ(S_OK, bt.close());
        rmpath(path);
    }

    /* --- 2d. 全新树：根页是内存里造出来的，一开始就是 MEM --- */
    {
        const std::string path = dbpath("p4_cc_ref_new");
        rmpath(path);

        Btree bt;
        ASSERT_EQ(S_OK, bt.open(path, cfg_for(8, 8)));
        ASSERT_EQ(S_OK, bt.put(Slice(kstr(0)), Slice(vstr(0))));
        EXPECT_EQ(REF_MEM, bt.root_ref_state());

        EXPECT_EQ(S_OK, bt.close());
        rmpath(path);
    }
}

/* ---------------------------------------------------------------- 3. 读 + 疯狂淘汰 */
TWT_TEST(P4_Concurrency, ReaderUnderEviction, "P4/T4.2 读线程 + 疯狂淘汰（页被淘汰不悬空）")
{
    const std::string path = dbpath("p4_cc_evict");
    rmpath(path);
    constexpr int kStable = 600; /* 读线程只看这一段，值永远不变 */
    constexpr int kChurn = 400;  /* 写线程只动这一段，负责制造淘汰压力 */

    Btree bt;
    /* cache 只够放 4 页 → 遍历时几乎每次 descend 都会触发淘汰 */
    ASSERT_EQ(S_OK, bt.open(path, cfg_for(8, 4)));
    for (int i = 0; i < kStable; ++i)
        ASSERT_EQ(S_OK, bt.put(Slice(kstr(i)), Slice(vstr(i))));

    std::atomic<int> bad{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> ths;

    /* 1 个写线程：反复改另一段键，逼出 split + eviction */
    ths.emplace_back([&bt, &stop, &bad]() {
        try {
            std::mt19937 rng(7);
            int i = 0;
            while (!stop.load()) {
                int k = kStable + (int)(rng() % (unsigned)kChurn);
                if (i % 3 == 0) {
                    /* 删不存在的键不算错，这里只关心「不崩、不污染稳定段」 */
                    (void)bt.remove(Slice(kstr(k)));
                } else {
                    if (bt.put(Slice(kstr(k)), Slice(vstr(k) + "-x")) != S_OK)
                        ++bad;
                }
                ++i;
            }
        } catch (const std::exception &) {
            ++bad;
            stop.store(true);
        }
    });

    /* 3 个读线程：读稳定段，值必须一字不差 */
    for (int t = 0; t < 3; ++t) {
        ths.emplace_back([&bt, &bad, t]() {
            try {
                std::mt19937 rng((unsigned)(100 + t));
                for (int i = 0; i < 1500; ++i) {
                    int k = (int)(rng() % (unsigned)kStable);
                    Val out;
                    if (bt.get(Slice(kstr(k)), out) != S_OK || sv(out) != vstr(k))
                        ++bad;
                }
            } catch (const std::exception &) {
                ++bad;
            }
        });
    }

    /* 等读线程跑完，再让写线程停 */
    for (size_t i = 1; i < ths.size(); ++i)
        ths[i].join();
    stop.store(true);
    ths[0].join();

    EXPECT_EQ(0, bad.load()) << "并发读/淘汰过程中出现了错误结果";

    /* 淘汰确实发生过；否则这条测试没测到东西 */
    EXPECT_GT(bt.stats().evictions, 0u) << "cache 只给 4 页，不可能没淘汰";

    /* 收尾：稳定段全量校验 */
    int wrong = 0;
    for (int k = 0; k < kStable; ++k) {
        Val out;
        if (bt.get(Slice(kstr(k)), out) != S_OK || sv(out) != vstr(k))
            ++wrong;
    }
    EXPECT_EQ(0, wrong) << "稳定段被并发淘汰弄坏了 " << wrong << " 个键";

    EXPECT_EQ(S_OK, bt.close());
    rmpath(path);
}
