/*-
 * src/storage/block.cpp —— P1 作业：块管理器
 *
 * 对标真 WT:
 *   src/block/block_open.c  : __wt_block_open  (文件描述块、allocsize)
 *   src/block/block_write.c : __wt_block_write_off
 *   src/block/block_read.c  : __wt_block_read_off
 *   src/block/block_ext.c   : extent 跳表的 alloc/free/merge
 *   src/block/block_addr.c  : __wt_block_addr_pack / unpack
 *
 * 文件布局（本版约定，实现必须严格照此）：
 *
 *   offset 0                 block 0：描述块（保留，不用；所以 off==0 是非法地址）
 *   offset alloc_size        block 1：超级块（定长 alloc_size，尾部补 0）
 *   offset 2*alloc_size      第一个可用块
 *   ...
 *
 * 每个数据块：
 *   [BlockHeader{disk_size, checksum, flags, unused[3]}] [镜像 size 字节] [补 0 到 alloc_size 对齐]
 *
 *   disk_size 是**对齐后的总长度**（含 header），size 是镜像精确长度。
 *   两者必须分开存 —— 见 write() 里的说明。
 * -*/

#include "tinywt/storage/block.h"

#include <algorithm>
#include <cstdio>

namespace twt {

BlockManager::~BlockManager()
{
    /* TODO(P1): 关闭仍打开的文件。析构里不能抛异常，close() 的失败只能忽略。 */
}

/*
 * open --
 *     以读写方式打开（不存在则创建），探测文件长度；
 *     若是空文件，预分配 2 个 block（描述块 + 超级块所在块），
 *     这样 alloc_extent 永远不会把 offset 0 发出去。
 *
 * 真 WT: __wt_block_open()
 */
Status
BlockManager::open(const std::string &path, u32 alloc_size)
{
    UNIMPLEMENTED("P1/T1.1 BlockManager::open");
}

/*
 * close --
 *     flush + close，幂等（重复调用返回 S_OK）。
 */
Status
BlockManager::close()
{
    UNIMPLEMENTED("P1/T1.1 BlockManager::close");
}

/* 底层 pread/pwrite：必须 flush，且短读/短写要能被发现 */
void
BlockManager::write_at(u64 off, const u8 *data, size_t n)
{
    UNIMPLEMENTED("P1/T1.1 BlockManager::write_at");
}

void
BlockManager::read_at(u64 off, u8 *data, size_t n)
{
    UNIMPLEMENTED("P1/T1.1 BlockManager::read_at");
}

/*
 * write --
 *     分配块 → 写 header + 镜像 → 返回地址 cookie。
 *
 * 分配长度：total = round_up(BLOCK_HEADER_SIZE + size, alloc_size)
 * 填 out：
 *   off   = 分配到的偏移
 *   size  = 镜像精确字节数
 *   alloc = total
 *   cksum = crc32(image, size)
 * header 里的 disk_size 存 total，checksum 存 cksum。
 *
 * ★ 为什么 size / alloc 必须分开：
 *   size  决定读回来 image 的边界（否则尾部对齐的 0 会被当成 cell 解析）
 *   alloc 决定 free 时回收多少字节（否则对齐字节泄漏成空洞）
 *
 * 真 WT: __wt_block_write_off()
 */
Status
BlockManager::write(const u8 *image, size_t size, Addr &out)
{
    UNIMPLEMENTED("P1/T1.2 BlockManager::write");
}

/*
 * read --
 *     按地址读回镜像，三重校验：
 *       (a) header.disk_size == a.alloc
 *       (b) header.checksum == a.cksum          ← 整块被别的合法页覆盖也能发现
 *       (c) crc32(image, a.size) == a.cksum     ← 真实内容校验
 *     任一不符返回 S_CORRUPT；读不到返回 S_IO。
 *
 * 真 WT: __wt_block_read_off()
 */
Status
BlockManager::read(const Addr &a, std::vector<u8> &image)
{
    UNIMPLEMENTED("P1/T1.2 BlockManager::read");
}

/*
 * free_block --
 *     块不立刻回到 avail_，而是进 discard_，等 checkpoint 统一回收。
 *
 * ★ 为什么：checkpoint N 还可能被读（正在跑的查询/上一个 checkpoint），
 *   它引用的块在 N+1 之前不能复用，否则就是「覆盖了别人还在读的数据」。
 *   真 WT 的 discard 跳表活得更久，因为它保留多个 checkpoint。
 */
Status
BlockManager::free_block(const Addr &a)
{
    UNIMPLEMENTED("P1/T1.2 BlockManager::free_block");
}

/*
 * checkpoint_extents --
 *     discard_ 并入 avail_ → 合并相邻区间 → 回填给调用方持久化。
 *     合并（coalesce）不只是省内存：不合并的话 first-fit 会因为碎片
 *     找不到连续空间而不断扩展文件。
 */
Status
BlockManager::checkpoint_extents(std::vector<Extent> &avail)
{
    UNIMPLEMENTED("P1/T1.2 BlockManager::checkpoint_extents");
}

/*
 * alloc_extent --
 *     first-fit 扫描 avail_；切走前 size 字节，剩余部分留在原 extent。
 *     找不到就扩展文件（file_size_ 增加），返回旧 file_size_。
 *     返回 0 表示失败（0 是非法偏移）。
 *
 * 真 WT 用 by-size 跳表做 best-fit，避免大块被小块切碎。
 */
u32
BlockManager::alloc_extent(u32 size)
{
    UNIMPLEMENTED("P1/T1.2 BlockManager::alloc_extent");
}

/* 插入空闲区间并合并 */
void
BlockManager::insert_avail(Extent e)
{
    UNIMPLEMENTED("P1/T1.2 BlockManager::insert_avail");
}

/* 按 offset 排序 + 合并首尾相接的区间（addr 恰好衔接也要合并） */
void
BlockManager::coalesce(std::vector<Extent> &v)
{
    UNIMPLEMENTED("P1/T1.2 BlockManager::coalesce");
}

size_t
BlockManager::avail_bytes() const
{
    UNIMPLEMENTED("P1/T1.2 BlockManager::avail_bytes");
}

std::string
BlockManager::dump_extents() const
{
    UNIMPLEMENTED("P1/T1.2 BlockManager::dump_extents");
}

/* -------------------------------------------------------------------------
 * 超级块：固定放在 offset alloc_size，定长 alloc_size 字节
 *
 * 编码（全部 varint，顺序不能变）：
 *   magic, version, alloc_size, file_size, root_page_id, next_page_id,
 *   root_addr.off, root_addr.size, root_addr.alloc, root_addr.cksum,
 *   avail_count, {extent.off, extent.size} * avail_count
 *
 * read_super 遇 magic 不匹配必须返回 S_NOTFOUND（= 空库 / 未初始化），
 * 不能返回 S_CORRUPT —— 上层靠这个区分「新库」和「坏了」。
 * ------------------------------------------------------------------------- */

Status
BlockManager::write_super(const SuperBlock &sb)
{
    UNIMPLEMENTED("P1/T1.2 BlockManager::write_super");
}

Status
BlockManager::read_super(SuperBlock &sb)
{
    UNIMPLEMENTED("P1/T1.2 BlockManager::read_super");
}

} /* namespace twt */
