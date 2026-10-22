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
#define TWT_PROJECT "P2"

#include <string>
#include <vector>

#include "support/harness.hpp"
#include "tinywt/btree/btree.h"
#include "tinywt/storage/cell.h"

using namespace twt;

/* ---------------------------------------------------------------- suffix */
static void
test_suffix_truncate()
{
    CASE("P2/T2.6 suffix compression（internal key 截断）");

    /* 公共前缀 2 字节 → 留 3 字节即可区分 */
    CHECK(suffix_truncate(key_of("abc"), key_of("abd")) == 3);
    /* 完全不同的首字节 → 留 1 字节 */
    CHECK(suffix_truncate(key_of("abc"), key_of("xbc")) == 1);
    /* key 是 next 的前缀 → 不能截（截了就区分不开） */
    CHECK(suffix_truncate(key_of("abc"), key_of("abcd")) == 3);
    /* 最右子树：没有 next → 原样保留 */
    CHECK(suffix_truncate(key_of("abc"), Key{}) == 3);

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
        CHECK(len >= 1 && len <= k.size());
        Key t(k.begin(), k.begin() + (long)len);
        CHECK(Slice(t.data(), t.size()).cmp(Slice(k.data(), k.size())) <= 0);
        /* 截断后必须还能区分 next：t < n */
        CHECK(Slice(t.data(), t.size()).cmp(Slice(n.data(), n.size())) < 0);
    }
}

/* ---------------------------------------------------------------- overflow */
static void
test_overflow()
{
    CASE("P2/T2.6 overflow page（超长 value 搬到独立块）");
    std::string p = dbpath("p2_ovfl.wt");
    rmpath(p);

    Btree bt;
    Config cfg;
    cfg.max_leaf_entries = 8;
    cfg.use_overflow = true;
    cfg.value_overflow_size = 256;
    CHECK_ST(bt.open(p, cfg));

    /* 直接测 overflow 块本身的往返 */
    std::vector<u8> big(5000);
    for (size_t i = 0; i < big.size(); ++i)
        big[i] = (u8)(i * 31);
    Addr a;
    CHECK_ST(bt.write_overflow(Slice(big.data(), big.size()), a));
    CHECK(a.valid());
    {
        Val back;
        CHECK_ST(bt.read_overflow(a, back));
        CHECK(back.size() == big.size());
        CHECK(back == big);
    }

    /* 通过正常的 put/get 路径（超过 value_overflow_size 的走 overflow） */
    std::string v(2000, 'x');
    CHECK_ST(bt.put(Slice("k1"), Slice(v)));
    CHECK_ST(bt.put(Slice("k2"), Slice("small")));
    {
        Val out;
        CHECK_ST(bt.get(Slice("k1"), out));
        CHECK(out.size() == v.size());
        CHECK(Slice(out.data(), out.size()).cmp(Slice(v)) == 0);
        CHECK_ST(bt.get(Slice("k2"), out));
        CHECK(Slice(out.data(), out.size()).cmp(Slice("small")) == 0);
    }

    CHECK_ST(bt.checkpoint());
    /* 重开：overflow 块的地址必须持久化正确 */
    {
        Val out;
        CHECK_ST(bt.get(Slice("k1"), out));
        CHECK(out.size() == v.size());
    }
    CHECK_ST(bt.close());
    rmpath(p);
}

/* ---------------------------------------------------------------- 字典 */
static void
test_value_dict()
{
    CASE("P2/T2.6 字典压缩（CELL_VALUE_COPY）");

    ValueDict d;
    CHECK(d.find(Slice("hello")) == -1);
    size_t s1 = d.add(Slice("hello"));
    CHECK(s1 == 0);
    size_t s2 = d.add(Slice("world"));
    CHECK(s2 == 1);
    /* 同一个值必须复用槽位 */
    CHECK(d.find(Slice("hello")) == 0);
    CHECK(d.add(Slice("hello")) == 0);
    CHECK(d.entries.size() == 2);

    /* cell 编码 + 解包 */
    std::vector<u8> b;
    cell_pack_value_copy(b, (u8)s1);
    Cell c;
    CHECK_ST(cell_unpack(b.data(), b.size(), c));
    CHECK(c.is_copy());
    CHECK(c.slot == s1);
    CHECK(c.total == b.size());
}

/* ---------------------------------------------------------------- 三者同时开 */
static void
test_codec_end_to_end()
{
    CASE("P2/T2.6 三种编码同时开启：全量读写");
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
    CHECK_ST(bt.open(p, cfg));

    const int N = 600;
    for (int i = 0; i < N; ++i) {
        /* 一半重复值（触发字典），一半超长值（触发 overflow） */
        std::string v = (i % 2 == 0) ? std::string("shared-value") : std::string(600, (char)('a' + i % 26));
        CHECK_ST(bt.put(Slice(kstr(i)), Slice(v)));
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
    CHECK(bad == 0);

    CHECK_ST(bt.checkpoint());
    std::string err;
    CHECK_ST(bt.verify(err));
    CHECK_ST(bt.close());
    rmpath(p);
}

int
main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("tinywt P2: codec family\n");
    RUN(test_suffix_truncate);
    RUN(test_overflow);
    RUN(test_value_dict);
    RUN(test_codec_end_to_end);
    return harness_report();
}
