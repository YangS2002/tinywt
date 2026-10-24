/*-
 * P1 —— Block Manager（对标 BusTub P1 Buffer Pool Manager：先造底座）
 *
 *   Task 1.1  块读写 + BlockHeader + CRC 损坏检测
 *   Task 1.2  extent 三表（alloc/avail/discard）+ 超级块 + addr cookie
 *   Task 1.3  cell 编解码 + key 前缀压缩
 * -*/
#include <cstring>
#include <vector>

#include "support/harness.hpp"
#include "tinywt/storage/block.h"
#include "tinywt/storage/cell.h"

using namespace twt;

/* ---------------------------------------------------------------- T1.3 */
TWT_TEST(P1_BlockCell, CellCodec, "P1/T1.3 cell 编解码 (对标 src/include/cell.h)")
{
    /* short key，无前缀 */
    {
        std::vector<u8> b;
        std::string k = "hello";
        cell_pack_key(b, Slice(k), 0);
        EXPECT_EQ((u8)(CELL_KEY_SHORT | (u8)(k.size() << CELL_SHORT_SHIFT)), b[0]);
        Cell c;
        ASSERT_EQ(S_OK, cell_unpack(b.data(), b.size(), c)) << "short key 解不开";
        EXPECT_TRUE(c.is_key());
        EXPECT_EQ(size_t(0), c.prefix);
        EXPECT_EQ("hello", c.data.str());
        EXPECT_EQ(b.size(), c.total);
    }

    /* 长 key（> 63），带前缀压缩 */
    {
        std::string prev(100, 'a');
        std::string cur = prev + std::string(50, 'b');
        std::vector<u8> b;
        cell_pack_key(b, Slice(cur), prev.size());
        Cell c;
        ASSERT_EQ(S_OK, cell_unpack(b.data(), b.size(), c)) << "长 key 解不开";
        EXPECT_EQ(prev.size(), c.prefix);
        EXPECT_EQ(size_t(50), c.data.size());
        EXPECT_EQ(b.size(), c.total);
        /* 还原整键 */
        Key full(prev.begin(), prev.end());
        full.insert(full.end(), c.data.data(), c.data.data() + c.data.size());
        EXPECT_EQ(0, Slice(full.data(), full.size()).cmp(Slice(cur))) << "前缀还原后的整键不对";
    }

    /* short value / long value */
    {
        std::vector<u8> b;
        std::string v = "v";
        cell_pack_value(b, Slice(v));
        Cell c;
        ASSERT_EQ(S_OK, cell_unpack(b.data(), b.size(), c));
        EXPECT_TRUE(c.is_value());
        EXPECT_EQ("v", c.data.str());

        std::string big(5000, 'x');
        b.clear();
        cell_pack_value(b, Slice(big));
        ASSERT_EQ(S_OK, cell_unpack(b.data(), b.size(), c));
        EXPECT_EQ(size_t(5000), c.data.size());
    }

    /* addr cell */
    {
        std::vector<u8> b;
        Addr a;
        a.off = 4096;
        a.size = 700;
        a.alloc = 8192;
        a.cksum = 0xDEADBEEFu;
        cell_pack_addr(b, a, 77, CELL_ADDR_LEAF_NO);
        Cell c;
        ASSERT_EQ(S_OK, cell_unpack(b.data(), b.size(), c));
        EXPECT_TRUE(c.is_addr());
        size_t i = 0;
        u64 v = 0;
        i += unpack_uint(c.data.data() + i, c.data.size() - i, v);
        EXPECT_EQ(u64(4096), v);
        i += unpack_uint(c.data.data() + i, c.data.size() - i, v);
        EXPECT_EQ(u64(700), v);
        i += unpack_uint(c.data.data() + i, c.data.size() - i, v);
        EXPECT_EQ(u64(8192), v);
        i += unpack_uint(c.data.data() + i, c.data.size() - i, v);
        EXPECT_EQ(u64(0xDEADBEEFu), v);
        i += unpack_uint(c.data.data() + i, c.data.size() - i, v);
        EXPECT_EQ(u64(77), v);
    }
}

