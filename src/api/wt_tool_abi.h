/*-
 * src/api/wt_tool_abi.h —— 工具面内部 ABI：真 wt / wtperf 会直接来要的 39 个符号
 *                          （34 个函数 + 5 个数据对象）
 *
 * ★ 为什么需要这个文件（不是"多此一举的兼容层"）：
 *   真 `wt` 和 `wtperf` 不是只调公开 API。它们源码里 include 了 WT 内部头
 *   （wt_internal.h / posix.h），所以**编译出来的二进制**带着一批 __wt_* 的依赖：
 *
 *       $ nm -D --undefined-only $WT/build/wt                   → 20 个函数
 *       $ nm -D --undefined-only $WT/build/bench/wtperf/wtperf  → 18 个函数
 *       并集（重叠 4 个）                                        → 34 个函数
 *       $ readelf -rW <工具> | grep R_X86_64_COPY               → 5 个数据对象
 *
 *   数据对象（__wt_optind 之类）在 nm 里显示为"已定义"，因为链接器做了 COPY
 *   重定位：在可执行文件的 .bss 里复制一份。只有 readelf -r 才看得出来。
 *
 * ★ 怎么让真工具用上我们的库 —— 两条路，结论不同：
 *   (a) LD_PRELOAD：真库**仍然被加载**（可执行文件里写着 NEEDED
 *       libwiredtiger.so.11.3.0）。我们没导出的符号会被真库静默兜底，于是
 *       "跑通了"可能是一半我们的实现 + 一半真 WT —— 假通过（实测 __wt_getopt
 *       就绑到了真库）。
 *   (b) 真替换：我们的 .so 与真库**同名同 SONAME**，用 LD_LIBRARY_PATH 顶上，
 *       真库根本不进场；这时少一个符号就是硬 undefined symbol。
 *   本项目走 (b)（CMake 的 SOVERSION 对齐 11.3.0，脚本里见 wt_setup_dropin）。
 *
 * ★ 签名从哪来（重要，别凭记忆改）：
 *   全部由 `gcc -E` 在真 WT 的构建配置下导出，即 `HAVE_DIAGNOSTIC=0` 时实际生效的那一版。
 *   反例：`__wt_err_func` 很容易被写成 3 参数（session, error, fmt, ...），
 *   但实际声明是 7 参数（多 func / line / category）。抄错就是运行时栈错位。
 *
 * ★ 纪律：
 *   这些函数只在"被真工具调用"时才有意义。我们的单测一条都不会调到它们 ——
 *   验收靠 `make capstone` 的 C2/C1，不靠断言。所以别为了让测试变绿去糊一个返回值。
 *
 * 分组见下：平台胶水 / 缓冲与格式化 / JSON / 内核元数据与日志 / 数据对象。
 * 每一条后面都标了真 WT 里的定义位置，实现时照着看。
 * -*/
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <pthread.h>
#include <sys/types.h>

#include <wiredtiger.h>

/* -------------------------------------------------------------------------
 * 内部类型：WT 的内部头里才有。我们只需要它们出现在指针位置，
 * 所以按**原 tag 名**前置声明（tag 必须一致，否则类型系统对不上）。
 * ------------------------------------------------------------------------- */
struct __wt_session_impl;
struct __wt_block;
struct __wt_block_ckpt;
struct __wt_ckpt;
union __wt_lsn;
union __wt_rand_state;

typedef struct __wt_session_impl WT_SESSION_IMPL;
typedef struct __wt_block WT_BLOCK;
typedef struct __wt_block_ckpt WT_BLOCK_CKPT;
typedef struct __wt_ckpt WT_CKPT;
typedef union __wt_lsn WT_LSN;
typedef union __wt_rand_state WT_RAND_STATE;

/* wt_thread_t 在真 WT 里是匿名 struct（src/include/posix.h:28），没法前置声明，
 * 只能照抄布局。只有指针进函数，stub 不解引用；但你真要实现线程封装时需要这几个字段。 */
typedef struct {
    bool created;
    uint16_t name_index;
    pthread_t id;
} wt_thread_t;

