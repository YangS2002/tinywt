/*-
 * include/twt_hazard.h —— P4/T4.2 作业：hazard pointer
 *
 * 对标真 WT: src/include/hazard.h、src/support/hazard.c（`__wt_hazard_set` /
 * `__wt_hazard_clear` / `__wt_page_release` 的释放路径）。
 *
 * ---------------------------------------------------------------------------
 * 要解决的问题：
 *
 *   线程 A：page_in(ref) 拿到 Page* p，开始读
 *   线程 B：evict 选中 p，reconcile + 析构 p
 *   线程 A：继续读 p  ──▶ use-after-free
 *
 * 加锁能解决，但读路径会被写者阻塞（写者优先问题）。hazard pointer 的思路是：
 *
 *   「读之前先把自己的指针登记到一个全局可见的地方；淘汰方在析构任何页之前，
 *     扫一遍所有登记点，有人在用就不淘汰。」
 *
 * 读路径因此完全无锁（一次 store），代价是 eviction 要扫描。
 *
 * ---------------------------------------------------------------------------
 * 本作业的简化：
 *   - 每个 HazardPointer 固定 HAZARD_SLOTS 个槽（真 WT 按线程动态分配）
 *   - 用 std::atomic<Page*> 的 acquire/release 语义，够覆盖 x86/ARM
 *   - 没有 hazard pointer 的「延迟回收队列」（真 WT 用 WT_RETIRED 链表，
 *     保证被摘下的页在一个宽限期之后才真正 free）
 * -*/
#pragma once

#include <atomic>
#include <cstddef>

#include "twt_page.h"

namespace twt {

constexpr int HAZARD_SLOTS = 16;

/*
 * HazardPointer --
 *     「我当前正在用的页」。一个线程/session 一个。
 *
 *   set(slot, p)   : store(release)，必须在**读 p 之前**完成
 *   clear(slot)    : store(nullptr)
 *   holds(p)       : 本对象是否登记了 p
 */
class HazardPointer {
public:
    HazardPointer() = default;
    HazardPointer(const HazardPointer &) = delete;
    HazardPointer &operator=(const HazardPointer &) = delete;

    void set(int slot, Page *p);
    void clear(int slot);
    bool holds(Page *p) const;

private:
    std::atomic<Page *> slots_[HAZARD_SLOTS] = {};
};

/*
 * HazardRegistry --
 *     全局登记表。eviction 在 free 之前调 any_holds()。
 *
 *   attach/detach 由 session 创建/销毁时调用；实现需要保证
 *   「attach 与 any_holds 并发」不会漏掉正在 set 的指针。
 *
 * ★ 内存序要点：
 *     写方：page 内容写完 → store(release) 登记指针
 *     读方：load(acquire) 读到指针 → 才能安全访问 page 内容
 *     淘汰方：load(acquire) 扫不到 → 才能 free
 *   三个方向都用 acquire/release，不能用 relaxed。
 */
class HazardRegistry {
public:
    void attach(HazardPointer *hp);
    void detach(HazardPointer *hp);
    bool any_holds(Page *p) const;
    size_t threads() const;

private:
    /* TODO(P4/T4.2): 自己选容器。要点是 attach/detach 与 any_holds 并发安全。 */
};

/* 进程级单例（真 WT 里挂在 WT_CONNECTION 上） */
HazardRegistry &hazard_registry();

} /* namespace twt */
