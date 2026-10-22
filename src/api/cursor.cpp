/*-
 * src/api/cursor.cpp —— P3 作业：WT_CURSOR 方法表
 *
 * 对标真 WT：src/btree/bt_cursor.c（游标状态机）+ src/cursor/cur_file.c
 *
 * 这一层的职责很单一，但有两个容易踩的坑：
 *
 *   1. 变参 ABI。set_key/set_value/get_key/get_value 都是 variadic，
 *      格式串由表的 key_format/value_format 决定（'S' 字符串、'i' int、
 *      'u' unsigned、'l'/'q' 长整型）。取参顺序错了，测试里表现为
 *      「值被截断 / 乱码」，但编译器不会报错。
 *
 *   2. 游标状态。真 WT 的游标有明确的状态位（key 是否设置、是否已定位）；
 *      在没定位的游标上调 next/prev 必须返回 WT_NOTFOUND，而不是去读空指针。
 *
 * 任务：
 *   T3.2  insert / update / remove / search / modify
 *   T3.3  next / prev / reset（顺序遍历，跨页要走兄弟叶）
 *   T3.4  update 的 overwrite 语义（不带 overwrite 时重复键返回 WT_DUPLICATE_KEY）
 * -*/
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "api/capi.h"

namespace {

/* 按 format 从 va_list 取一个字段，统一转成字节串（'S'/'s'/'i'/'u'/'l'/'q'） */
void
read_field(const char *fmt, size_t len, va_list &ap, std::string &out)
{
    (void)fmt;
    (void)len;
    (void)ap;
    (void)out;
    UNIMPLEMENTED("P3/T3.2 read_field");
}

/* 把内部字节串按 format 写回调用方指针 */
void
write_field(const char *fmt, size_t len, const std::string &in, va_list &ap)
{
    (void)fmt;
    (void)len;
    (void)in;
    (void)ap;
    UNIMPLEMENTED("P3/T3.2 write_field");
}

int
cur_close(WT_CURSOR *c)
{
    (void)c;
    UNIMPLEMENTED_RC("P3/T3.2 cursor.close", WT_ERROR);
}

void
cur_set_key(WT_CURSOR *c, ...)
{
    (void)c;
    UNIMPLEMENTED_VOID("P3/T3.2 cursor.set_key");
}

void
cur_set_value(WT_CURSOR *c, ...)
{
    (void)c;
    UNIMPLEMENTED_VOID("P3/T3.2 cursor.set_value");
}

int
cur_get_key(WT_CURSOR *c, ...)
{
    (void)c;
    UNIMPLEMENTED_RC("P3/T3.2 cursor.get_key", WT_ERROR);
}

int
cur_get_value(WT_CURSOR *c, ...)
{
    (void)c;
    UNIMPLEMENTED_RC("P3/T3.2 cursor.get_value", WT_ERROR);
}

/*
 * cur_insert --
 *     key 必须已设置。键已存在时：
 *       overwrite == true  → 覆盖
 *       否则               → WT_DUPLICATE_KEY（★ 注意不是 WT_ERROR）
 *     真 WT 里 overwrite 是游标打开时的配置，不是 insert 的参数。
 */
int
cur_insert(WT_CURSOR *c)
{
    (void)c;
    UNIMPLEMENTED_RC("P3/T3.2 cursor.insert", WT_ERROR);
}

/*
 * cur_update --
 *     key 必须已设置；键不存在 → WT_NOTFOUND。
 *     语义是「覆盖」：不需要 overwrite 配置。
 */
int
cur_update(WT_CURSOR *c)
{
    (void)c;
    UNIMPLEMENTED_RC("P3/T3.2 cursor.update", WT_ERROR);
}

/* cur_remove：键不存在 → WT_NOTFOUND */
int
cur_remove(WT_CURSOR *c)
{
    (void)c;
    UNIMPLEMENTED_RC("P3/T3.2 cursor.remove", WT_ERROR);
}

/*
 * cur_search --
 *     key 必须已设置；找到则把 value 填进游标（get_value 才能取到）。
 *     键不存在 → WT_NOTFOUND，且游标不再是「已定位」状态。
 */
int
cur_search(WT_CURSOR *c)
{
    (void)c;
    UNIMPLEMENTED_RC("P3/T3.2 cursor.search", WT_ERROR);
}

/*
 * cur_search_near --
 *     找「最接近 key」的位置：优先第一个 >= key，没有则最后一个。
 *     *exactp 置 1 表示精确命中，0 表示近似。返回值仍是 0（不是 WT_NOTFOUND），
 *     除非表是空的。
 */
int
cur_search_near(WT_CURSOR *c, int *exactp)
{
    (void)c;
    (void)exactp;
    UNIMPLEMENTED_RC("P3/T3.3 cursor.search_near", WT_ERROR);
}

/*
 * cur_next / cur_prev --
 *     依赖 Btree::cursor_next / cursor_prev（P2/T2.5）维护的遍历状态。
 *     ★ 没有定位过（既没 search 也没 next）时返回 WT_NOTFOUND：
 *       真 WT 不会替你从第一行开始扫。
 *     ★ 越过最后一行返回 WT_NOTFOUND，并把游标复位（下一次 prev 从末尾开始）。
 */
int
cur_next(WT_CURSOR *c)
{
    (void)c;
    UNIMPLEMENTED_RC("P3/T3.3 cursor.next", WT_ERROR);
}

int
cur_prev(WT_CURSOR *c)
{
    (void)c;
    UNIMPLEMENTED_RC("P3/T3.3 cursor.prev", WT_ERROR);
}

/* cur_reset：清掉 key/value/定位状态，并释放遍历持有的页 pin */
int
cur_reset(WT_CURSOR *c)
{
    (void)c;
    UNIMPLEMENTED_RC("P3/T3.3 cursor.reset", WT_ERROR);
}

/*
 * cur_modify --
 *     读出现值 → 依次应用 WT_MODIFY 补丁（offset/size/data）→ 写回。
 *     offset 超出当前长度时用空格补齐（真 WT 报 EINVAL；这里简化）。
 *     键不存在 → WT_NOTFOUND。
 */
int
cur_modify(WT_CURSOR *c, WT_MODIFY *entries, int nentries)
{
    (void)c;
    (void)entries;
    (void)nentries;
    UNIMPLEMENTED_RC("P3/T3.4 cursor.modify", WT_ERROR);
}

} /* namespace */

