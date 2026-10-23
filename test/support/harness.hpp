/*-
 * harness.hpp —— tinywt 测试脚手架（BusTub 式：每个 Project 一个独立可执行）
 *
 * 设计目标：
 *   1. 每个 test/pN_*_test.cpp 自带 main，只 include 本文件，互不依赖。
 *   2. 一个 CASE(...) = 一个测试项（Test）；RUN(fn) 包住它，做异常防火墙。
 *      → 骨架里任何函数抛 NotImplemented，只有这一项变 NOTIMPL，
 *        后面的测试项照常跑完，grade 才能逐项报分。
 *   3. 结束时打印每个人类可读的汇总 + 机器可读的 RESULT/SCORE 行，
 *      交给 scripts/grade.py 聚合。
 *
 * 用法：
 *   #define TWT_PROJECT "P2"
 *   #include "support/harness.hpp"
 *
 *   static void test_xxx() {
 *       CASE("P2/T2.3 分裂");
 *       CHECK(cond);
 *       CHECK_ST(expr);
 *   }
 *   int main() { RUN(test_xxx); return harness_report(); }
 *
 * 状态：
 *   PASS    测试函数正常返回，且至少有一个断言、零失败
 *   FAIL    有断言失败
 *   NOTIMPL 命中了 UNIMPLEMENTED（该 Task 还没写）
 *   ERROR   ！抛出了别的异常 / 零断言
 * -*/
#pragma once

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

#include <unistd.h>

#include "tinywt/common/defs.h"

#ifndef TWT_PROJECT
#define TWT_PROJECT "P?"
#endif

namespace fs = std::filesystem;

/* -------------------------------------------------------------------------
 * 崩溃 / 超时看门狗（wire）
 *
 * 为什么需要：harness 的异常防火墙只挡得住 C++ 异常。段错误、assert、
 * std::terminate、死循环都挡不住 —— 那会让当前项只有 BEGIN 没有 RESULT，
 * grade.py 只能记 MISS。MISS 是"没测"，但学生看到的是"这一项没结果"，
 * 分不清是崩溃、超时还是漏写。
 *
 * 这里做两件事：
 *   1. 进程级信号处理器：段错误 / abort / 总线错误 / 非法指令 / 算术异常
 *      → 立刻补一行 RESULT ... CRASH(...)，错误信息里带当前 CASE 名。
 *   2. 每项一个 alarm 超时（默认 180s，TWT_CASE_TIMEOUT 可改）
 *      → 死循环变成 TIMEOUT，而不是把整个二进制挂到 grade.py 的 1800s 上限。
 *
 * ★ sanitizer 构建不接管 SIGSEGV/SIGABRT：ASan/TSan 自己的报告比我们这行
 *   RESULT 有用得多（栈、影子内存、竞争双方）。只保留 SIGALRM。
 * ------------------------------------------------------------------------- */
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define TWT_SANITIZER_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define TWT_SANITIZER_BUILD 1
#endif
#endif
#ifndef TWT_SANITIZER_BUILD
#define TWT_SANITIZER_BUILD 0
#endif

namespace twt_hx {

/* 当前项的 RESULT 前缀："RESULT P2\t<case>\t"。信号处理器只做 write()。 */
inline char g_prefix[512] = {0};
inline size_t g_prefix_len = 0;

inline void
set_prefix(const char *proj, const char *name)
{
    int n = std::snprintf(g_prefix, sizeof(g_prefix), "RESULT %s\t%s\t", proj, name);
    g_prefix_len = (n > 0 && (size_t)n < sizeof(g_prefix)) ? (size_t)n : 0;
}

inline void
wire(const char *s)
{
    if (s != nullptr && *s != '\0')
        (void)!::write(STDOUT_FILENO, s, std::strlen(s));
}

/* async-signal-safe：只用 write / strlen / _exit */
inline void
fatal(int sig)
{
    const char *status = "ERROR";
    switch (sig) {
    case SIGALRM:
        status = "TIMEOUT";
        break;
    case SIGSEGV:
        status = "CRASH(SEGV)";
        break;
    case SIGABRT:
        status = "CRASH(ABRT)";
        break;
    case SIGBUS:
        status = "CRASH(BUS)";
        break;
    case SIGFPE:
        status = "CRASH(FPE)";
        break;
    case SIGILL:
        status = "CRASH(ILL)";
        break;
    default:
        break;
    }
    if (g_prefix_len)
        (void)!::write(STDOUT_FILENO, g_prefix, g_prefix_len);
    wire(status);
    wire("\t0\t1\t0\n");
    _exit(128 + sig);
}

inline void
install()
{
    struct sigaction sa {};
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fatal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;

    (void)sigaction(SIGALRM, &sa, nullptr);

#if !TWT_SANITIZER_BUILD
    (void)sigaction(SIGSEGV, &sa, nullptr);
    (void)sigaction(SIGABRT, &sa, nullptr);
    (void)sigaction(SIGBUS, &sa, nullptr);
    (void)sigaction(SIGFPE, &sa, nullptr);
    (void)sigaction(SIGILL, &sa, nullptr);
#endif
}

struct Installer {
    Installer() { install(); }
};
inline const Installer g_installer {};

/* 每项超时秒数：TWT_CASE_TIMEOUT=<秒>，0 = 关掉 */
inline int
case_timeout()
{
    static const int secs = []() {
        const char *e = std::getenv("TWT_CASE_TIMEOUT");
        int v = (e != nullptr && *e != '\0') ? std::atoi(e) : 180;
        return v > 0 ? v : 0;
    }();
    return secs;
}

} /* namespace twt_hx */


