/*-
 * tinywt: 一个用于学习 WiredTiger 实现的极简存储引擎
 *
 * 本文件对标 WiredTiger 的 src/include/error.h / misc.h / intpack_inline.h
 * ---------------------------------------------------------------------------
 * 真 WT 对应：
 *   - 状态码体系      src/include/error.h       (WT_RET / WT_ERR / WT_NOTFOUND)
 *   - Slice/WT_ITEM   src/include/misc.h        (WT_ITEM)
 *   - 变长整数        src/include/intpack_inline.h (__wt_vpack_uint / __wt_vunpack_uint)
 *   - checksum        src/checksum/...          (__wt_crc32c, x86 硬件加速)
 *
 * ★ 本文件是「已给部分」，只有函数**声明**，实现见 src/defs.cpp（P0 作业）。
 * -*/
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace twt {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

/* -------------------------------------------------------------------------
 * 状态码（对标 src/include/error.h）
 *
 * ★ 契约：数值必须与真 WT 的 dist/api_err.py 完全一致。
 *   这不是随便定的 —— 真 wtperf / wt 工具会把返回值直接和 WT_NOTFOUND 比较，
 *   差一个数就会把「键不存在」误判成「需要回滚」。
 *   compat/probe.c 里有 _Static_assert 钉死这组值。
 * ------------------------------------------------------------------------- */
enum Status : int {
    S_OK = 0,
    S_ROLLBACK = -31800,  /* 对标 WT_ROLLBACK        */
    S_DUPLICATE = -31801, /* 对标 WT_DUPLICATE_KEY   */
    S_ERROR = -31802,     /* 对标 WT_ERROR           */
    S_NOTFOUND = -31803,  /* 对标 WT_NOTFOUND        */
    S_PANIC = -31804,     /* 对标 WT_PANIC           */
    S_RESTART = -31805,   /* 对标 WT_RESTART         */
    /* 以下三个是 tinywt 自己的，不属于 WT 契约 */
    S_NOTSUP = -31806,  /* 该功能未实现（作业骨架用） */
    S_IO = -31807,      /* 系统调用失败 */
    S_CORRUPT = -31808, /* 校验和不匹配 / 磁盘镜像损坏 */
};

inline const char *
status_str(Status s)
{
    switch (s) {
    case S_OK:
        return "OK";
    case S_ROLLBACK:
        return "ROLLBACK";
    case S_DUPLICATE:
        return "DUPLICATE";
    case S_ERROR:
        return "ERROR";
    case S_NOTFOUND:
        return "NOTFOUND";
    case S_PANIC:
        return "PANIC";
    case S_RESTART:
        return "RESTART";
    case S_NOTSUP:
        return "NOTSUP";
    case S_IO:
        return "IO";
    case S_CORRUPT:
        return "CORRUPT";
    default:
        return "UNKNOWN";
    }
}

#define TW_RET(s)                          \
    do {                                   \
        ::twt::Status _s = (s);            \
        if (_s != ::twt::S_OK)             \
            return _s;                     \
    } while (0)

/* -------------------------------------------------------------------------
 * 未实现异常（作业骨架用）
 *
 * 每个待实现的函数体只有一行 UNIMPLEMENTED("P2/T2.2 descend")。
 * 测试脚手架会 catch 它，把该测试项记为 NOTIMPL 而不是让整个进程崩掉，
 * 这样 make grade 才能逐项看到「哪几项还没写」。
 * ------------------------------------------------------------------------- */
struct NotImplemented : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/*
 * 「本进程里有没有踩到未实现」
 *
 * C++ 内核里用 UNIMPLEMENTED() 直接抛异常；但 C ABI 层（P3）不能跨语言
 * 边界抛异常，只能返回错误码。为了不让「没实现」被误报成「逻辑写错了」，
 * 两边都打这个标记，测试脚手架在测试项结束时统一读一次。
 */
inline bool &
notimpl_hit()
{
    static bool v = false;
    return v;
}

#define UNIMPLEMENTED(what)                          \
    do {                                             \
        ::twt::notimpl_hit() = true;                 \
        throw ::twt::NotImplemented(                 \
          std::string("unimplemented: ") + (what));  \
    } while (0)

/* C ABI 层的写法：打标记 + 返回错误码（不能抛） */
#define UNIMPLEMENTED_RC(what, rc)                   \
    do {                                             \
        ::twt::notimpl_hit() = true;                 \
        return (rc);                                 \
    } while (0)

/* C ABI 层里返回 void 的函数 */
#define UNIMPLEMENTED_VOID(what)                     \
    do {                                             \
        ::twt::notimpl_hit() = true;                 \
        return;                                      \
    } while (0)

/* -------------------------------------------------------------------------
 * Slice：对标 WT_ITEM（WT 里到处传 WT_ITEM，{data, size, mem, flags}）
 * 这里只保留只读视图语义。★ 已给部分，不用改。
 * ------------------------------------------------------------------------- */
struct Slice {
    const u8 *p = nullptr;
    size_t n = 0;

    Slice() = default;
    Slice(const u8 *p_, size_t n_) : p(p_), n(n_) {}
    Slice(const char *s) : p((const u8 *)s), n(std::strlen(s)) {}
    Slice(const std::string &s) : p((const u8 *)s.data()), n(s.size()) {}
    Slice(const std::vector<u8> &v) : p(v.data()), n(v.size()) {}

    const u8 *data() const { return p; }
    size_t size() const { return n; }
    bool empty() const { return n == 0; }
    std::string str() const { return std::string((const char *)p, n); }

    /* 字典序比较：先比公共前缀，再比长度 */
    int cmp(const Slice &o) const
    {
        size_t m = n < o.n ? n : o.n;
        int r = m ? std::memcmp(p, o.p, m) : 0;
        if (r != 0)
            return r < 0 ? -1 : 1;
        if (n == o.n)
            return 0;
        return n < o.n ? -1 : 1;
    }
};

inline bool operator==(const Slice &a, const Slice &b) { return a.cmp(b) == 0; }
inline bool operator!=(const Slice &a, const Slice &b) { return a.cmp(b) != 0; }
inline bool operator<(const Slice &a, const Slice &b) { return a.cmp(b) < 0; }

/* -------------------------------------------------------------------------
 * Addr：对标 WT_ADDR + block manager 的 address cookie
 *
 * 真 WT 里 WT_ADDR 存的是 block manager 的「cookie」（一段不透明字节，
 * 由 __wt_block_addr_pack 打包成 (offset, size, checksum)），WT 上层不解析它。
 * 这里直接把 cookie 展开成三个字段，可读性好，语义等价。
 * ------------------------------------------------------------------------- */
struct Addr {
    u32 off = 0;   /* 文件内偏移；0 是非法值（首块被描述块占用） */
    u32 size = 0;  /* 页面镜像的**精确**字节数（不含 BlockHeader） */
    u32 alloc = 0; /* 实际占用的块大小（按 alloc_size 对齐，含 BlockHeader） */
    u32 cksum = 0; /* 镜像的校验和 */

    bool valid() const { return off != 0 && alloc != 0; }
};

using Key = std::vector<u8>;
using Val = std::vector<u8>;

inline Key
to_key(Slice s)
{
    return Key(s.data(), s.data() + s.size());
}
inline Slice
slice_of(const Key &k)
{
    return Slice(k.data(), k.size());
}
inline Key
key_of(const std::string &s)
{
    return Key(s.begin(), s.end());
}

/* -------------------------------------------------------------------------
 * P0/T0.2 变长整数（对标 __wt_vpack_uint / __wt_vunpack_uint）
 *
 * 格式：标准 LEB128（小端 7 位一组，最高位是 continuation）。
 * 语义要求：小整数占 1 字节；pack/unpack 必须严格互逆；越界返回 0。
 *
 * ★ 作业：在 src/defs.cpp 里实现下面四个函数。
 * ------------------------------------------------------------------------- */
size_t pack_uint(u8 *dst, u64 v);
size_t uint_size(u64 v);
size_t unpack_uint(const u8 *src, size_t avail, u64 &out); /* 返回消耗字节数，失败 0 */
void append_uint(std::vector<u8> &buf, u64 v);

/* -------------------------------------------------------------------------
 * P0/T0.2 校验和（对标 __wt_checksum_sw / __wt_crc32c）
 *
 * 真 WT 在 x86 上用硬件 CRC32C（Castagnoli，多项式 0x1EDC6F41 反射形式
 * 0x82F63B78）；这里为了不引依赖，用标准 CRC32（IEEE，反射形式 0xEDB88320），
 * 语义等价（只要求「同一份数据算出同一个值」+ 单比特翻转必被发现）。
 *
 * 标准检查值："123456789" → 0xCBF43926
 * ------------------------------------------------------------------------- */
u32 crc32(const u8 *data, size_t n);

} /* namespace twt */