/* ---------------------------------------------------------------- T1.1 */
TWT_TEST(P1_BlockCell, BlockIO, "P1/T1.1 块读写 + 校验")
{
    std::string p = dbpath("p1_block.wt");
    rmpath(p);

    BlockManager bm;
    ASSERT_EQ(S_OK, bm.open(p, 4096)) << "BlockManager::open 失败";

    /* offset 0 必须是非法偏移：首块留给描述块 */
    std::vector<u8> img(1000, 0x5A);
    Addr a1, a2;
    ASSERT_EQ(S_OK, bm.write(img.data(), img.size(), a1)) << "第一次写块失败";
    EXPECT_NE(0u, a1.off) << "offset 0 是非法偏移（首块留给描述块）";
    EXPECT_EQ(0u, a1.alloc % 4096) << "分配单元必须按 alloc_size 对齐";
    EXPECT_EQ(img.size(), a1.size) << "Addr::size 是镜像精确长度";
    EXPECT_EQ(crc32(img.data(), img.size()), a1.cksum) << "cksum 不是对镜像算的 crc32";

    std::vector<u8> back;
    ASSERT_EQ(S_OK, bm.read(a1, back)) << "刚写的块读不回来";
    EXPECT_EQ(img.size(), back.size());
    EXPECT_EQ(0, std::memcmp(back.data(), img.data(), img.size())) << "读回来的字节和写进去的不一样";

    /* 故意用错误的 cksum 读 → 必须报 CORRUPT */
    Addr bad = a1;
    bad.cksum ^= 0xFFu;
    EXPECT_EQ(S_CORRUPT, bm.read(bad, back)) << "校验和对不上必须报 CORRUPT";

    /* 分配第二个块，偏移必须不同且不重叠 */
    std::vector<u8> img2(5000, 0x33);
    ASSERT_EQ(S_OK, bm.write(img2.data(), img2.size(), a2)) << "第二次写块失败";
    EXPECT_NE(a1.off, a2.off);
    EXPECT_TRUE((a2.off >= a1.off + a1.alloc) || (a1.off >= a2.off + a2.alloc))
      << "两个块重叠了：a1=[" << a1.off << "," << a1.off + a1.alloc << ") a2=[" << a2.off << ","
      << a2.off + a2.alloc << ")";

    EXPECT_EQ(S_OK, bm.close());
    rmpath(p);
}

/* ---------------------------------------------------------------- T1.2 */
TWT_TEST(P1_BlockCell, ExtentSuper, "P1/T1.2 extent + 超级块")
{
    std::string p = dbpath("p1_block.wt");
    rmpath(p);

    BlockManager bm;
    ASSERT_EQ(S_OK, bm.open(p, 4096)) << "BlockManager::open 失败";

    std::vector<u8> img(1000, 0x5A);
    Addr a1, a2;
    ASSERT_EQ(S_OK, bm.write(img.data(), img.size(), a1));
    std::vector<u8> img2(5000, 0x33);
    ASSERT_EQ(S_OK, bm.write(img2.data(), img2.size(), a2));

    /* 释放 → 进 discard，checkpoint 后回到 avail */
    ASSERT_EQ(S_OK, bm.free_block(a1));
    EXPECT_EQ(size_t(1), bm.discard().size()) << "释放的块应先落到 discard 表";
    std::vector<Extent> e;
    ASSERT_EQ(S_OK, bm.checkpoint_extents(e));
    EXPECT_TRUE(bm.discard().empty()) << "checkpoint 之后 discard 必须清空";
    EXPECT_GT(bm.avail_bytes(), 0u) << "被释放的 extent 应回到 avail";

    /* 超级块往返 */
    SuperBlock sb;
    sb.root_page_id = 42;
    sb.next_page_id = 43;
    sb.root_addr = a2;
    sb.file_size = bm.file_size();
    sb.avail = e;
    ASSERT_EQ(S_OK, bm.write_super(sb)) << "写超级块失败";

    SuperBlock sb2;
    sb2.alloc_size = 4096;
    ASSERT_EQ(S_OK, bm.read_super(sb2)) << "读超级块失败";
    EXPECT_EQ(u64(42), sb2.root_page_id);
    EXPECT_EQ(u64(43), sb2.next_page_id);
    EXPECT_EQ(a2.off, sb2.root_addr.off);

    EXPECT_EQ(S_OK, bm.close());
    rmpath(p);
}