/* 当前测试项的计数器（测试代码直接用这三个名字） */
inline int g_pass = 0;
inline int g_fail = 0;
inline const char *g_case = "(no case)";

/*
 * CASE 名字的稳定存储。
 * 有些测试用动态名字（CASE(tag.c_str())），如果只存指针，
 * 传进来的临时 std::string 在函数返回后就没了 —— 汇总时会打出乱码。
 * 所以这里额外持有一份拷贝，g_case 指向它。
 */
inline std::string &
case_name_store()
{
    static std::string s;
    return s;
}

struct CaseResult {
    std::string proj;
    std::string name;
    const char *status = "ERROR";
    int pass = 0;
    int fail = 0;
    long ms = 0;
    std::string note;
};

inline std::vector<CaseResult> &
results()
{
    static std::vector<CaseResult> v;
    return v;
}

#define CASE(name)                                                                                \
    do {                                                                                          \
        case_name_store() = (name);                                                               \
        g_case = case_name_store().c_str();                                                       \
        twt_hx::set_prefix(TWT_PROJECT, g_case); /* 崩了也能报出是哪个 CASE */                    \
        printf("\n=== %s ===\n", g_case);                                                         \
    } while (0)

#define CHECK(cond)                                                                               \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            printf("  FAIL [%s] %s:%d  %s\n", g_case, __FILE__, __LINE__, #cond);                 \
            ++g_fail;                                                                             \
        } else {                                                                                  \
            ++g_pass;                                                                             \
        }                                                                                         \
    } while (0)

#define CHECK_ST(s)                                                                               \
    do {                                                                                          \
        twt::Status _s = (s);                                                                     \
        if (_s != twt::S_OK) {                                                                    \
            printf("  FAIL [%s] %s:%d  status=%s (%d)\n", g_case, __FILE__, __LINE__,             \
              twt::status_str(_s), (int)_s);                                                      \
            ++g_fail;                                                                             \
        } else {                                                                                  \
            ++g_pass;                                                                             \
        }                                                                                         \
    } while (0)

/* 断言「返回了某个特定错误码」——错误码契约类测试用它 */
#define CHECK_RC(s, want)                                                                         \
    do {                                                                                          \
        twt::Status _s = (s);                                                                     \
        if (_s != (want)) {                                                                       \
            printf("  FAIL [%s] %s:%d  got=%s(%d) want=%s(%d)\n", g_case, __FILE__, __LINE__,     \
              twt::status_str(_s), (int)_s, twt::status_str(want), (int)(want));                  \
            ++g_fail;                                                                             \
        } else {                                                                                  \
            ++g_pass;                                                                             \
        }                                                                                         \
    } while (0)

/*
 * RUN(test_fn) —— 一个测试项
 *
 * 异常防火墙放在这里而不是 CASE 里，原因是 C++ 的 try 必须配一个块；
 * 让「一个测试函数 = 一个测试项」，题目代码就不用改结构。
 */
