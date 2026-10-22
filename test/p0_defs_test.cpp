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
#define TWT_PROJECT "P0"

#include "harness.hpp"
#include "wiredtiger.h"

using namespace twt;

static void
test_error_contract()
{
    CASE("P0/T0.1 错误码契约（Status vs WT_*）");

    CHECK((int)S_ROLLBACK == WT_ROLLBACK);
    CHECK((int)S_DUPLICATE == WT_DUPLICATE_KEY);
    CHECK((int)S_ERROR == WT_ERROR);
    CHECK((int)S_NOTFOUND == WT_NOTFOUND);
    CHECK((int)S_PANIC == WT_PANIC);
    CHECK((int)S_RESTART == WT_RESTART);

    /* S_OK 必须是 0：C ABI 用 0 表示成功，这点不能妥协 */
    CHECK((int)S_OK == 0);

    /* 最容易踩的坑：NOTFOUND 和 ROLLBACK 只差 3，混了以后错误处理全乱 */
    CHECK(WT_NOTFOUND != WT_ROLLBACK);
}

static void
test_varint()
{
    CASE("P0/T0.2 varint (LEB128)");
    u64 vals[] = {0, 1, 63, 64, 127, 128, 300, 65535, 1ULL << 20, (1ULL << 40) + 7};
    for (u64 v : vals) {
        u8 buf[16];
        size_t n = pack_uint(buf, v);
        CHECK(n == uint_size(v));
        u64 out = 0;
        size_t k = unpack_uint(buf, n, out);
        CHECK(k == n);
        CHECK(out == v);
    }

    /* 边界：0 也必须占 1 字节（否则 uint_size 会返回 0 → 死循环/错位） */
    CHECK(uint_size(0) == 1);
    /* 越界读：只给一半的字节，必须返回 0 而不是猜 */
    u8 two[2] = {0x80, 0x80};
    u64 dummy = 0;
    CHECK(unpack_uint(two, 2, dummy) == 0);
}

static void
test_crc32()
{
    CASE("P0/T0.3 crc32");
    const char *s = "123456789";
    /* CRC-32/IEEE 标准校验值 */
    CHECK(crc32(reinterpret_cast<const u8 *>(s), 9) == 0xCBF43926u);
    u8 a[4] = {1, 2, 3, 4};
    u8 b[4] = {1, 2, 3, 5};
    CHECK(crc32(a, 4) != crc32(b, 4));
    /* 空输入也要有确定值（0） */
    CHECK(crc32(a, 0) == 0);
}

int
main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("tinywt P0: 基础件\n");
    RUN(test_error_contract);
    RUN(test_varint);
    RUN(test_crc32);
    return harness_report();
}
