/*-
 * P1 —— Block Manager（对标 BusTub P1 Buffer Pool Manager：先造底座）
 *
 *   Task 1.1  块读写 + BlockHeader + CRC 损坏检测
 *   Task 1.2  extent 三表（alloc/avail/discard）+ 超级块 + addr cookie
 *   Task 1.3  cell 编解码 + key 前缀压缩
 * -*/
#define TWT_PROJECT "P1"

#include <cstring>
#include <vector>

#include "harness.hpp"
#include "twt_block.h"
#include "twt_cell.h"

using namespace twt;

/* ---------------------------------------------------------------- T1.3 */
static void
test_cell()
{
    CASE("P1/T1.3 cell 编解码 (对标 src/include/cell.h)");

    /* short key，无前缀 */
    {
        std::vector<u8> b;
        std::string k = "hello";
        cell_pack_key(b, Slice(k), 0);
        CHECK(b[0] == (CELL_KEY_SHORT | (u8)(k.size() << CELL_SHORT_SHIFT)));
        Cell c;
        CHECK_ST(cell_unpack(b.data(), b.size(), c));
        CHECK(c.is_key());
        CHECK(c.prefix == 0);
        CHECK(c.data.str() == "hello");
        CHECK(c.total == b.size());
    }

    /* 长 key（> 63），带前缀压缩 */
    {
        std::string prev(100, 'a');
        std::string cur = prev + std::string(50, 'b');
        std::vector<u8> b;
        cell_pack_key(b, Slice(cur), prev.size());
        Cell c;
        CHECK_ST(cell_unpack(b.data(), b.size(), c));
        CHECK(c.prefix == prev.size());
        CHECK(c.data.size() == 50);
        CHECK(c.total == b.size());
        /* 还原整键 */
        Key full(prev.begin(), prev.end());
        full.insert(full.end(), c.data.data(), c.data.data() + c.data.size());
        CHECK(Slice(full.data(), full.size()).cmp(Slice(cur)) == 0);
    }

    /* short value / long value */
    {
        std::vector<u8> b;
        std::string v = "v";
        cell_pack_value(b, Slice(v));
        Cell c;
        CHECK_ST(cell_unpack(b.data(), b.size(), c));
        CHECK(c.is_value());
        CHECK(c.data.str() == "v");

        std::string big(5000, 'x');
        b.clear();
        cell_pack_value(b, Slice(big));
        CHECK_ST(cell_unpack(b.data(), b.size(), c));
        CHECK(c.data.size() == 5000);
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
        CHECK_ST(cell_unpack(b.data(), b.size(), c));
        CHECK(c.is_addr());
        size_t i = 0;
        u64 v = 0;
        i += unpack_uint(c.data.data() + i, c.data.size() - i, v);
        CHECK(v == 4096);
        i += unpack_uint(c.data.data() + i, c.data.size() - i, v);
        CHECK(v == 700);
        i += unpack_uint(c.data.data() + i, c.data.size() - i, v);
        CHECK(v == 8192);
        i += unpack_uint(c.data.data() + i, c.data.size() - i, v);
        CHECK(v == 0xDEADBEEFu);
        i += unpack_uint(c.data.data() + i, c.data.size() - i, v);
        CHECK(v == 77);
    }
}

/* ---------------------------------------------------------------- T1.1 + T1.2 */
static void
test_block()
{
    CASE("P1/T1.1 块读写 + 校验");
    std::string p = dbpath("p1_block.wt");
    rmpath(p);

    BlockManager bm;
    CHECK_ST(bm.open(p, 4096));

    /* offset 0 必须是非法偏移：首块留给描述块 */
    std::vector<u8> img(1000, 0x5A);
    Addr a1, a2;
    CHECK_ST(bm.write(img.data(), img.size(), a1));
    CHECK(a1.off != 0);
    CHECK(a1.alloc % 4096 == 0); /* 分配单元对齐 */
    CHECK(a1.size == img.size()); /* 镜像长度是精确值 */
    CHECK(a1.cksum == crc32(img.data(), img.size()));

    std::vector<u8> back;
    CHECK_ST(bm.read(a1, back));
    CHECK(back.size() == img.size());
    CHECK(std::memcmp(back.data(), img.data(), img.size()) == 0);

    /* 故意用错误的 cksum 读 → 必须报 CORRUPT */
    Addr bad = a1;
    bad.cksum ^= 0xFFu;
    CHECK(bm.read(bad, back) == S_CORRUPT);

    /* 分配第二个块，偏移必须不同且不重叠 */
    std::vector<u8> img2(5000, 0x33);
    CHECK_ST(bm.write(img2.data(), img2.size(), a2));
    CHECK(a2.off != a1.off);
    CHECK((a2.off >= a1.off + a1.alloc) || (a1.off >= a2.off + a2.alloc));

    CHECK_ST(bm.close());
    rmpath(p);
}

/* ---------------------------------------------------------------- T1.2 */
static void
test_extent_and_super()
{
    CASE("P1/T1.2 extent + 超级块");
    std::string p = dbpath("p1_block.wt");
    rmpath(p);

    BlockManager bm;
    CHECK_ST(bm.open(p, 4096));

    std::vector<u8> img(1000, 0x5A);
    Addr a1, a2;
    CHECK_ST(bm.write(img.data(), img.size(), a1));
    std::vector<u8> img2(5000, 0x33);
    CHECK_ST(bm.write(img2.data(), img2.size(), a2));

    /* 释放 → 进 discard，checkpoint 后回到 avail */
    CHECK_ST(bm.free_block(a1));
    CHECK(bm.discard().size() == 1);
    std::vector<Extent> e;
    CHECK_ST(bm.checkpoint_extents(e));
    CHECK(bm.discard().empty());
    CHECK(bm.avail_bytes() > 0);

    /* 超级块往返 */
    SuperBlock sb;
    sb.root_page_id = 42;
    sb.next_page_id = 43;
    sb.root_addr = a2;
    sb.file_size = bm.file_size();
    sb.avail = e;
    CHECK_ST(bm.write_super(sb));

    SuperBlock sb2;
    sb2.alloc_size = 4096;
    CHECK_ST(bm.read_super(sb2));
    CHECK(sb2.root_page_id == 42);
    CHECK(sb2.next_page_id == 43);
    CHECK(sb2.root_addr.off == a2.off);

    CHECK_ST(bm.close());
    rmpath(p);
}

int
main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("tinywt P1: block manager + cell\n");
    RUN(test_block);
    RUN(test_extent_and_super);
    RUN(test_cell);
    return harness_report();
}
