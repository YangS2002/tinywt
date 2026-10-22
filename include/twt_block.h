/*-
 * 对标 WiredTiger: src/include/block.h + src/block/block_open.c
 *                  src/block/block_write.c + src/block/block_read.c
 *                  src/block/block_ext.c (extent 管理) + src/block/block_addr.c
 *
 * ---------------------------------------------------------------------------
 * WT 块管理器的三件核心事：
 *
 *   1. 文件 = 一系列 block。block 0 写文件描述块 —— 因此 "offset == 0" 是
 *      非法偏移（WT_BLOCK_INVALID_OFFSET），可以当 null 用。
 *
 *   2. 每个 block 前面有 WT_BLOCK_HEADER：
 *          uint32 disk_size;   // 落盘页大小（便于 salvage）
 *          uint32 checksum;    // 页校验和
 *      校验和存两份：一份在 block header（给 salvage 用），一份写在父页引用它的
 *      address cookie 里（这样即使整页被另一张合法页覆盖也能被发现）。
 *
 *   3. 每个 checkpoint 维护三张 extent 表（跳表实现）：
 *          alloc   : 本 checkpoint 分配的 extent
 *          avail   : 可分配（空闲）的 extent
 *          discard : 本 checkpoint 释放的 extent
 *      → 关键点：checkpoint N 里释放的块，要到 checkpoint N+1 才能复用，
 *        否则上一个 checkpoint 仍然引用着它。这就是 discard 表存在的理由。
 *
 * 简化说明：
 *   - extent 表用有序 vector 代替 WT 的双跳表（by-offset + by-size）
 *   - 分配用 first-fit 代替 WT 的 by-size best-fit
 *   - 只保留单个 checkpoint（真 WT 保留多个，所以 discard 必须跨 checkpoint 存活）
 * -*/
#pragma once

#include <fstream>
#include <string>
#include <vector>

#include "twt_defs.h"

namespace twt {

/* 与 WT_BLOCK_HEADER 对齐：disk_size + checksum */
struct BlockHeader {
    u32 disk_size;
    u32 checksum;
};
constexpr size_t BLOCK_HEADER_SIZE = 8;

/* 空闲区间（对标 WT_EXT：{off, size, depth, next[]} 双跳表节点） */
struct Extent {
    u32 off;
    u32 size;
};

/* 超级块：对标 WT 的 metadata (WiredTiger.wt) + checkpoint 里的 extent 表 */
struct SuperBlock {
    u64 magic = 0x545754545353ULL; /* "TWTSS" */
    u32 version = 1;
    u32 alloc_size = 4096;
    u64 file_size = 0;
    u64 root_page_id = 0;
    u64 next_page_id = 1;
    Addr root_addr;

    /*
     * --- P4/T4.4 + T4.5 ---
     * flags         : SBF_DIRTY_SHUTDOWN = 上次没走干净关闭 → open 时要 RTS
     * stable_ts     : 稳定时间戳（watermark），RTS 的回滚锚点
     * ckpt_lsn      : P3/T3.5 的 checkpoint LSN，日志只需保留 >= 它（预留）
     */
    u32 flags = 0;
    u64 stable_ts = 0;
    u64 ckpt_lsn = 0;

    std::vector<Extent> avail; /* checkpoint 时持久化 */
};

/* 超级块 flags 位 */
constexpr u32 SBF_DIRTY_SHUTDOWN = 0x01;

class BlockManager {
public:
    BlockManager() = default;
    ~BlockManager();

    /* 不允许拷贝 */
    BlockManager(const BlockManager &) = delete;
    BlockManager &operator=(const BlockManager &) = delete;

    Status open(const std::string &path, u32 alloc_size = 4096);
    Status close();

    /*
     * write --
     *     分配一个块 → 写 BlockHeader + 镜像 → 返回地址 cookie。
     *     真 WT: __wt_block_write_off()
     */
    Status write(const u8 *image, size_t size, Addr &out);

    /*
     * read --
     *     按地址读回镜像，并校验 (a) header 里的 disk_size
     *     (b) 镜像 checksum == addr.cksum == header.checksum。
     *     真 WT: __wt_block_read_off()
     */
    Status read(const Addr &a, std::vector<u8> &image);

    /*
     * free_block --
     *     块并不立刻回到 avail，而是进入 discard 表；由 checkpoint 统一回收。
     *     真 WT: __wt_block_free() → __wt_block_extend / discard 跳表
     */
    Status free_block(const Addr &a);

    Status write_super(const SuperBlock &sb);
    Status read_super(SuperBlock &sb);

    /* checkpoint：把 discard 表合并回 avail 并持久化 */
    Status checkpoint_extents(std::vector<Extent> &avail);

    /* 从超级块恢复空闲 extent 表 */
    void set_avail(const std::vector<Extent> &v)
    {
        avail_ = v;
        discard_.clear();
    }

    u32 alloc_size() const { return alloc_size_; }
    u64 file_size() const { return file_size_; }
    const std::vector<Extent> &avail() const { return avail_; }
    const std::vector<Extent> &discard() const { return discard_; }
    size_t avail_bytes() const;
    std::string dump_extents() const;

private:
    u32 alloc_extent(u32 size); /* first-fit，找不到就扩展文件 */
    void insert_avail(Extent e);
    void coalesce(std::vector<Extent> &v);
    void write_at(u64 off, const u8 *data, size_t n);
    void read_at(u64 off, u8 *data, size_t n);

    std::fstream f_;
    std::string path_;
    bool open_ = false;
    u32 alloc_size_ = 4096;
    u64 file_size_ = 0;
    std::vector<Extent> avail_;
    std::vector<Extent> discard_;
};

} /* namespace twt */
