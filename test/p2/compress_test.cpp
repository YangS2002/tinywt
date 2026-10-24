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
#include <random>
#include <string>
#include <vector>

#include "support/harness.hpp"
#include "tinywt/btree/btree.h"
#include "tinywt/compression/compress.h"

using namespace twt;

TWT_TEST(P2_Compress, RleRoundtrip, "P2/T2.7 RLE 往返")
{
    /* 高度重复的数据（压缩率应该明显） */
    std::vector<u8> rep(4000, 0xAB);
    for (size_t i = 2000; i < 4000; ++i)
        rep[i] = 0xCD;

    std::vector<u8> comp;
    ASSERT_EQ(S_OK, rle_compress(rep.data(), rep.size(), comp)) << "RLE 压缩失败";
    EXPECT_LT(comp.size(), rep.size()) << "高度重复的数据压不小，说明 RLE 没生效";

    std::vector<u8> back;
    ASSERT_EQ(S_OK, rle_decompress(comp.data(), comp.size(), back)) << "RLE 解压失败";
    EXPECT_TRUE(back == rep) << "往返之后字节不一致";

    /* 空输入 */
    std::vector<u8> empty;
    ASSERT_EQ(S_OK, rle_compress(nullptr, 0, comp)) << "空输入也必须成功";
    ASSERT_EQ(S_OK, rle_decompress(comp.data(), comp.size(), back));
    EXPECT_TRUE(back.empty());

    /* 单字节 */
    u8 one = 7;
    ASSERT_EQ(S_OK, rle_compress(&one, 1, comp));
    ASSERT_EQ(S_OK, rle_decompress(comp.data(), comp.size(), back));
    EXPECT_TRUE(back.size() == 1 && back[0] == 7);

    /* 随机数据：往返仍然要对（压缩率不保证） */
    std::mt19937 rng(20261022);
    std::vector<u8> rnd(1000);
    for (auto &b : rnd)
        b = (u8)(rng() & 0xff);
    ASSERT_EQ(S_OK, rle_compress(rnd.data(), rnd.size(), comp));
    ASSERT_EQ(S_OK, rle_decompress(comp.data(), comp.size(), back));
    EXPECT_TRUE(back == rnd) << "随机数据往返之后字节不一致";
}

TWT_TEST(P2_Compress, RleCorrupt, "P2/T2.7 损坏的 RLE 流必须报 CORRUPT")
{
    /* 裸的 byte 没有 count 前缀 → 解析不出头 */
    u8 bad1[1] = {0x41};
    std::vector<u8> out;
    EXPECT_EQ(S_CORRUPT, rle_decompress(bad1, 1, out)) << "半截记录必须当损坏，不能读越界";

    /* count 存在但 data 缺失 */
    u8 bad2[2] = {0x05, 0x00};
    EXPECT_EQ(S_CORRUPT, rle_decompress(bad2, 1, out)) << "count 声明了但数据不够，必须报 CORRUPT";
}

TWT_TEST(P2_Compress, PageCompression, "P2/T2.7 页压缩：RLE 建库后文件更小、数据一致")
{
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
        ASSERT_EQ(S_OK, a.open(pn, Config{}));
        ASSERT_TRUE(fill(a)) << "不压缩建库失败";
        ASSERT_EQ(S_OK, a.checkpoint());
        ASSERT_EQ(S_OK, a.close());
    }
    {
        Btree b;
        Config cfg;
        cfg.compression = Compression::RLE;
        ASSERT_EQ(S_OK, b.open(pr, cfg));
        ASSERT_TRUE(fill(b)) << "RLE 建库失败";
        ASSERT_EQ(S_OK, b.checkpoint());
        ASSERT_EQ(S_OK, b.close());
    }

    std::error_code ec1, ec2;
    auto sn = fs::file_size(pn, ec1);
    auto sr = fs::file_size(pr, ec2);
    EXPECT_FALSE(ec1);
    EXPECT_FALSE(ec2);
    std::printf(
      "  file size: none=%llu rle=%llu\n", (unsigned long long)sn, (unsigned long long)sr);
    EXPECT_LT(sr, sn) << "RLE 建出来的文件反而更大，说明压缩没落到块上";

    /* 解压后的数据必须完整 */
    {
        Btree b;
        Config cfg;
        cfg.compression = Compression::RLE;
        ASSERT_EQ(S_OK, b.open(pr, cfg));
        int bad = 0;
        for (int i = 0; i < N; ++i) {
            Val out;
            std::string want(300, (char)('a' + i % 3));
            if (b.get(Slice(kstr(i)), out) != S_OK ||
              Slice(out.data(), out.size()).cmp(Slice(want)) != 0)
                ++bad;
        }
        EXPECT_EQ(0, bad) << "解压后回读有 " << bad << " 个不对";
        EXPECT_EQ(S_OK, b.close());
    }

    rmpath(pn);
    rmpath(pr);
}
