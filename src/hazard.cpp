/*-
 * src/hazard.cpp —— P4/T4.2 作业：hazard pointer 实现
 *
 * 对标真 WT: src/support/hazard.c（约 300 行）。
 *
 * 真 WT 的实现比这里的接口多两件事，做完这题可以回头读：
 *   1. `__wt_hazard_set()` 不只是登记，还要处理「登记时页正好被抢走」——
 *      用 ref->state 的 CAS 重试，所以签名带一个 `bool *busyp` 返回值。
 *   2. `__wt_hazard_clear()` 之后还有一步 `__wt_page_release()`：
 *      真正 unpin 并把页交给淘汰线程。
 * -*/
#include "twt_hazard.h"

namespace twt {

/*
 * set --
 *     把 p 登记到 slot 并返回旧值（旧值非空说明调用方忘了 clear，
 *     属于用法错误，可以断言）。
 *
 *     ★ 顺序：page 内容必须在 store(release) **之前**写好；
 *       读者用 load(acquire) 读到指针后才有权访问内容。
 */
void
HazardPointer::set(int slot, Page *p)
{
    /* TODO(P4/T4.2) */
    (void)slot;
    (void)p;
    UNIMPLEMENTED("P4/T4.2 HazardPointer::set");
}

void
HazardPointer::clear(int slot)
{
    /* TODO(P4/T4.2)：store(nullptr, release) */
    (void)slot;
    UNIMPLEMENTED("P4/T4.2 HazardPointer::clear");
}

bool
HazardPointer::holds(Page *p) const
{
    /* TODO(P4/T4.2)：注意释放语义下比较指针的合法性 */
    (void)p;
    UNIMPLEMENTED("P4/T4.2 HazardPointer::holds");
}

void
HazardRegistry::attach(HazardPointer *hp)
{
    /* TODO(P4/T4.2) */
    (void)hp;
    UNIMPLEMENTED("P4/T4.2 HazardRegistry::attach");
}

void
HazardRegistry::detach(HazardPointer *hp)
{
    /* TODO(P4/T4.2) */
    (void)hp;
    UNIMPLEMENTED("P4/T4.2 HazardRegistry::detach");
}

/*
 * any_holds --
 *     扫描所有登记点。返回 true = 有人在用，不能 free。
 *
 * ★ 这里是 eviction 的正确性闸门：漏扫一个就会 use-after-free，
 *   而 ASan 在 `make asan` 下能抓到。多扫（假阳性）只会让淘汰变慢，不可怕。
 */
bool
HazardRegistry::any_holds(Page *p) const
{
    /* TODO(P4/T4.2) */
    (void)p;
    UNIMPLEMENTED("P4/T4.2 HazardRegistry::any_holds");
}

size_t
HazardRegistry::threads() const
{
    /* TODO(P4/T4.2) */
    UNIMPLEMENTED("P4/T4.2 HazardRegistry::threads");
}

HazardRegistry &
hazard_registry()
{
    /* TODO(P4/T4.2)：函数内 static，避免静态初始化顺序问题 */
    UNIMPLEMENTED("P4/T4.2 hazard_registry");
}

} /* namespace twt */
