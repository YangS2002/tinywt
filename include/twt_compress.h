/*-
 * include/twt_compress.h —— P2/T2.7 作业：页压缩
 *
 * 对标真 WT: src/compress/ 与 ext/compressors/（真 WT 的压缩器是 .so 扩展，
 * 通过 WT_CONNECTION::add_compressor 注册；本作业先做一个内建 RLE）。
 *
 * ★ 压缩发生在哪一层：**块写入之前**。Btree::reconcile_page 生成的页镜像
 *   先压缩、再交给 BlockManager::write。所以压缩/解压完全不感知 cell 格式，
 *   这也是为什么 WT 能让 snappy / zlib 直接插进来。
 *
 * 不变量：decompress(compress(x)) == x，对任意字节串成立（包括空串和
 * 已经被压缩过的数据）。压缩率不是正确性的一部分。
 * -*/
#pragma once

#include <string>
#include <vector>

#include "twt_defs.h"

namespace twt {

enum class Compression : u8 {
    NONE = 0,
    RLE = 1, /* 内建：字节级 run-length，用于自测与解锁 compression=rle */
};

/*
 * rle_compress --
 *     格式：[run_count varint][byte][run_count varint][byte] ...
 *     run_count 是「连续相同字节的个数」（1..256）。
 *
 *     返回 S_OK / S_ERROR。out 会被清空后重填。
 *
 *     提示：解压时必须校验「剩余输入不足」「run 越界」，
 *     损坏的页要返回 S_CORRUPT 而不是读越界。
 */
Status rle_compress(const u8 *in, size_t n, std::vector<u8> &out);

/* 返回 S_OK / S_CORRUPT（输入不是合法 RLE 流）/ S_ERROR */
Status rle_decompress(const u8 *in, size_t n, std::vector<u8> &out);

/* 统一入口：Btree 在 reconcile / instantiate 两侧调用这两个 */
Status compress_page(Compression c, const std::vector<u8> &in, std::vector<u8> &out);
Status decompress_page(Compression c, const std::vector<u8> &in, std::vector<u8> &out);

} /* namespace twt */
