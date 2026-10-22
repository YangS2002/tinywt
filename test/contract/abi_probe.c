/*-
 * test/contract/abi_probe.c —— 链接器评分器的"合同探针"
 *
 * 这个文件不实现任何东西，只做两件事：
 *   1. 编译期：证明真 wiredtiger.h 能独立编译（C11），且 wtperf 依赖的
 *      函数/方法/类型全部存在——API 面一旦缺失，这里先炸。
 *   2. 链接期：P3 起把本文件链进 libwiredtiger.a，缺哪个实现就缺哪个符号。
 *
 * 依赖面来源：bench 目录下 wtperf 全部源文件 grep（见 docs/PROJECTS.md §7）。
 *
 *   make probe       只编译（C 合同 + 错误码数值）
 *   make probe-link  再往前一步：把本文件链进 libtinywt.a，
 *                    缺哪个接口就在链接器报哪个符号 —— 这就是「链接器当评分器」。
 * -*/
#include "wiredtiger.h"

/* ---- 公开函数面（真工具实际调用：wt 3 个 + wtperf 4 个，去重后 5 个） ----
 *
 * 清单由 `make wt-abi` 对着真二进制核对：真 wt 需要
 *   wiredtiger_open / wiredtiger_strerror / wiredtiger_version
 * 真 wtperf 需要
 *   wiredtiger_open / wiredtiger_strerror / wiredtiger_config_parser_open
 *   / wiredtiger_calc_modify
 * 少一个（尤其是 wiredtiger_version：wt 启动就拿它做版本自检，
 * 对不上直接 EXIT_FAILURE），链接期的 --no-undefined 就少查一个接口。
 */
static const void *const twt_probe_functions[] = {
    (const void *)wiredtiger_open,
    (const void *)wiredtiger_strerror,
    (const void *)wiredtiger_version,
    (const void *)wiredtiger_config_parser_open,
    (const void *)wiredtiger_calc_modify,
};

/* ---- 方法面：取成员地址即要求结构体字段存在 ---- */
static void
twt_probe_members(WT_CONNECTION *conn, WT_SESSION *ses, WT_CURSOR *cur, WT_CONFIG_PARSER *cp)
{
    /* WT_CONNECTION */
    (void)conn->close;
    (void)conn->open_session;

    /* WT_SESSION（wtperf 用到的全部） */
    (void)ses->close;
    (void)ses->create;
    (void)ses->drop;
    (void)ses->open_cursor;
    (void)ses->begin_transaction;
    (void)ses->commit_transaction;
    (void)ses->rollback_transaction;
    (void)ses->checkpoint;
    (void)ses->truncate;

    /* WT_CURSOR */
    (void)cur->set_key;
    (void)cur->set_value;
    (void)cur->get_key;
    (void)cur->get_value;
    (void)cur->next;
    (void)cur->prev;
    (void)cur->search;
    (void)cur->insert;
    (void)cur->update;
    (void)cur->remove;
    (void)cur->reset;
    (void)cur->modify;

    /* WT_CONFIG_PARSER（wtperf 解析 .wtperf 配置就靠它） */
    (void)cp->next;
    (void)cp->close;
}

/* 编译期常量合同：错误码数值必须与真 WT 一致（wtperf 判 ret == WT_NOTFOUND）
 * 真值来源：dist/api_err.py（注释写明 "These numbers cannot change without
 * breaking backward compatibility"）。tinywt 的 twt::Status 必须逐一对齐，
 * C ABI 边界（src/api/）只做 twt::Status ↔ int 的**恒等**映射。 */
_Static_assert(WT_ROLLBACK == -31800, "WT_ROLLBACK 数值变了");
_Static_assert(WT_DUPLICATE_KEY == -31801, "WT_DUPLICATE_KEY 数值变了");
_Static_assert(WT_ERROR == -31802, "WT_ERROR 数值变了");
_Static_assert(WT_NOTFOUND == -31803, "WT_NOTFOUND 数值变了，wtperf 判断会失效");

/* 锚点：保证上面两个探针符号不被优化掉，同时消除 unused 告警 */
__attribute__((used)) void
twt_probe_anchor(void)
{
    (void)twt_probe_functions[0];
    twt_probe_members(NULL, NULL, NULL, NULL);
}
