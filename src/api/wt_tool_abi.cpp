/*-
 * src/api/wt_tool_abi.cpp —— P3/T3.6：工具面内部 ABI 的 34 个函数 + 5 个数据对象
 *
 * 这个文件里的东西**没有一个会被我们的单测调用**。唯一调用方是
 * 真 `wt` / `wtperf` 二进制 —— 也就是 `make capstone` 的 C1/C2。
 * 所以：空实现不会让 `make grade` 变红，但会让 `make capstone` 起不来。
 * 这是"工具面验收"，不是"单元测试验收"。
 *
 * 起不来的方式有两种，都见过：
 *   - 函数缺了 → 加载期 / 调用期报 undefined symbol（真替换模式下）
 *   - 数据对象尺寸不对 → 链接器按 size 做 COPY，小一字节就踩坏工具的 .bss
 *
 *
 * 实现顺序建议（按依赖，不是按字母）：
 *   1. A 组的纯平台胶水（abort / errno / sleep / yield / random / thread_*）
 *      → 这组最独立，先做，能立刻让 wtperf 的线程和计时跑起来
 *   2. B 组的 buf / fmt / getopt / strtouq
 *      → 这组开始要 WT_ITEM 语义，正好复习 P2 的 cell 编码
 *   3. C 组 JSON
 *      → 独立，纯粹是 `wt dump -x` 的格式问题
 *   4. D 组元数据 / checkpoint / 日志
 *      → 最难，也最有价值：它把你 P2 写的 checkpoint 格式拉到真 WT 的字节级标准
 *
 * 每实现一个，跑一次：
 *   make shared && python3 scripts/wt_abi.py \
 *       external/wiredtiger/build/wt external/wiredtiger/build/bench/wtperf/wtperf
 * 缺口数字会一个个掉下来，掉到 0 就是 C2/C1 可以开跑的时候。
 * -*/
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>

#include "api/wt_tool_abi.h"
#include "tinywt/common/defs.h"

