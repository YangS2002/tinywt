/*-
 * P4/T4.5 WAL 与崩溃恢复
 *
 * 崩溃恢复的判据只有一句话：
 *   「已提交的修改，在数据页还没落盘的情况下，重启后必须还在。」
 *
 * 测试怎么模拟崩溃：不调用 close()（close 会 checkpoint），
 * 直接把数据文件删掉，只留日志 —— 等价于「数据页全丢，日志还在」。
 * -*/
#define TWT_PROJECT "P4"

#include <cstdio>
#include <string>

#include "harness.hpp"
#include "twt_txn.h"

using namespace twt;

static void
test_wal_replay()
{
    CASE("P4/T4.5 WAL：崩溃后重放，已提交的写不丢");
    std::string p = dbpath("p4_wal.wt");
    std::string lp = p + ".log";
    rmpath(p);
    rmpath(lp);

    const int N = 300;

    /* --- 崩溃前：只写内存 + 日志，不 checkpoint --- */
    {
        Btree *bt = new Btree(); /* 故意不析构：析构会 checkpoint，就模拟不出崩溃了 */
        Config cfg;
        cfg.max_leaf_entries = 16;
        if (bt->open(p, cfg) != S_OK) {
            CHECK(false);
            return;
        }

        LogManager log;
        CHECK_ST(log.open(lp));
        for (int i = 0; i < N; ++i) {
            std::string k = kstr(i), v = vstr(i);
            CHECK_ST(bt->put(Slice(k), Slice(v)));
            Val vv(v.begin(), v.end());
            CHECK_ST(log.append(LOG_PUT, key_of(k), &vv));
        }
        CHECK_ST(log.append_commit(1));
        /* 到这里"断电"：bt 不 close、不 delete，数据页没有超级块，等于全丢 */
    }

    /* 数据文件丢了（没 checkpoint 过，本来也没有有效超级块） */
    rmpath(p);
    CHECK(fs::exists(lp)); /* 日志必须还在 */

    /* --- 恢复：空库 + 重放日志 --- */
    {
        Btree bt;
        Config cfg;
        cfg.max_leaf_entries = 16;
        CHECK_ST(bt.open(p, cfg));

        LogManager log;
        CHECK_ST(log.open(lp));
        CHECK_ST(log.replay(bt));
        CHECK(log.records() == (u64)N + 1); /* N 条 PUT + 1 条 COMMIT */

        int ok = 0, bad = 0;
        for (int i = 0; i < N; ++i) {
            Val v;
            if (bt.get(Slice(kstr(i)), v) == S_OK &&
              Slice(v.data(), v.size()).cmp(Slice(vstr(i))) == 0)
                ++ok;
            else
                ++bad;
        }
        CHECK(bad == 0);
        CHECK(ok == N);

        /* 恢复出来的数据 checkpoint 之后日志就可以清掉了 */
        CHECK_ST(bt.checkpoint());
        CHECK_ST(log.truncate());
        CHECK(log.records() == 0);
        CHECK_ST(bt.close());
    }

    rmpath(p);
    rmpath(lp);
}

static void
test_wal_truncated_tail()
{
    CASE("P4/T4.5 WAL：尾部被截断的记录要能容忍");
    std::string p = dbpath("p4_wal_trunc.wt");
    std::string lp = p + ".log";
    rmpath(p);
    rmpath(lp);

    {
        LogManager log;
        CHECK_ST(log.open(lp));
        for (int i = 0; i < 10; ++i) {
            std::string k = kstr(i), v = vstr(i);
            Val vv(v.begin(), v.end());
            CHECK_ST(log.append(LOG_PUT, key_of(k), &vv));
        }
        CHECK_ST(log.close());
    }

    /* 砍掉最后 3 个字节：最后一条记录变成半截 */
    {
        std::error_code ec;
        auto sz = fs::file_size(lp, ec);
        CHECK(!ec);
        fs::resize_file(lp, sz - 3, ec);
        CHECK(!ec);
    }

    {
        Btree bt;
        CHECK_ST(bt.open(p, Config{}));
        LogManager log;
        CHECK_ST(log.open(lp));

        /* 前 9 条必须重放成功；第 10 条半截 → 停止，但不报错 */
        CHECK_ST(log.replay(bt));
        Val v;
        CHECK_ST(bt.get(Slice(kstr(0)), v));
        CHECK_ST(bt.get(Slice(kstr(8)), v));
        CHECK_ST(bt.checkpoint());
        CHECK_ST(bt.close());
    }

    rmpath(p);
    rmpath(lp);
}

static void
test_wal_remove()
{
    CASE("P4/T4.5 WAL：REMOVE 记录也能重放");
    std::string p = dbpath("p4_wal_rm.wt");
    std::string lp = p + ".log";
    rmpath(p);
    rmpath(lp);

    {
        Btree bt;
        CHECK_ST(bt.open(p, Config{}));
        LogManager log;
        CHECK_ST(log.open(lp));

        /* 先落两条 PUT，再一条 REMOVE，然后才 checkpoint */
        for (int i = 0; i < 2; ++i) {
            std::string k = kstr(i), v = vstr(i);
            Val vv(v.begin(), v.end());
            CHECK_ST(bt.put(Slice(k), Slice(v)));
            CHECK_ST(log.append(LOG_PUT, key_of(k), &vv));
        }
        CHECK_ST(bt.remove(Slice(kstr(1))));
        CHECK_ST(log.append(LOG_REMOVE, key_of(kstr(1)), nullptr));
        CHECK_ST(log.append_commit(1));
        CHECK_ST(bt.checkpoint());
        CHECK_ST(bt.close());
        CHECK_ST(log.close());
    }

    /* 换一个"空"库重放同一份日志：应得到 k0 存在、k1 不存在 */
    {
        std::string p2 = dbpath("p4_wal_rm2.wt");
        rmpath(p2);
        Btree bt;
        CHECK_ST(bt.open(p2, Config{}));
        LogManager log;
        CHECK_ST(log.open(lp));
        CHECK_ST(log.replay(bt));

        Val v;
        CHECK_ST(bt.get(Slice(kstr(0)), v));
        CHECK_RC(bt.get(Slice(kstr(1)), v), S_NOTFOUND);
        CHECK_ST(bt.checkpoint());
        CHECK_ST(bt.close());
        rmpath(p2);
    }

    rmpath(p);
    rmpath(lp);
}

int
main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("tinywt P4: WAL / recovery\n");
    RUN(test_wal_replay);
    RUN(test_wal_truncated_tail);
    RUN(test_wal_remove);
    return harness_report();
}
