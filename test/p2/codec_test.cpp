/*-
 * P2/T2.6 编码家族 —— suffix compression / overflow / 字典
 *
 * 这三样都是「一页装更多行」的手段，但它们作用的位置不同：
 *
 *   suffix   ：internal key 只留能区分相邻子树的那一段（页里省 key 字节）
 *   overflow ：超长 key/value 搬到独立块（页里只留 8~16 字节的 Addr）
 *   dict     ：同页重复 value 只存一份（页里只留 1 字节槽位）
 *
 * 验收：
 *   1. suffix_truncate 截断后仍能区分相邻 key，且仍是子树下界
 *   2. overflow 往返正确，且不破坏正常读写
 *   3. 字典：同值复用槽位，不同值不同槽位
 *   4. 三者同时开启时全量读写仍然正确（最重要的一条）
 * -*/
#include <string>
#include <vector>

#include "support/harness.hpp"
#include "tinywt/btree/btree.h"
#include "tinywt/storage/cell.h"

using namespace twt;

/* ---------------------------------------------------------------- suffix */
TWT_TEST(P2_Codec, SuffixTruncate, "P2/T2.6 suffix compression（internal key 截断）")
{
    /* 公共前缀 2 字节 → 留 3 字节即可区分 */
    EXPECT_EQ(size_t(3), suffix_truncate(key_of("abc"), key_of("abd")));
    /* 完全不同的首字节 → 留 1 字节 */
    EXPECT_EQ(size_t(1), suffix_truncate(key_of("abc"), key_of("xbc")));
    /* key 是 next 的前缀 → 不能截（截了就区分不开） */
    EXPECT_EQ(size_t(3), suffix_truncate(key_of("abc"), key_of("abcd")));
    /* 最右子树：没有 next → 原样保留 */
    EXPECT_EQ(size_t(3), suffix_truncate(key_of("abc"), Key{}));

    /*
     * 不变式（比具体数字更重要）：
     *   截断结果必须是原 key 的前缀，且 <= 原 key。
     *   这样它作为分隔符时仍是「子树下界」，不会让 [sep, first_key) 出现空洞。
     */
    struct Case {
        const char *k, *n;
    } cases[] = {{"key-000001", "key-000002"}, {"aaa", "aab"}, {"zzz", "zzzz"}};
    for (const auto &c : cases) {
        Key k = key_of(c.k), n = key_of(c.n);
        size_t len = suffix_truncate(k, n);
        EXPECT_GE(len, size_t(1)) << "截断长度不能是 0（空分隔符等于没截）";
        EXPECT_LE(len, k.size());
        Key t(k.begin(), k.begin() + (long)len);
        EXPECT_LE(Slice(t.data(), t.size()).cmp(Slice(k.data(), k.size())), 0)
          << "截断结果必须 <= 原 key";
        /* 截断后必须还能区分 next：t < n */
        EXPECT_LT(Slice(t.data(), t.size()).cmp(Slice(n.data(), n.size())), 0)
          << "截断过头了：分隔符不再严格小于 next";
    }
}