extern "C" {

/* =========================================================================
 * A. 平台胶水（14）—— wtperf 的 test/utility 层要的
 *    全是薄封装，实现起来不难，但一条写错（比如 sleep 单位）就会让吞吐数字失真。
 * ========================================================================= */

/* 真 WT: src/os_common/os_abort.c:16 */
void __wt_abort(WT_SESSION_IMPL *session);

/* 单调/墙钟时间。真 WT: src/os_posix/os_time.c:16 */
void __wt_epoch_raw(WT_SESSION_IMPL *session, struct timespec *tsp);

/* 真 WT: src/os_common/os_errno.c:16 */
int __wt_errno(void);

/* 相对/绝对路径拼装。真 WT: src/os_common/filename.c:49 */
int __wt_filename(WT_SESSION_IMPL *session, const char *name, char **path);

/* 真 WT: src/os_posix/os_getenv.c:16 */
int __wt_getenv(WT_SESSION_IMPL *session, const char *variable, const char **envp);

/* 真 WT: src/os_posix/os_time.c:56 */
int __wt_localtime(WT_SESSION_IMPL *session, const time_t *timep, struct tm *result);

/* xorshift 风格随机数。真 WT: src/support/rand.c:145 */
uint32_t __wt_random(WT_RAND_STATE volatile *rnd_state);

/* 真 WT: src/support/rand.c:103 */
void __wt_random_init_seed(WT_SESSION_IMPL *session, WT_RAND_STATE volatile *rnd_state);

/* 注意单位：秒 + 微秒，不是纳秒。真 WT: src/os_posix/os_sleep.c:16 */
void __wt_sleep(uint64_t seconds, uint64_t micro_seconds);

/* 真 WT: src/os_posix/os_setvbuf.c:16 */
void __wt_stream_set_line_buffer(FILE *fp);

/* 真 WT: src/os_posix/os_thread.c:48 */
int __wt_thread_create(
  WT_SESSION_IMPL *session, wt_thread_t *tidret, void *(*func)(void *), void *arg);

/* 真 WT: src/os_posix/os_thread.c:76 */
int __wt_thread_join(WT_SESSION_IMPL *session, wt_thread_t *tid);

/* 让出 CPU。真 WT: src/os_posix/os_yield.c:16 */
void __wt_yield(void);

/* 真 WT: src/support/err.c:650 */
void __wt_verbose_worker(WT_SESSION_IMPL *session, WT_VERBOSE_CATEGORY category,
  WT_VERBOSE_LEVEL level, const char *fmt, ...);

/* =========================================================================
 * B. 缓冲、格式化、配置（10）—— 工具打印/解析全走这里
 *    这组是"看起来像 printf，其实带 WT 语义"：buf 是 WT_ITEM，未实现时会立刻暴露。
 * ========================================================================= */

/* 扩容到至少 size，保留原内容。真 WT: src/support/scratch.c:16 */
int __wt_buf_grow_worker(WT_SESSION_IMPL *session, WT_ITEM *buf, size_t size);

/* 覆写式格式化（printf 语义）。真 WT: src/support/scratch.c:90 */
int __wt_buf_fmt(WT_SESSION_IMPL *session, WT_ITEM *buf, const char *fmt, ...);

/* 追加式格式化（wt 的 dump / stat 输出靠它拼行）。真 WT: src/support/scratch.c:106 */
int __wt_buf_catfmt(WT_SESSION_IMPL *session, WT_ITEM *buf, const char *fmt, ...);

/* 取一块 scratch buffer。真 WT: src/support/scratch.c:265 */
int __wt_scr_alloc_func(WT_SESSION_IMPL *session, size_t size, WT_ITEM **scratchp);

/* 返回本应写出的长度（截断时 > size）。真 WT: src/os_posix/os_snprintf.c:16 */
int __wt_vsnprintf_len_incr(char *buf, size_t size, size_t *retsizep, const char *fmt, va_list ap);

/* 无符号 64 位解析，出错语义比 strtoull 严格。真 WT: src/os_common/os_strtouq.c:16 */
uint64_t __wt_strtouq(const char *nptr, char **endptr, int base);

/* WT 自己的 getopt（改了错误输出，不污染 argv）。真 WT: src/os_common/os_getopt.c:86 */
int __wt_getopt(const char *progname, int nargc, char *const *nargv, const char *ostr);

/* 合并配置串，返回新分配的字符串。真 WT: src/config/config_collapse.c:338 */
int __wt_config_merge(
  WT_SESSION_IMPL *session, const char **cfg, const char *cfg_strip, const char **config_ret);

/* 错误上报的总入口（所有 WT_ERR_MSG / WT_RET_MSG 最终汇到这里）。
 * 真 WT: src/support/err.c:473 */
void __wt_err_func(WT_SESSION_IMPL *session, int error, const char *func, int line,
  WT_VERBOSE_CATEGORY category, const char *fmt, ...);

/* 释放 __wt_malloc 分配的块。真 WT: src/os_common/os_alloc.c:320 */
void __wt_free_int(WT_SESSION_IMPL *session, const void *p_arg);

/* =========================================================================
 * C. JSON（5）—— `wt dump -x` / `wt load -j` 的编解码
 *    真 WT 自己手写了一个流式 JSON reader，没引第三方库。
 * ========================================================================= */

/* 真 WT: src/cursor/cur_json.c:833 */
ssize_t __wt_json_strlen(const char *src, size_t srclen);

/* 真 WT: src/cursor/cur_json.c:873 */
int __wt_json_strncpy(
  WT_SESSION *wt_session, char **pdst, size_t dstlen, const char *src, size_t srclen);

/* 真 WT: src/cursor/cur_json.c:470 */
int __wt_json_token(
  WT_SESSION *wt_session, const char *src, int *toktype, const char **tokstart, size_t *toklen);

/* 真 WT: src/cursor/cur_json.c:595 */
const char *__wt_json_tokname(int toktype);

/* 真 WT: src/cursor/cur_json.c:369 */
size_t __wt_json_unpack_str(
  u_char *dest, size_t dest_len, const u_char *src, size_t src_len);

/* =========================================================================
 * D. 内核元数据与日志（5）—— 这一组真的碰存储，不是"纯工具函数"
 *    C2 能不能过，主要看这 5 个。它们直接读 WT 的元数据表和 checkpoint 地址 cookie，
 *    也就是 P2（页与写路径）里你写的东西必须和真 WT 的格式字节级对齐。
 * ========================================================================= */

/* 把 checkpoint 地址 cookie 解成 WT_BLOCK_CKPT（含 root page 的 addr/size/checksum）。
 * 真 WT: src/block/block_addr.c:272 */
int __wt_block_ckpt_decode(WT_SESSION *wt_session, WT_BLOCK *block, const uint8_t *ckpt,
  size_t ckpt_size, WT_BLOCK_CKPT *ci);

/* 从元数据表读 `checkpoint=(...)` 配置，解析成 WT_CKPT 链表。
 * 真 WT: src/meta/meta_ext.c:88 */
int __wt_metadata_get_ckptlist(WT_SESSION *session, const char *name, WT_CKPT **ckptbasep);

/* 真 WT: src/meta/meta_ext.c */
void __wt_metadata_free_ckptlist(WT_SESSION *session, WT_CKPT *ckptbase);

/* `wt printlog` 的实现：把 WAL 记录解码成人能读的形式。
 * 真 WT: src/txn/txn_log.c:755 */
int __wt_txn_printlog(WT_SESSION *wt_session, const char *ofile, uint32_t flags,
  WT_LSN *start_lsn, WT_LSN *end_lsn);

/* 文件拷贝 + fsync（wt 工具的 backup 路径用）。真 WT: src/os_common/filename.c:98 */
int __wt_copy_and_sync(WT_SESSION *wt_session, const char *from, const char *to);

/* =========================================================================
 * E. getopt 状态与进程级全局（5 个**数据对象**，不是函数）
 *
 * ★ 为什么这里会有变量：`nm -D --undefined-only` 看不见它们。
 *   `__wt_optind` 这类是"可执行文件直接引用库里的数据"，链接器会用
 *   R_X86_64_COPY 重定位：在可执行文件的 .bss 里**复制一份**。所以它们在
 *   `nm -D` 里显示为"已定义"，而只在 `readelf -r` 里露出真实需求：
 *
 *       $ readelf -rW $WT/build/wt | grep R_X86_64_COPY
 *       ... __wt_optreset / __wt_optwt / __wt_optind / __wt_optarg
 *       $ readelf -rW $WT/build/bench/wtperf/wtperf | grep R_X86_64_COPY
 *       ... __wt_optreset / __wt_optind / __wt_optarg / __wt_process
 *
 * ★ COPY 重定位按 **size** 搬字节：我们定义的对象比真库小，ld.so 就会踩坏
 *   可执行文件 .bss 里它后面那块内存。尺寸一个字节都不能改。
 *   真值：__wt_optarg 8 / __wt_optind 4 / __wt_optreset 4 / __wt_optwt 4 /
 *         __wt_process 128（`readelf -sW libwiredtiger.so.11.3.0` 实测）。
 * ========================================================================= */

/* getopt 的游标。真 WT: src/os_common/os_getopt.c:71（__wt_optind 初值 1） */
extern int __wt_optind;

/* 置 1 表示下一次 __wt_getopt 从头开始。真 WT: os_getopt.c:74 */
extern int __wt_optreset;

/* 1 = 用 WT_* 返回码而不是 '?'。真 WT: os_getopt.c:75 */
extern int __wt_optwt;

/* 当前选项的参数。真 WT: os_getopt.c:76 */
extern char *__wt_optarg;

/* 进程级全局。真 WT: WT_PROCESS __wt_process（src/support/global.c:11），
 * 结构体定义在 src/include/connection.h:18，含 checksum 函数指针、TS 频率比等。
 * 这里只保证**尺寸与对齐**（COPY 按 128 字节搬）；字段布局不在这里复制 ——
 * 工具侧用到的字段语义属于 T3.6 的实现内容，零初始化的函数指针一旦被调用就是空指针。 */
extern unsigned char __wt_process[128];

} /* extern "C" */