extern "C" {

/* =========================================================================
 * A. 平台胶水（14）
 * ========================================================================= */

/* 真 WT: src/os_common/os_abort.c:16 —— 记录 verbose 后 abort()。
 * 注意：这不是"返回错误"，是真的把进程杀掉。工具的 panic 路径靠它。 */
void
__wt_abort(WT_SESSION_IMPL *session)
{
    (void)session;
    UNIMPLEMENTED_VOID("P3/T3.6 __wt_abort");
}

/* 真 WT: src/os_posix/os_time.c:16 */
void
__wt_epoch_raw(WT_SESSION_IMPL *session, struct timespec *tsp)
{
    (void)session;
    (void)tsp;
    UNIMPLEMENTED_VOID("P3/T3.6 __wt_epoch_raw");
}

/* 真 WT: src/os_common/os_errno.c:16 */
int
__wt_errno(void)
{
    UNIMPLEMENTED_RC("P3/T3.6 __wt_errno", WT_ERROR);
}

/* 真 WT: src/os_common/filename.c:49 */
int
__wt_filename(WT_SESSION_IMPL *session, const char *name, char **path)
{
    (void)session;
    (void)name;
    (void)path;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_filename", WT_ERROR);
}

/* 真 WT: src/os_posix/os_getenv.c:16 */
int
__wt_getenv(WT_SESSION_IMPL *session, const char *variable, const char **envp)
{
    (void)session;
    (void)variable;
    (void)envp;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_getenv", WT_ERROR);
}

/* 真 WT: src/os_posix/os_time.c:56 */
int
__wt_localtime(WT_SESSION_IMPL *session, const time_t *timep, struct tm *result)
{
    (void)session;
    (void)timep;
    (void)result;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_localtime", WT_ERROR);
}

/* 真 WT: src/support/rand.c:145 */
uint32_t
__wt_random(WT_RAND_STATE volatile *rnd_state)
{
    (void)rnd_state;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_random", 0);
}

/* 真 WT: src/support/rand.c:103 */
void
__wt_random_init_seed(WT_SESSION_IMPL *session, WT_RAND_STATE volatile *rnd_state)
{
    (void)session;
    (void)rnd_state;
    UNIMPLEMENTED_VOID("P3/T3.6 __wt_random_init_seed");
}

/* 真 WT: src/os_posix/os_sleep.c:16 —— 参数是 (秒, 微秒)。 */
void
__wt_sleep(uint64_t seconds, uint64_t micro_seconds)
{
    (void)seconds;
    (void)micro_seconds;
    UNIMPLEMENTED_VOID("P3/T3.6 __wt_sleep");
}

/* 真 WT: src/os_posix/os_setvbuf.c:16 */
void
__wt_stream_set_line_buffer(FILE *fp)
{
    (void)fp;
    UNIMPLEMENTED_VOID("P3/T3.6 __wt_stream_set_line_buffer");
}

/* 真 WT: src/os_posix/os_thread.c:48 */
int
__wt_thread_create(WT_SESSION_IMPL *session, wt_thread_t *tidret, void *(*func)(void *), void *arg)
{
    (void)session;
    (void)tidret;
    (void)func;
    (void)arg;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_thread_create", WT_ERROR);
}

/* 真 WT: src/os_posix/os_thread.c:76 */
int
__wt_thread_join(WT_SESSION_IMPL *session, wt_thread_t *tid)
{
    (void)session;
    (void)tid;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_thread_join", WT_ERROR);
}

/* 真 WT: src/os_posix/os_yield.c:16 */
void
__wt_yield(void)
{
    UNIMPLEMENTED_VOID("P3/T3.6 __wt_yield");
}

/* 真 WT: src/support/err.c:650 */
void
__wt_verbose_worker(WT_SESSION_IMPL *session, WT_VERBOSE_CATEGORY category,
  WT_VERBOSE_LEVEL level, const char *fmt, ...)
{
    (void)session;
    (void)category;
    (void)level;
    (void)fmt;
    UNIMPLEMENTED_VOID("P3/T3.6 __wt_verbose_worker");
}

/* =========================================================================
 * B. 缓冲、格式化、配置（10）
 * ========================================================================= */

/* 真 WT: src/support/scratch.c:16 */
int
__wt_buf_grow_worker(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size)
{
    (void)session;
    (void)buf;
    (void)size;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_buf_grow_worker", WT_ERROR);
}

/* 真 WT: src/support/scratch.c:90 */
int
__wt_buf_fmt(WT_SESSION_IMPL *session, WT_ITEM *buf, const char *fmt, ...)
{
    (void)session;
    (void)buf;
    (void)fmt;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_buf_fmt", WT_ERROR);
}

/* 真 WT: src/support/scratch.c:106 */
int
__wt_buf_catfmt(WT_SESSION_IMPL *session, WT_ITEM *buf, const char *fmt, ...)
{
    (void)session;
    (void)buf;
    (void)fmt;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_buf_catfmt", WT_ERROR);
}

/* 真 WT: src/support/scratch.c:265 */
int
__wt_scr_alloc_func(WT_SESSION_IMPL *session, size_t size, WT_ITEM **scratchp)
{
    (void)session;
    (void)size;
    (void)scratchp;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_scr_alloc_func", WT_ERROR);
}

/* 真 WT: src/os_posix/os_snprintf.c:16 —— 返回值是本应写出的长度，截断时为 -1 或 >size。 */
int
__wt_vsnprintf_len_incr(char *buf, size_t size, size_t *retsizep, const char *fmt, va_list ap)
{
    (void)buf;
    (void)size;
    (void)retsizep;
    (void)fmt;
    (void)ap;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_vsnprintf_len_incr", WT_ERROR);
}

/* 真 WT: src/os_common/os_strtouq.c:16 */
uint64_t
__wt_strtouq(const char *nptr, char **endptr, int base)
{
    (void)nptr;
    (void)endptr;
    (void)base;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_strtouq", 0);
}

/* 真 WT: src/os_common/os_getopt.c:86 */
int
__wt_getopt(const char *progname, int nargc, char *const *nargv, const char *ostr)
{
    (void)progname;
    (void)nargc;
    (void)nargv;
    (void)ostr;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_getopt", WT_ERROR);
}

/* 真 WT: src/config/config_collapse.c:338 */
int
__wt_config_merge(
  WT_SESSION_IMPL *session, const char **cfg, const char *cfg_strip, const char **config_ret)
{
    (void)session;
    (void)cfg;
    (void)cfg_strip;
    (void)config_ret;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_config_merge", WT_ERROR);
}

/* 真 WT: src/support/err.c:473 */
void
__wt_err_func(WT_SESSION_IMPL *session, int error, const char *func, int line,
  WT_VERBOSE_CATEGORY category, const char *fmt, ...)
{
    (void)session;
    (void)error;
    (void)func;
    (void)line;
    (void)category;
    (void)fmt;
    UNIMPLEMENTED_VOID("P3/T3.6 __wt_err_func");
}

/* 真 WT: src/os_common/os_alloc.c:320 */
void
__wt_free_int(WT_SESSION_IMPL *session, const void *p_arg)
{
    (void)session;
    (void)p_arg;
    UNIMPLEMENTED_VOID("P3/T3.6 __wt_free_int");
}

/* =========================================================================
 * C. JSON（5）
 * ========================================================================= */

/* 真 WT: src/cursor/cur_json.c:833 */
ssize_t
__wt_json_strlen(const char *src, size_t srclen)
{
    (void)src;
    (void)srclen;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_json_strlen", 0);
}

/* 真 WT: src/cursor/cur_json.c:873 */
int
__wt_json_strncpy(
  WT_SESSION *wt_session, char **pdst, size_t dstlen, const char *src, size_t srclen)
{
    (void)wt_session;
    (void)pdst;
    (void)dstlen;
    (void)src;
    (void)srclen;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_json_strncpy", WT_ERROR);
}

/* 真 WT: src/cursor/cur_json.c:470 */
int
__wt_json_token(
  WT_SESSION *wt_session, const char *src, int *toktype, const char **tokstart, size_t *toklen)
{
    (void)wt_session;
    (void)src;
    (void)toktype;
    (void)tokstart;
    (void)toklen;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_json_token", WT_ERROR);
}

/* 真 WT: src/cursor/cur_json.c:595 */
const char *
__wt_json_tokname(int toktype)
{
    (void)toktype;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_json_tokname", nullptr);
}

/* 真 WT: src/cursor/cur_json.c:369 */
size_t
__wt_json_unpack_str(u_char *dest, size_t dest_len, const u_char *src, size_t src_len)
{
    (void)dest;
    (void)dest_len;
    (void)src;
    (void)src_len;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_json_unpack_str", 0);
}

/* =========================================================================
 * D. 内核元数据与日志（5）—— 这 5 个要求 P2 的格式与真 WT 字节级一致
 * ========================================================================= */

/* 真 WT: src/block/block_addr.c:272 */
int
__wt_block_ckpt_decode(WT_SESSION *wt_session, WT_BLOCK *block, const uint8_t *ckpt,
  size_t ckpt_size, WT_BLOCK_CKPT *ci)
{
    (void)wt_session;
    (void)block;
    (void)ckpt;
    (void)ckpt_size;
    (void)ci;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_block_ckpt_decode", WT_ERROR);
}

/* 真 WT: src/meta/meta_ext.c:88 */
int
__wt_metadata_get_ckptlist(WT_SESSION *session, const char *name, WT_CKPT **ckptbasep)
{
    (void)session;
    (void)name;
    (void)ckptbasep;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_metadata_get_ckptlist", WT_ERROR);
}

/* 真 WT: src/meta/meta_ext.c */
void
__wt_metadata_free_ckptlist(WT_SESSION *session, WT_CKPT *ckptbase)
{
    (void)session;
    (void)ckptbase;
    UNIMPLEMENTED_VOID("P3/T3.6 __wt_metadata_free_ckptlist");
}

/* 真 WT: src/txn/txn_log.c:755 */
int
__wt_txn_printlog(WT_SESSION *wt_session, const char *ofile, uint32_t flags, WT_LSN *start_lsn,
  WT_LSN *end_lsn)
{
    (void)wt_session;
    (void)ofile;
    (void)flags;
    (void)start_lsn;
    (void)end_lsn;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_txn_printlog", WT_ERROR);
}

/* 真 WT: src/os_common/filename.c:98 */
int
__wt_copy_and_sync(WT_SESSION *wt_session, const char *from, const char *to)
{
    (void)wt_session;
    (void)from;
    (void)to;
    UNIMPLEMENTED_RC("P3/T3.6 __wt_copy_and_sync", WT_ERROR);
}

/* =========================================================================
 * E. 数据对象（5 个）
 *
 * 这几个没有"函数体"可留空 —— 它们的契约是**尺寸**，不是行为。
 * 初值照抄真 WT：__wt_optind = 1（POSIX getopt 语义），其余为 0。
 * ========================================================================= */

/* 真 WT: os_getopt.c:71 —— 下标游标，初值 1 不是 0。 */
int __wt_optind = 1;

/* 真 WT: os_getopt.c:74 */
int __wt_optreset = 0;

/* 真 WT: os_getopt.c:75 */
int __wt_optwt = 0;

/* 真 WT: os_getopt.c:76 */
char *__wt_optarg = nullptr;

/* 真 WT: src/support/global.c:11 / struct __wt_process 在 src/include/connection.h:18。
 * COPY 重定位按 128 字节整块搬，尺寸必须一模一样。
 * 注意：这里零初始化，所以 __wt_process.checksum 这类函数指针是 nullptr ——
 * 工具若走到用它的路径会立刻段错误。这不是"漏了"，是 T3.6 还没实现那部分语义。 */
alignas(8) unsigned char __wt_process[128] = {0};

/* 尺寸自检：改小一个字节就会踩坏真工具 .bss 的相邻内存。 */
static_assert(sizeof(__wt_process) == 128, "__wt_process 必须与真库的 sizeof(WT_PROCESS) 一致");

} /* extern "C" */