template <typename Fn>
inline void
run_case(const char *fallback_name, Fn fn)
{
    g_pass = 0;
    g_fail = 0;
    case_name_store() = fallback_name;
    g_case = case_name_store().c_str();
    twt_hx::set_prefix(TWT_PROJECT, g_case);
    twt::notimpl_hit() = false;

    /* 每项一个闹钟：死循环 / 死等锁 → TIMEOUT，而不是整份结果作废 */
    const int tmo = twt_hx::case_timeout();
    if (tmo > 0)
        (void)alarm((unsigned)tmo);

    auto t0 = std::chrono::steady_clock::now();
    const char *status = "PASS";
    std::string note;

    /*
     * 开跑前先报一声。RESULT 行是**最后**统一打印的，所以进程要是中途崩了
     * （段错误 / assert / std::terminate），这一项和它后面的项都会从输出里消失，
     * grade.py 拿到的分母就悄悄变小了 —— "3/49" 会变成 "3/30"，百分比还更高。
     * 有了 BEGIN 行，grade.py 至少能指出"哪一项开跑了却没有结果"。
     */
    printf("BEGIN %s\t%s\n", TWT_PROJECT, case_name_store().c_str());
    fflush(stdout);

    try {
        fn();
    } catch (const twt::NotImplemented &e) {
        status = "NOTIMPL";
        note = e.what();
    } catch (const std::exception &e) {
        status = "ERROR";
        note = std::string("exception: ") + e.what();
    } catch (...) {
        status = "ERROR";
        note = "unknown exception";
    }

    (void)alarm(0); /* 拆掉闹钟：后面的打印/汇总不能再被它打断 */

    /* C ABI 层不能抛异常，靠这个标记report */
    if (status[0] == 'P' && twt::notimpl_hit())
        status = "NOTIMPL";

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();

    if (status[0] == 'P') {
        if (g_fail > 0)
            status = "FAIL";
        else if (g_pass == 0) {
            status = "ERROR";
            note = "no assertion executed";
        }
    }

    CaseResult r;
    r.proj = TWT_PROJECT;
    r.name = case_name_store(); /* CASE() 里的名字比函数名可读，且已拷贝过 */
    r.status = status;
    r.pass = g_pass;
    r.fail = g_fail;
    r.ms = (long)ms;
    r.note = note;
    results().push_back(r);

    /*
     * 每项**跑完立刻**打一行 RESULT，不再等 harness_report 收尾时统一打。
     * 原因：统一打的话，中途崩一次就一行都没有，前面已经跑完的项也一起消失，
     * grade.py 看到的是"这个二进制什么都没测"，分不清是崩溃还是没写。
     * 逐项打之后，崩在哪一项一目了然（那一项只有 BEGIN 没有 RESULT）。
     */
    printf("RESULT %s\t%s\t%s\t%d\t%d\t%ld\n", r.proj.c_str(), r.name.c_str(), r.status, r.pass,
      r.fail, r.ms);
    fflush(stdout);
}

#define RUN(fn) run_case(#fn, fn)

/*
 * RUN_NAMED(name, fn) —— 给一项起一个**稳定**的名字。
 *
 * 为什么需要：RUN 用函数名当 fallback，而 CASE() 里的名字才是给人看的。
 * 如果一个函数内部调 CASE() 多次（比如多组种子），那么崩溃时打出来的是
 * 崩溃那一刻的名字，跑通时打的是最后一个 —— 同一个评分项的名字会变。
 * 想让一项始终叫同一个名字，用 RUN_NAMED。
 */
#define RUN_NAMED(name, fn) run_case(name, fn)

/* 每个测试二进制自己的收尾：打印汇总 + 机器可读行，返回 0/1 */
inline int
harness_report()
{
    const char *proj = TWT_PROJECT;
    int ok = 0, total = 0;
    int asserts = 0;

    printf("\n--------------------------------------------------------------------------\n");
    printf("  %-4s %-46s %-8s %6s %6s\n", "proj", "test", "status", "pass", "fail");
    printf("--------------------------------------------------------------------------\n");
    for (const CaseResult &r : results()) {
        ++total;
        asserts += r.pass;
        if (std::strcmp(r.status, "PASS") == 0)
            ++ok;
        printf("  %-4s %-46s %-8s %6d %6d", r.proj.c_str(), r.name.c_str(), r.status, r.pass,
          r.fail);
        if (!r.note.empty())
            printf("  %s", r.note.c_str());
        printf("\n");
    }
    printf("--------------------------------------------------------------------------\n");
    printf("  %s: %d/%d tests, %d assertions\n", proj, ok, total, asserts);

    /* 机器可读的收尾行（RESULT 已经在 run_case 里逐项打过了，这里只补 SCORE） */
    printf("SCORE %s\t%d\t%d\n", proj, ok, total);

    return ok == total ? 0 : 1;
}

/* ---------------- 测试常用小工具 ---------------- */

/* 临时库路径：所有测试共用 /tmp/tinywt_test，按名字隔离 */
inline std::string
dbpath(const std::string &name)
{
    fs::path d = fs::temp_directory_path() / "tinywt_test";
    fs::create_directories(d);
    return (d / name).string();
}

inline void
rmpath(const std::string &p)
{
    std::error_code ec;
    fs::remove_all(p, ec);
}

/* 固定宽度键，保证字典序 == 数值序（K 定长是 B+树测试的常见前提） */
inline std::string
kstr(int i)
{
    char b[32];
    std::snprintf(b, sizeof(b), "key-%06d", i);
    return std::string(b);
}

inline std::string
vstr(int i)
{
    char b[64];
    std::snprintf(b, sizeof(b), "value-%06d-%s", i, std::string(20, 'p').c_str());
    return std::string(b);
}