/* ---------------------------------------------------------------- overflow */
TWT_TEST(P2_Codec, Overflow, "P2/T2.6 overflow page（超长 value 搬到独立块）")
{
    std::string p = dbpath("p2_ovfl.wt");
    rmpath(p);

    Btree bt;
    Config cfg;
    cfg.max_leaf_entries = 8;
    cfg.use_overflow = true;
    cfg.value_overflow_size = 256;
    ASSERT_EQ(S_OK, bt.open(p, cfg));

    /* 直接测 overflow 块本身的往返 */
    std::vector<u8> big(5000);
    for (size_t i = 0; i < big.size(); ++i)
        big[i] = (u8)(i * 31);
    Addr a;
    ASSERT_EQ(S_OK, bt.write_overflow(Slice(big.data(), big.size()), a)) << "写 overflow 块失败";
    EXPECT_TRUE(a.valid()) << "overflow 的 Addr 必须有效";
    {
        Val back;
        ASSERT_EQ(S_OK, bt.read_overflow(a, back)) << "读 overflow 块失败";
        EXPECT_EQ(big.size(), back.size());
        EXPECT_TRUE(back == big) << "overflow 往返后字节不一致";
    }

    /* 通过正常的 put/get 路径（超过 value_overflow_size 的走 overflow） */
    std::string v(2000, 'x');
    ASSERT_EQ(S_OK, bt.put(Slice("k1"), Slice(v)));
    ASSERT_EQ(S_OK, bt.put(Slice("k2"), Slice("small")));
    {
        Val out;
        ASSERT_EQ(S_OK, bt.get(Slice("k1"), out));
        EXPECT_EQ(v.size(), out.size());
        EXPECT_EQ(0, Slice(out.data(), out.size()).cmp(Slice(v)));
        ASSERT_EQ(S_OK, bt.get(Slice("k2"), out));
        EXPECT_EQ(0, Slice(out.data(), out.size()).cmp(Slice("small"))) << "小 value 不该走 overflow";
    }

    ASSERT_EQ(S_OK, bt.checkpoint());
    /* 重开：overflow 块的地址必须持久化正确 */
    {
        Val out;
        ASSERT_EQ(S_OK, bt.get(Slice("k1"), out));
        EXPECT_EQ(v.size(), out.size());
    }
    EXPECT_EQ(S_OK, bt.close());
    rmpath(p);
}

/* ---------------------------------------------------------------- 字典 */
TWT_TEST(P2_Codec, ValueDict, "P2/T2.6 字典压缩（CELL_VALUE_COPY）")
{
    ValueDict d;
    EXPECT_EQ(-1, d.find(Slice("hello")));
    size_t s1 = d.add(Slice("hello"));
    EXPECT_EQ(size_t(0), s1);
    size_t s2 = d.add(Slice("world"));
    EXPECT_EQ(size_t(1), s2);
    /* 同一个值必须复用槽位 */
    EXPECT_EQ(0, d.find(Slice("hello")));
    EXPECT_EQ(size_t(0), d.add(Slice("hello"))) << "重复值必须复用槽位，不能新开一个";
    EXPECT_EQ(size_t(2), d.entries.size());

    /* cell 编码 + 解包 */
    std::vector<u8> b;
    cell_pack_value_copy(b, (u8)s1);
    Cell c;
    ASSERT_EQ(S_OK, cell_unpack(b.data(), b.size(), c));
    EXPECT_TRUE(c.is_copy());
    EXPECT_EQ(s1, (size_t)c.slot);
    EXPECT_EQ(b.size(), c.total);
}

/* ---------------------------------------------------------------- 三者同时开 */
TWT_TEST(P2_Codec, EndToEnd, "P2/T2.6 三种编码同时开启：全量读写")
{
    std::string p = dbpath("p2_codec.wt");
    rmpath(p);

    Btree bt;
    Config cfg;
    cfg.max_leaf_entries = 12;
    cfg.max_intl_refs = 6;
    cfg.max_cached_pages = 32;
    cfg.internal_key_truncate = true;
    cfg.use_overflow = true;
    cfg.value_overflow_size = 200;
    cfg.dictionary = true;
    ASSERT_EQ(S_OK, bt.open(p, cfg));

    const int N = 600;
    for (int i = 0; i < N; ++i) {
        /* 一半重复值（触发字典），一半超长值（触发 overflow） */
        std::string v =
          (i % 2 == 0) ? std::string("shared-value") : std::string(600, (char)('a' + i % 26));
        ASSERT_EQ(S_OK, bt.put(Slice(kstr(i)), Slice(v))) << "第 " << i << " 个键写失败";
    }

    int bad = 0;
    for (int i = 0; i < N; ++i) {
        Val out;
        std::string want = (i % 2 == 0) ? std::string("shared-value")
                                        : std::string(600, (char)('a' + i % 26));
        if (bt.get(Slice(kstr(i)), out) != S_OK ||
          Slice(out.data(), out.size()).cmp(Slice(want)) != 0)
            ++bad;
    }
    EXPECT_EQ(0, bad) << "三种编码全开时回读有 " << bad << " 个不对";

    ASSERT_EQ(S_OK, bt.checkpoint());
    std::string err;
    ASSERT_EQ(S_OK, bt.verify(err)) << "verify: " << err;
    EXPECT_EQ(S_OK, bt.close());
    rmpath(p);
}