/*
 * twt_cursor_create --
 *     分配 twt_cursor 并绑定方法表。
 *     ★ 绑定这段是「已给」的 ABI 样板：iface.uri / key_format / value_format
 *       必须指向**生命周期长于游标**的字符串（这里指向 twt_table 里的 string）。
 */
WT_CURSOR *
twt_cursor_create(twt_session *ses, twt_table *tab, const std::string &uri, bool overwrite)
{
    (void)uri;
    auto *cur = new twt_cursor();
    cur->ses = ses;
    cur->tab = tab;
    cur->overwrite = overwrite;
    cur->iface.session = &ses->iface;
    cur->iface.uri = tab->uri.c_str();
    cur->iface.key_format = tab->key_format.c_str();
    cur->iface.value_format = tab->value_format.c_str();

    cur->iface.close = cur_close;
    cur->iface.set_key = cur_set_key;
    cur->iface.set_value = cur_set_value;
    cur->iface.get_key = cur_get_key;
    cur->iface.get_value = cur_get_value;
    cur->iface.insert = cur_insert;
    cur->iface.update = cur_update;
    cur->iface.remove = cur_remove;
    cur->iface.search = cur_search;
    cur->iface.search_near = cur_search_near;
    cur->iface.next = cur_next;
    cur->iface.prev = cur_prev;
    cur->iface.reset = cur_reset;
    cur->iface.modify = cur_modify;
    return &cur->iface;
}

/* 销毁游标：先释放遍历状态（Btree::cursor_release），再 delete */
void
twt_cursor_destroy(twt_cursor *c)
{
    (void)c;
    UNIMPLEMENTED("P3/T3.2 twt_cursor_destroy");
}

/* -------------------------------------------------------------------------
 * wiredtiger_calc_modify —— 把 oldv → newv 的差异算成 WT_MODIFY 补丁
 *
 * 简化：只产生「一个连续区间」的补丁（真 WT 会生成多段最优补丁）。
 * 返回值约定（wtperf 的调用方靠这个退化）：
 *   0            成功，*nentriesp 是补丁数（可能为 0 = 完全相同）
 *   WT_NOTFOUND  差异无法用 maxdiff 内的补丁表达 → 调用方改走整值 update
 *
 * 算法提示：先算公共前缀 p，再算公共后缀 s；
 *   rm = olen - p - s（被替换掉的旧字节数）
 *   add = nlen - p - s（新增的字节数），add > maxdiff 就返回 WT_NOTFOUND。
 * ------------------------------------------------------------------------- */
extern "C" int
wiredtiger_calc_modify(WT_SESSION *session, const WT_ITEM *oldv, const WT_ITEM *newv,
  size_t maxdiff, WT_MODIFY *entries, int *nentriesp)
{
    (void)session;
    (void)oldv;
    (void)newv;
    (void)maxdiff;
    (void)entries;
    (void)nentriesp;
    UNIMPLEMENTED_RC("P3/T3.4 wiredtiger_calc_modify", WT_ERROR);
}
