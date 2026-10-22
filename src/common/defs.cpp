/*-
 * src/common/defs.cpp —— P0 作业：基础件
 *
 * 对标真 WT 的文件：
 *   intpack_inline.h : __wt_vpack_uint / __wt_vunpack_uint / __wt_vsize_uint
 *   checksum 目录     : __wt_checksum_sw / __wt_crc32c
 *
 * 规则（全过程适用）：
 *   1. 只改标了 TODO 的函数体，别动签名、别动头文件里的结构体布局。
 *   2. 函数体里现在只有一行 UNIMPLEMENTED(...)；把它换成你的实现。
 *   3. 不要为了让测试变绿去改测试，也不要把 UNIMPLEMENTED 留在被调用的路径上。
 * -*/

#include "tinywt/common/defs.h"

namespace twt {

/*
 * pack_uint --
 *     把 v 按 LEB128 写入 dst，返回写入字节数（1..10）。
 *     dst 至少要有 10 字节空间。
 *
 *     真 WT: __wt_vpack_uint() —— 格式细节与 LEB128 不同，
 *     但「小整数短、大整数长、前缀无关」这个性质是一样的。
 *
 *     提示：每轮取低 7 位，剩下的右移 7 位；还有剩余就置 0x80 继续。
 */
size_t
pack_uint(u8 *dst, u64 v)
{
    UNIMPLEMENTED("P0/T0.2 pack_uint");
}

/*
 * uint_size --
 *     返回 pack_uint 会写入多少字节（不实际写）。
 *     常见错误：用 while(v>>=7) 写成 do-while 漏掉 v==0 的情况（0 也要占 1 字节）。
 */
size_t
uint_size(u64 v)
{
    UNIMPLEMENTED("P0/T0.2 uint_size");
}

/*
 * unpack_uint --
 *     从 src 解出 v，返回消耗的字节数；数据不完整 / 溢出（>64 位）返回 0。
 *     avail 是 src 可读的字节数，绝不能越过它读。
 *
 *     真 WT: __wt_vunpack_uint()
 *
 *     注意：最后一组如果 shift 已经 >= 64，说明编码超过 64 位，必须返回 0，
 *     否则会产生未定义行为（移位超过位宽）。
 */
size_t
unpack_uint(const u8 *src, size_t avail, u64 &out)
{
    UNIMPLEMENTED("P0/T0.2 unpack_uint");
}

/*
 * append_uint --
 *     把 pack_uint 的结果追加到 buf 末尾。
 *     P1 的 cell 编码、P2 的页镜像都会大量用到它。
 */
void
append_uint(std::vector<u8> &buf, u64 v)
{
    UNIMPLEMENTED("P0/T0.2 append_uint");
}

/*
 * crc32 --
 *     标准 CRC-32/IEEE（反射多项式 0xEDB88320，初值/终值均取反）。
 *     "123456789" 的结果必须是 0xCBF43926。
 *
 *     真 WT: __wt_checksum_sw() —— 它默认用 CRC32C，且 x86 上走 SSE4.2 硬件指令。
 *     这里只要「同一份数据稳定得到同一个值」+「改 1 个比特一定变」，就够用了。
 *
 *     提示：用 256 项查表法；表可以函数内 static 惰性初始化。
 *     注意线程安全：P4 会有并发读，要么用 constexpr 表，要么保证幂等。
 */
u32
crc32(const u8 *data, size_t n)
{
    UNIMPLEMENTED("P0/T0.2 crc32");
}

} /* namespace twt */
