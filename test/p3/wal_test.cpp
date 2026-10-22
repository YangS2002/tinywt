/*-
 * P3/T3.5 WAL 重放 + 崩溃恢复 —— BusTub 里没有对应项，这是持久性的收口
 *
 * 记录布局与三条恢复语义见 include/tinywt/wal/log.h 顶部。这里只强调测试在考什么：
 *
 *   重放      —— 日志里已提交的事务，重放到一棵**空树**上必须完全还原
 *   未提交丢弃 —— 没有 COMMIT 的事务一条都不能进数据文件（否则就是脏读）
 *   截断      —— checkpoint 之后日志可以被裁剪，且裁剪是真的从磁盘上删
 *   尾部半条  —— 崩溃现场最后一条写一半是**正常**的，必须当成 S_OK
 *   中间损坏  —— 这不是崩溃，是介质/程序错，必须报 S_CORRUPT
 *
 * ★ 为什么「尾部半条」和「中间损坏」要分开判：
 *   两者的现场都是 checksum 对不上，区别只在**后面还有没有完整记录**。
 *   一律报错 → 崩溃后永远起不来；一律放过 → 真损坏被当成正常截断，静默丢数据。
 *
 * 对标真 WT：
 *   src/log/log.c          __wt_log_write（LOG_ALIGN=128、LSN=(file<<32)|offset）
 *   src/log/log_recover.c  __wt_log_recover（扫描 → 只重放已提交 → 立刻 checkpoint）
 *
 * 评分点（每项一个测试函数）：
 *   1. WAL 重放
 *   2. 未提交丢弃
 *   3. 截断（ckpt 之后裁剪）
 *   4. 尾部截断（半条记录）
 *   5. 中间损坏 → CORRUPT
 * -*/
#define TWT_PROJECT "P3"

#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "support/harness.hpp"
#include "tinywt/btree/btree.h"
#include "tinywt/wal/log.h"

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

/* 一套「日志 + 空数据文件」的组合，用完整体清掉 */
struct Fixture {
    std::string name;
    std::string logpath;
    std::string dbpath_;

    explicit Fixture(const char *n) : name(n)
    {
        logpath = dbpath(name) + ".log";
        dbpath_ = dbpath(name) + ".db";
        rmpath(logpath);
        rmpath(dbpath_);
    }
    ~Fixture()
    {
        rmpath(logpath);
        rmpath(dbpath_);
    }
};

/* 文件里某个字节异或一下（模拟介质损坏 / 半截写） */
bool
flip_byte(const std::string &path, long off)
{
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f)
        return false;
    f.seekg(off);
    char c = 0;
    if (!f.read(&c, 1))
        return false;
    c = (char)(c ^ 0x5A);
    f.clear();
    f.seekp(off);
    f.write(&c, 1);
    return f.good();
}

} /* namespace */

/* ---------------------------------------------------------------- 1. 重放 */
static void
test_replay()
{
    CASE("P3/T3.5 WAL 重放（已提交事务还原到空树）");
    Fixture fx("p3_wal_replay");

    {
        LogManager log;
        CHECK_ST(log.open(fx.logpath));

        /* txn 1：写两条 */
        CHECK_ST(log.txn_begin(1));
        CHECK_ST(log.put(1, Slice("k1"), Slice("v1")));
        CHECK_ST(log.put(1, Slice("k2"), Slice("v2")));
        CHECK_ST(log.txn_commit(1));

        /* txn 2：删一条、加一条 */
        CHECK_ST(log.txn_begin(2));
        CHECK_ST(log.remove(2, Slice("k1")));
        CHECK_ST(log.put(2, Slice("k3"), Slice("v3")));
        CHECK_ST(log.txn_commit(2));

        CHECK(log.last_lsn() > 0);
        CHECK(log.record_count() >= 6); /* 每次 API 调用至少一条记录 */
        CHECK_ST(log.close());
    }

    /* 换一个全新的 Btree 当「崩溃后重开」的数据文件 */
    {
        Btree bt;
        CHECK_ST(bt.open(fx.dbpath_, small_cfg()));

        LogManager log;
        CHECK_ST(log.open(fx.logpath));
        CHECK_ST(log.replay(bt));

        Val out;
        CHECK_RC(bt.get(Slice("k1"), out), S_NOTFOUND); /* txn 2 里的 remove 生效 */
        CHECK_ST(bt.get(Slice("k2"), out));
        CHECK(sv(out) == "v2");
        CHECK_ST(bt.get(Slice("k3"), out));
        CHECK(sv(out) == "v3");

        CHECK_ST(log.close());
        CHECK_ST(bt.close());
    }
}

