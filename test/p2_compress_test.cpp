/*-
 * P2/T2.7 页压缩 —— 内建 RLE
 *
 * 压缩位置：**块写入之前**。所以它和 cell 格式无关，
 * 正确的判据只有两条：往返一致、文件真的变小。
 *
 * 验收：
 *   1. RLE 往返（含空串、单字节、随机数据）
 *   2. 损坏的 RLE 流返回 S_CORRUPT，不读越界
 *   3. 同一份数据分别用 NONE / RLE 建库，RLE 的文件更小且数据一致
 * -*/
#define TWT_PROJECT "P2"

#include <random>
#include <string>
#include <vector>

#include "harness.hpp"
#include "twt_btree.h"
#include "twt_compress.h"

using namespace twt;

static void
test_rle_roundtrip()
{
    CASE("P2/T2.7 RLE 往返");
    /* 高度重复的数据（压缩率应该明显） */
    std::vector<u8> rep(4000, 0xAB);
    for (size_t i = 2000; i < 4000; ++i)
        rep[i] = 0xCD;

    std::vector<u8> comp;
    CHECK_ST(rle_compress(rep.data(), rep.size(), comp));
    CHECK(comp.size() < rep.size());

    std::vector<u8> back;
    CHECK_ST(rle_decompress(comp.data(), comp.size(), back));
    CHECK(back == rep);

    /* 空输入 */
    std::vector<u8> empty;
    CHECK_ST(rle_compress(nullptr, 0, comp));
    CHECK_ST(rle_decompress(comp.data(), comp.size(), back));
    CHECK(back.empty());

    /* 单字节 */
    u8 one = 7;
    CHECK_ST(rle_compress(&one, 1, comp));
    CHECK_ST(rle_decompress(comp.data(), comp.size(), back));
    CHECK(back.size() == 1 && back[0] == 7);

    /* 随机数据：往返仍然要对（压缩率不保证） */
    std::mt19937 rng(20261022);
    std::vector<u8> rnd(1000);
    for (auto &b : rnd)
        b = (u8)(rng() & 0xff);
    CHECK_ST(rle_compress(rnd.data(), rnd.size(), comp));
    CHECK_ST(rle_decompress(comp.data(), comp.size(), back));
    CHECK(back == rnd);
}

static void
test_rle_corrupt()
{
    CASE("P2/T2.7 损坏的 RLE 流必须报 CORRUPT");
    /* 裸的 byte 没有 count 前缀 → 解析不出头 */
    u8 bad1[1] = {0x41};
    std::vector<u8> out;
    CHECK(rle_decompress(bad1, 1, out) == S_CORRUPT);

    /* count 存在但 data 缺失 */
    u8 bad2[2] = {0x05, 0x00};
    CHECK(rle_decompress(bad2, 1, out) == S_CORRUPT);
}

static void
test_page_compression()
{
    CASE("P2/T2.7 页压缩：RLE 建库后文件更小、数据一致");
    std::string pn = dbpath("p2_cmp_none.wt");
    std::string pr = dbpath("p2_cmp_rle.wt");
    rmpath(pn);
    rmpath(pr);

    const int N = 400;

    auto fill = [](Btree &bt) {
        for (int i = 0; i < N; ++i) {
            /* 值高度重复，RLE 应该压得动 */
            std::string v(300, (char)('a' + i % 3));
            if (bt.put(Slice(kstr(i)), Slice(v)) != S_OK)
                return false;
        }
        return true;
    };

    {
        Btree a;
        CHECK_ST(a.open(pn, Config{}));
        CHECK(fill(a));
        CHECK_ST(a.checkpoint());
        CHECK_ST(a.close());
    }
    {
        Btree b;
        Config cfg;
        cfg.compression = Compression::RLE;
        CHECK_ST(b.open(pr, cfg));
        CHECK(fill(b));
        CHECK_ST(b.checkpoint());
        CHECK_ST(b.close());
    }

    std::error_code ec1, ec2;
    auto sn = fs::file_size(pn, ec1);
    auto sr = fs::file_size(pr, ec2);
    CHECK(!ec1 && !ec2);
    printf("  file size: none=%llu rle=%llu\n", (unsigned long long)sn, (unsigned long long)sr);
    CHECK(sr < sn);

    /* 解压后的数据必须完整 */
    {
        Btree b;
        Config cfg;
        cfg.compression = Compression::RLE;
        CHECK_ST(b.open(pr, cfg));
        int bad = 0;
        for (int i = 0; i < N; ++i) {
            Val out;
            std::string want(300, (char)('a' + i % 3));
            if (b.get(Slice(kstr(i)), out) != S_OK ||
              Slice(out.data(), out.size()).cmp(Slice(want)) != 0)
                ++bad;
        }
        CHECK(bad == 0);
        CHECK_ST(b.close());
    }

    rmpath(pn);
    rmpath(pr);
}

int
main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("tinywt P2: page compression\n");
    RUN(test_rle_roundtrip);
    RUN(test_rle_corrupt);
    RUN(test_page_compression);
    return harness_report();
}
