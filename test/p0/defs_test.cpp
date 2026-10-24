/*-
 * P0 —— 基础件热身（对标 BusTub P0: C++ Primer）
 *
 *   T0.1  错误码契约：internal Status 必须与真 wiredtiger.h 的 WT_* 数值一致
 *   T0.2  varint（LEB128）
 *   T0.3  crc32
 *
 * 这一项不需要写代码，只要读懂「为什么错误码必须逐位对齐」：
 * 真 wtperf 会拿返回值直接和 WT_NOTFOUND 比，错一个数就把「键不存在」
 * 当成「事务冲突」。
 * -*/
#include "support/harness.hpp"
#include "wiredtiger.h"

using namespace twt;

TWT_TEST(P0_Defs, ErrorContract, "P0/T0.1 构建骨架 + 错误码契约")
{
    EXPECT_EQ((int)S_ROLLBACK, WT_ROLLBACK);
    EXPECT_EQ((int)S_DUPLICATE, WT_DUPLICATE_KEY);
    EXPECT_EQ((int)S_ERROR, WT_ERROR);
    EXPECT_EQ((int)S_NOTFOUND, WT_NOTFOUND);
    EXPECT_EQ((int)S_PANIC, WT_PANIC);
    EXPECT_EQ((int)S_RESTART, WT_RESTART);

    /* S_OK 必须是 0：C ABI 用 0 表示成功，这点不能妥协 */
    EXPECT_EQ((int)S_OK, 0);

    /* 最容易踩的坑：NOTFOUND 和 ROLLBACK 只差 3，混了以后错误处理全乱 */
    EXPECT_NE(WT_NOTFOUND, WT_ROLLBACK);
}

TWT_TEST(P0_Defs, Varint, "P0/T0.2 varint (LEB128)")
{
    const u64 vals[] = {0, 1, 63, 64, 127, 128, 300, 65535, 1ULL << 20, (1ULL << 40) + 7};
    for (u64 v : vals) {
        u8 buf[16];
        const size_t n = pack_uint(buf, v);
        EXPECT_EQ(uint_size(v), n) << "value=" << v;
        u64 out = 0;
        const size_t k = unpack_uint(buf, n, out);
        EXPECT_EQ(n, k) << "value=" << v;
        EXPECT_EQ(v, out) << "value=" << v;
    }

    /* 边界：0 也必须占 1 字节（否则 uint_size 会返回 0 → 死循环/错位） */
    EXPECT_EQ(size_t(1), uint_size(0));
    /* 越界读：只给一半的字节，必须返回 0 而不是猜 */
    u8 two[2] = {0x80, 0x80};
    u64 dummy = 0;
    EXPECT_EQ(size_t(0), unpack_uint(two, 2, dummy));
}

TWT_TEST(P0_Defs, Crc32, "P0/T0.2 crc32")
{
    const char *s = "123456789";
    /* CRC-32/IEEE 标准校验值 */
    EXPECT_EQ(0xCBF43926u, crc32(reinterpret_cast<const u8 *>(s), 9));
    u8 a[4] = {1, 2, 3, 4};
    u8 b[4] = {1, 2, 3, 5};
    EXPECT_NE(crc32(a, 4), crc32(b, 4));
    /* 空输入也要有确定值（0） */
    EXPECT_EQ(0u, crc32(a, 0));
}