/* ---------------------------------------------------------------- 2. 未提交丢弃 */
static void
test_uncommitted_discarded()
{
    CASE("P3/T3.5 未提交丢弃（没有 COMMIT 的事务不进数据文件）");
    Fixture fx("p3_wal_uncommitted");

    {
        LogManager log;
        CHECK_ST(log.open(fx.logpath));

        /* 已提交 */
        CHECK_ST(log.txn_begin(1));
        CHECK_ST(log.put(1, Slice("k-ok"), Slice("yes")));
        CHECK_ST(log.txn_commit(1));

        /* 写完了但没提交（进程在这里被杀） */
        CHECK_ST(log.txn_begin(2));
        CHECK_ST(log.put(2, Slice("k-lost"), Slice("no")));
        CHECK_ST(log.put(2, Slice("k-ok"), Slice("overwritten-by-ghost")));
        /* 故意不写 COMMIT */

        /* 提交记录写在后面的事务，不能因为「前面有个没提交的」就被连累 */
        CHECK_ST(log.txn_begin(3));
        CHECK_ST(log.put(3, Slice("k-later"), Slice("later")));
        CHECK_ST(log.txn_commit(3));

        CHECK_ST(log.close());
    }

    {
        Btree bt;
        CHECK_ST(bt.open(fx.dbpath_, small_cfg()));

        LogManager log;
        CHECK_ST(log.open(fx.logpath));
        CHECK_ST(log.replay(bt));

        Val out;
        CHECK_ST(bt.get(Slice("k-ok"), out));
        CHECK(sv(out) == "yes"); /* 未被幽灵事务覆盖 */
        CHECK_RC(bt.get(Slice("k-lost"), out), S_NOTFOUND);
        CHECK_ST(bt.get(Slice("k-later"), out));
        CHECK(sv(out) == "later");

        CHECK_ST(log.close());
        CHECK_ST(bt.close());
    }
}

/* ---------------------------------------------------------------- 3. 截断 */
static void
test_truncate_upto()
{
    CASE("P3/T3.5 截断（ckpt 之后日志只留 >= ckpt_lsn）");
    Fixture fx("p3_wal_trunc");
    Lsn ckpt_lsn = 0;
    u64 before = 0;

    {
        LogManager log;
        CHECK_ST(log.open(fx.logpath));

        /* 这一段视为「已经 checkpoint 落盘」的历史 */
        CHECK_ST(log.txn_begin(1));
        CHECK_ST(log.put(1, Slice("k-old"), Slice("in-page")));
        CHECK_ST(log.txn_commit(1));
        ckpt_lsn = log.last_lsn();

        /* 这一段是 checkpoint 之后才发生的，必须留着 */
        CHECK_ST(log.txn_begin(2));
        CHECK_ST(log.put(2, Slice("k-new"), Slice("in-log")));
        CHECK_ST(log.txn_commit(2));

        before = log.record_count();
        CHECK(ckpt_lsn > 0);

        CHECK_ST(log.truncate_upto(ckpt_lsn));
        CHECK(log.record_count() < before);
        CHECK(log.record_count() == 3); /* 只剩 txn 2 的 begin/put/commit */
        CHECK_ST(log.close());
    }

    /* 裁剪必须落到磁盘：重开日志文件，条数不能「长回来」 */
    {
        Btree bt;
        CHECK_ST(bt.open(fx.dbpath_, small_cfg()));

        LogManager log;
        CHECK_ST(log.open(fx.logpath));
        CHECK_ST(log.replay(bt));

        Val out;
        CHECK_ST(bt.get(Slice("k-new"), out));
        CHECK(sv(out) == "in-log");
        /*
         * k-old 不在了 —— 这是对的：它的数据已经在 checkpoint 写进数据页里，
         * 日志不再需要它。重放到空树上没有它，恰好证明「日志确实被裁剪了」。
         */
        CHECK_RC(bt.get(Slice("k-old"), out), S_NOTFOUND);

        CHECK_ST(log.close());
        CHECK_ST(bt.close());
    }
}

