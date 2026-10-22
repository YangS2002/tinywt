/*-
 * 对标 WiredTiger: src/include/cell.h + src/cell/cell_pack.c + src/cell/cell_unpack.c
 *
 * ---------------------------------------------------------------------------
 * WT 的 cell 是「磁盘页里的记录格式」。每个 cell 以 1 字节描述符开头：
 *
 *   bit 1-2 : short 类型   0b00 非 short
 *                          0b01 short key        (WT_CELL_KEY_SHORT)
 *                          0b10 short key + 前缀字节 (WT_CELL_KEY_SHORT_PFX)
 *                          0b11 short value      (WT_CELL_VALUE_SHORT)
 *             short 变体下，描述符的其余 6 位是数据长度（<= 63）
 *   bit 3   : WT_CELL_64V          —— cell 后跟一个 packed uint64
 *   bit 4   : WT_CELL_SECOND_DESC  —— 后面还有第二个描述字节（prepared / 时间窗口）
 *   bit 5-8 : cell 类型
 *
 * 前缀压缩（WT_CELL_KEY_PFX / KEY_SHORT_PFX）：
 *   row-store leaf 页的 key cell 后面紧跟 1 字节「与前一个 key 的公共前缀长度」，
 *   cell 里只存后缀。这是 WT leaf 页压缩率的主要来源之一。
 * ---------------------------------------------------------------------------
 * 简化说明：
 *   - 不做时间窗口（WT_CELL_SECOND_DESC 那套 txn/ts 窗口）→ Phase 7 再加
 *   - 不做 overflow cell / value dictionary（WT_CELL_VALUE_COPY）
 *   - addr cell 用「varint 长度 + cookie」自描述（真 WT 靠 cookie 自身解析）
 * -*/
#pragma once

#include "twt_defs.h"

namespace twt {

/* --- 与 src/include/cell.h 完全一致的常量 --- */
constexpr u8 CELL_KEY_SHORT = 0x01;
constexpr u8 CELL_KEY_SHORT_PFX = 0x02;
constexpr u8 CELL_VALUE_SHORT = 0x03;
constexpr u8 CELL_SHORT_MAX = 63;
constexpr u8 CELL_SHORT_SHIFT = 2;

constexpr u8 CELL_64V = 0x04;
constexpr u8 CELL_SECOND_DESC = 0x08;

constexpr u8 CELL_ADDR_DEL = (0);
constexpr u8 CELL_ADDR_INT = (1u << 4);
constexpr u8 CELL_ADDR_LEAF = (2u << 4);
constexpr u8 CELL_ADDR_LEAF_NO = (3u << 4);
constexpr u8 CELL_DEL = (4u << 4);
constexpr u8 CELL_KEY = (5u << 4);
constexpr u8 CELL_KEY_OVFL = (6u << 4);
constexpr u8 CELL_KEY_PFX = (7u << 4);
constexpr u8 CELL_VALUE = (8u << 4);
constexpr u8 CELL_VALUE_COPY = (9u << 4);
constexpr u8 CELL_VALUE_OVFL = (10u << 4);

constexpr u8
CELL_SHORT_TYPE(u8 v)
{
    return (u8)(v & 0x03U);
}
constexpr u8
CELL_TYPE(u8 v)
{
    return (u8)(v & 0xf0U);
}

/* -------------------------------------------------------------------------
 * 编码：把 key / value / addr 追加到 buf
 * ------------------------------------------------------------------------- */

/*
 * cell_pack_key --
 *     打包一个 key cell。prefix = 与页内前一个 key 的公共前缀长度。
 *     真 WT: __wt_cell_pack_leaf_key() (src/cell/cell_pack.c)
 */
void cell_pack_key(std::vector<u8> &buf, Slice key, size_t prefix);

/*
 * cell_pack_value --
 *     打包一个 value cell。
 *     真 WT: __wt_cell_pack_value() (src/cell/cell_pack.c)
 */
void cell_pack_value(std::vector<u8> &buf, Slice val);

/*
 * cell_pack_addr --
 *     打包一个 off-page 引用 cell（internal 页的 key → 子页地址）。
 *     真 WT: __wt_cell_pack_addr() + __wt_block_addr_pack()
 *
 *     cookie = varint(off) varint(size) varint(cksum) varint(page_id)
 *     注意：真 WT 的 WT_REF 是用「父页里的槽位」标识的，没有全局 page_id；
 *     本版为了给 cache 做 key 引入 page_id，属于有意的简化。
 */
void cell_pack_addr(std::vector<u8> &buf, const Addr &addr, u64 page_id, u8 cell_type);

/* -------------------------------------------------------------------------
 * 解码
 * ------------------------------------------------------------------------- */
struct Cell {
    u8 desc = 0;      /* 原始描述符字节 */
    u8 type = 0;      /* CELL_* 类型 */
    bool is_short = false;
    size_t prefix = 0; /* 前缀压缩长度 */
    Slice data;        /* 载荷：key 的后缀 / value / addr cookie */
    size_t total = 0;  /* 整个 cell 占用的字节数 */

    bool is_key() const
    {
        return is_short ? (CELL_SHORT_TYPE(type) == CELL_KEY_SHORT ||
                             CELL_SHORT_TYPE(type) == CELL_KEY_SHORT_PFX)
                        : (type == CELL_KEY || type == CELL_KEY_PFX);
    }
    bool is_value() const
    {
        return is_short ? (CELL_SHORT_TYPE(type) == CELL_VALUE_SHORT) : (type == CELL_VALUE);
    }
    bool is_addr() const
    {
        return !is_short &&
          (type == CELL_ADDR_INT || type == CELL_ADDR_LEAF || type == CELL_ADDR_LEAF_NO);
    }
};

/*
 * cell_unpack --
 *     解析 buf 开头的第一个 cell。
 *     真 WT: __wt_cell_unpack_safe() (src/cell/cell_unpack.c)
 */
Status cell_unpack(const u8 *p, size_t avail, Cell &out);

} /* namespace twt */
