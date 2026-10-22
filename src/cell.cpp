/*-
 * src/cell.cpp —— P1 作业：cell 编解码
 *
 * 对标真 WT: src/cell/cell_pack.c + src/cell/cell_unpack.c
 *
 * cell 是「磁盘页里的一条记录的头部」。它不存整条记录，只存
 * 「类型 + 长度 + 压缩信息」，真正的载荷紧跟在后面。所以 cell 编码直接决定了
 * 一页能装多少行，进而决定树高和 IO 次数。
 *
 * 位布局见 include/twt_cell.h 顶部的注释（★ 先读那段再动手）。
 * -*/

#include "twt_cell.h"

namespace twt {

/*
 * cell_pack_key --
 *     打包一个 key cell（可选前缀压缩）。
 *
 * 布局：
 *   short（key 全长 <= CELL_SHORT_MAX=63）：
 *       [desc: (0b01|0b10) | len<<2]  [prefix? 1B]  [suffix bytes]
 *   否则：
 *       [CELL_KEY | CELL_KEY_PFX]     [prefix? 1B]  [varint suffix_len]  [suffix bytes]
 *
 * 关键点（容易写错的地方）：
 *   - short 变体里，desc 的高 6 位存的是「整个 key 的长度」，不是后缀长度；
 *     解出来时 suffix = len - prefix。
 *   - desc 的 bit0-1 是 short 子类型，bit2 是 CELL_64V，位序别搞反
 *     （CELL_SHORT_SHIFT == 2）。
 *   - prefix > key.size() 时按 key.size() 截断（防御性）。
 *   - 只写后缀：buf 里绝不出现 key 的前 prefix 个字节。
 *
 * 真 WT: __wt_cell_pack_leaf_key()
 */
void
cell_pack_key(std::vector<u8> &buf, Slice key, size_t prefix)
{
    UNIMPLEMENTED("P1/T1.3 cell_pack_key");
}

/*
 * cell_pack_value --
 *     short 变体下 6 位存长度；否则 [CELL_VALUE] + varint 长度。
 *
 * 真 WT: __wt_cell_pack_value()
 */
void
cell_pack_value(std::vector<u8> &buf, Slice val)
{
    UNIMPLEMENTED("P1/T1.3 cell_pack_value");
}

/*
 * cell_pack_addr --
 *     打包 off-page 引用 cell（内部页的「key → 子页地址」）。
 *
 * 布局：[desc=cell_type] [varint cookie_len] [cookie]
 *   cookie = varint(off) varint(size) varint(alloc) varint(cksum) varint(page_id)
 *
 * 为什么要长度前缀：真 WT 的 cookie 是自描述的（__wt_block_addr_unpack 知道
 * 几个字段、各几字节）；这里为了能独立定位 cell 边界，显式存长度。
 *
 * 真 WT: __wt_cell_pack_addr() + __wt_block_addr_pack()
 */
void
cell_pack_addr(std::vector<u8> &buf, const Addr &addr, u64 page_id, u8 cell_type)
{
    UNIMPLEMENTED("P1/T1.3 cell_pack_addr");
}

/*
 * cell_unpack --
 *     解析 buf 开头的第一个 cell，填 out（desc/type/is_short/prefix/data/total）。
 *
 * 要求：
 *   - 任何越界读都要返回 S_CORRUPT，绝不能读越界（这是不可信输入，
 *     损坏的页就是靠这层检查挡住的 —— 对标 __wt_cell_unpack_safe）。
 *   - out.total 是「这个 cell 自己」占的字节数（不含后面的载荷之外的东西），
 *     上层靠它顺序推进到下一个 cell。
 *   - out.data 指向**载荷**（key 的后缀 / value / addr cookie），不拷贝。
 *
 * 真 WT: __wt_cell_unpack_safe()
 */
Status
cell_unpack(const u8 *p, size_t avail, Cell &out)
{
    UNIMPLEMENTED("P1/T1.3 cell_unpack");
}

} /* namespace twt */