/* ---------------------------------------------------------------- 4. 尾部半条 */
static void
test_tail_truncation()
{
    CASE("P3/T3.5 尾部截断（半条记录 = 正常崩溃，返回 OK）");
    Fixture fx("p3_wal_tail");
    long size = 0;

    {
        LogManager log;
        CHECK_ST(log.open(fx.logpath));

        CHECK_ST(log.txn_begin(1));
        CHECK_ST(log.put(1, Slice("k1"), Slice("v1")));
        CHECK_ST(log.txn_commit(1));

        CHECK_ST(log.txn_begin(2));
        CHECK_ST(log.put(2, Slice("k2"), Slice("v2")));
        CHECK_ST(log.txn_commit(2));

        CHECK_ST(log.close());
    }

    /* 砍掉最后 3 个字节：最后一条提到一半，进程死了 */
    {
        std::error_code ec;
        size = (long)fs::file_size(fx.logpath, ec);
        CHECK(!ec && size > 8);
        fs::resize_file(fx.logpath, (std::uintmax_t)(size - 3), ec);
        CHECK(!ec);
    }

    {
        Btree bt;
        CHECK_ST(bt.open(fx.dbpath_, small_cfg()));

        LogManager log;
        CHECK_ST(log.open(fx.logpath));
        /* 关键：尾部不完整不是错误 */
        CHECK_ST(log.replay(bt));

        Val out;
        CHECK_ST(bt.get(Slice("k1"), out));
        CHECK(sv(out) == "v1");
        /* txn 2 的 COMMIT 被砍掉了 → 整个 txn 2 视为未提交，丢弃 */
        CHECK_RC(bt.get(Slice("k2"), out), S_NOTFOUND);

        CHECK_ST(log.close());
        CHECK_ST(bt.close());
    }
}

/* ---------------------------------------------------------------- 5. 中间损坏 */
static void
test_mid_file_corruption()
{
    CASE("P3/T3.5 中间损坏 → CORRUPT（不是尾截断）");
    Fixture fx("p3_wal_corrupt");

    {
        LogManager log;
        CHECK_ST(log.open(fx.logpath));
        for (u64 t = 1; t <= 3; ++t) {
            CHECK_ST(log.txn_begin(t));
            std::string k = "k" + std::to_string(t);
            CHECK_ST(log.put(t, Slice(k), Slice("v")));
            CHECK_ST(log.txn_commit(t));
        }
        CHECK_ST(log.close());
    }

    /*
     * 改掉第 1 条记录的 checksum 字段（布局：[u32 len][u32 crc][u8 flags]，
     * 偏移 4..7 是 crc）。这样 len 仍然可读 —— 解析器能跳到下一条完整记录，
     * 从而分辨出「损坏在中间」而不是「尾部截断」。
     */
    CHECK(flip_byte(fx.logpath, 6));

    {
        Btree bt;
        CHECK_ST(bt.open(fx.dbpath_, small_cfg()));

        LogManager log;
        CHECK_ST(log.open(fx.logpath));
        CHECK_RC(log.replay(bt), S_CORRUPT);

        CHECK_ST(log.close());
        CHECK_ST(bt.close());
    }
}

