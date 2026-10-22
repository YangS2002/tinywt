/*-
 * src/compression/compress.cpp —— P2/T2.7 作业：页压缩（内建 RLE）
 *
 * 对标真 WT: ext/compressors/snappy/snappy_compress.c（接口形状一样，
 * 只是那里连的是第三方库）。真 WT 的压缩器实现两个回调：
 *   compress : 把页镜像压成更短的字节串
 *   decompress: 反转
 * 本作业把这两个回调做成自由函数，T2.7 的测试直接它们。
 * -*/
#include "tinywt/compression/compress.h"

namespace twt {

/*
 * rle_compress --
 *     [count varint][byte] 重复。count 用 varint 编码（通常 1 字节）。
 *     连续段最长 256（避免 count 无限增长掩盖坏数据）。
 *
 *     ★ 对不可压缩的数据（全是随机字节）输出会比输入还大 —— 这是 RLE 的正常
 *       表现。真 WT 在这种情况下会放弃压缩（返回「不划算」），本作业不判这个，
 *       只保证往返正确。
 */
Status
rle_compress(const u8 *in, size_t n, std::vector<u8> &out)
{
    /* TODO(P2/T2.7): 见头文件里的格式说明。 */
    (void)in;
    (void)n;
    (void)out;
    UNIMPLEMENTED("P2/T2.7 rle_compress");
}

/*
 * rle_decompress --
 *     逐段展开；任何一段的 count 为 0、或输入在中途结束 → S_CORRUPT。
 *     解出来的总长度不设上限会导致 OOM，所以要么限制（比如 1GB），
 *     要么在调用方已知原始长度时校验（真 WT 的块头里有 disk_size）。
 */
Status
rle_decompress(const u8 *in, size_t n, std::vector<u8> &out)
{
    /* TODO(P2/T2.7) */
    (void)in;
    (void)n;
    (void)out;
    UNIMPLEMENTED("P2/T2.7 rle_decompress");
}

/*
 * compress_page / decompress_page --
 *     按 Compression 分派。NONE 直接拷贝（不做任何事）。
 *     真 WT 对应 __wt_compressor_compress() / __wt_compressor_decompress()。
 */
Status
compress_page(Compression c, const std::vector<u8> &in, std::vector<u8> &out)
{
    switch (c) {
    case Compression::NONE:
        out = in;
        return S_OK;
    case Compression::RLE:
        return rle_compress(in.data(), in.size(), out);
    }
    return S_ERROR;
}

Status
decompress_page(Compression c, const std::vector<u8> &in, std::vector<u8> &out)
{
    switch (c) {
    case Compression::NONE:
        out = in;
        return S_OK;
    case Compression::RLE:
        return rle_decompress(in.data(), in.size(), out);
    }
    return S_ERROR;
}

} /* namespace twt */