/* ---------------------------------------------------------------- 6. 随机 crash ×4 */
static void
test_random_crash()
{
    CASE("P3/T3.5 随机 crash ×4 种子（状态必须等于某个已提交前缀）");

    struct ROp {
        bool remove;
        std::string key, val;
    };

    for (int seed = 1; seed <= 4; ++seed) {
        Fixture fx(("p3_wal_crash" + std::to_string(seed)).c_str());

        std::vector<ROp> seq;        /* 已提交事务的写操作，按提交顺序展开 */
        std::vector<size_t> txn_end; /* 每个已提交事务结束后 seq 的长度 */

        /* --- 造日志：70% 提交，30% 半途而废；另有一段只出现在未提交事务里的键 --- */
        {
            LogManager log;
            CHECK_ST(log.open(fx.logpath));

            std::mt19937 rng((unsigned)(seed * 7919));
            u64 txnid = 1;

            for (int t = 0; t < 12; ++t, ++txnid) {
                const bool commit = (rng() % 100) < 70;
                const int nops = 1 + (int)(rng() % 3);

                CHECK_ST(log.txn_begin(txnid));
                for (int o = 0; o < nops; ++o) {
                    const unsigned kind = rng() % 100;
                    if (kind < 75) {
                        const std::string k = "c" + std::to_string(rng() % 10);
                        const std::string v =
                          "v" + std::to_string(txnid) + "_" + std::to_string(o);
                        CHECK_ST(log.put(txnid, Slice(k), Slice(v)));
                        if (commit)
                            seq.push_back({false, k, v});
                    } else if (kind < 90) {
                        const std::string k = "c" + std::to_string(rng() % 10);
                        CHECK_ST(log.remove(txnid, Slice(k)));
                        if (commit)
                            seq.push_back({true, k, ""});
                    } else {
                        /* 只出现在未提交事务里的键：重放后必须一个都不存在 */
                        const std::string k = "u" + std::to_string(rng() % 8);
                        CHECK_ST(log.put(txnid, Slice(k), Slice("ghost")));
                    }
                }
                if (commit) {
                    CHECK_ST(log.txn_commit(txnid));
                    txn_end.push_back(seq.size());
                }
            }
            CHECK_ST(log.close());
        }

        /* --- 在随机位置砍断日志：模拟进程在写日志时被杀 --- */
        {
            std::error_code ec;
            const auto sz = fs::file_size(fx.logpath, ec);
            CHECK(!ec && sz > 32);
            const auto third = (std::uintmax_t)(sz / 3);
            const auto cut = third + ((std::uintmax_t)seed * 131) % third;
            CHECK(cut > 0 && cut < sz);
            fs::resize_file(fx.logpath, cut, ec);
            CHECK(!ec);
        }

        Btree bt;
        CHECK_ST(bt.open(fx.dbpath_, small_cfg()));

        LogManager log;
        CHECK_ST(log.open(fx.logpath));
        CHECK_ST(log.replay(bt)); /* 尾部半条是正常崩溃 */

        /* --- 不变量 1：未提交的键一个都不能出现 --- */
        int ghost = 0;
        for (int k = 0; k < 8; ++k) {
            Val out;
            if (bt.get(Slice("u" + std::to_string(k)), out) != S_NOTFOUND)
                ++ghost;
        }
        CHECK(ghost == 0);

        /*
         * --- 不变量 2：重放后的状态必须等于「前 c 个已提交事务」的后像 ---
         * c 是多少取决于截断点落在哪，测试不可能预知，所以枚举所有事务边界。
         * 这一条同时能抓住：半个事务被应用、事务顺序被打乱、删除被漏掉。
         */
        auto matches = [&](size_t cut_txns) -> bool {
            const size_t lim = (cut_txns == 0) ? 0 : txn_end[cut_txns - 1];
            std::map<std::string, std::string> want;
            for (size_t i = 0; i < lim; ++i) {
                if (seq[i].remove)
                    want.erase(seq[i].key);
                else
                    want[seq[i].key] = seq[i].val;
            }
            for (int k = 0; k < 10; ++k) {
                const std::string key = "c" + std::to_string(k);
                Val out;
                const Status s = bt.get(Slice(key), out);
                auto it = want.find(key);
                if (it == want.end()) {
                    if (s != S_NOTFOUND)
                        return false;
                } else if (s != S_OK || sv(out) != it->second) {
                    return false;
                }
            }
            return true;
        };

        bool matched = false;
        for (size_t c = 0; c <= txn_end.size() && !matched; ++c)
            matched = matches(c);
        CHECK(matched);

        CHECK_ST(log.close());
        CHECK_ST(bt.close());
    }
}

int
main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("tinywt P3: write-ahead log\n");
    RUN(test_replay);
    RUN(test_uncommitted_discarded);
    RUN(test_truncate_upto);
    RUN(test_tail_truncation);
    RUN(test_mid_file_corruption);
    RUN(test_random_crash);
    return harness_report();
}
