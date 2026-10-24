/*-
 * harness.hpp —— tinywt 测试脚手架（GoogleTest 适配层）
 *
 * 分工：
 *   GoogleTest  负责「测例怎么跑、断言怎么写」：TEST / EXPECT_* / ASSERT_* / --gtest_filter
 *   本文件      负责「分数怎么算」：BEGIN / RESULT / SCORE 三行协议 + 崩溃看门狗
 *
 * 为什么还要这层适配：
 *   1. grade.py 靠 RESULT 行逐项算分，而 RESULT 必须**每项跑完立刻**打，
 *      不能等到收尾统一打（中途崩一次就一行都没有，前面跑完的项一起消失）。
 *      GoogleTest 的 TestEventListener 正好在每项开始/结束时各给一次回调。
 *   2. 骨架里每个函数体都是 UNIMPLEMENTED(...)，会抛 NotImplemented。
 *      这一项要记成 NOTIMPL（还没写），不能记成 FAIL（写错了），
 *      否则 49 项全红，学生看不出自己到底写没写对。
 *   3. 段错误 / 死循环要能定位到**哪一项**：进程级信号处理器 + 每项一个闹钟。
 *
 * 用法：
 *   #include "support/harness.hpp"
 *
 *   TWT_TEST(P2_Btree, Split, "P2/T2.3 分裂 + 对拍 std::map")
 *   {
 *       ASSERT_EQ(S_OK, t.open(path, cfg));
 *       EXPECT_GT(t.stats().splits, 0u);
 *   }
 *
 *   ★ 第三个参数是给人看、也给 grade.py 看的名字，必须带 Px/Ty 前缀
 *     （check_docs.py 按这个前缀把测例归到 Task 上）。
 *     GoogleTest 自己的 TEST(suite, name) 只允许 ASCII 标识符，
 *     所以名字用注册表外挂，见 register_name()。
 *
 *   main() 在 test/support/gtest_main.cpp 里，每个测试二进制链一次。
 * -*/
#pragma once

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <unistd.h>

#include "tinywt/common/defs.h"

namespace fs = std::filesystem;

/* -------------------------------------------------------------------------
 * 崩溃 / 超时看门狗（wire）
 *
 * 为什么需要：GoogleTest 的异常防火墙只挡得住 C++ 异常（而且要靠
 * --gtest_catch_exceptions）。段错误、assert、std::terminate、死循环都挡不住 ——
 * 那会让当前项只有 BEGIN 没有 RESULT，grade.py 只能记 MISS。
 * MISS 是"没测"，但学生看到的是"这一项没结果"，分不清是崩溃、超时还是漏写。
 *
 * 这里做两件事：
 *   1. 进程级信号处理器：段错误 / abort / 总线错误 / 非法指令 / 算术异常
 *      → 立刻补一行 RESULT ... CRASH(...)，错误信息里带当前项的名字。
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

/*
 * 静态对象先装一遍，main() 里再装一遍 —— 因为 GoogleTest 的
 * --gtest_install_failure_signal_handler 会覆盖 SIGSEGV，必须后装。
 */
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

namespace twt_test {

/* 本二进制的 Project 标签（"P0".."P4"）：定义在 test/support/gtest_main.cpp */
const char *project();

/*
 * 评分项的可读名字。
 *
 * GoogleTest 的 TEST(suite, name) 里两个名字都只能是 ASCII 标识符，
 * 而评分项叫 "P2/T2.3 分裂 + 对拍 std::map（4 组种子 × cache 压力）"。
 * 所以名字走外挂注册表：TWT_TEST 的第三个参数在 static init 阶段登记，
 * 打 BEGIN/RESULT 时按 "Suite.Name" 反查。
 *
 * 顺带的好处：RESULT 行里是同一个名字，check_docs.py 的 Px/Ty 前缀归账不会漂。
 */
bool register_name(const std::string &gtest_name, const char *display);

const std::string &display_name(const std::string &gtest_name);

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

/*
 * 事件监听器：GoogleTest 每项开始/结束各回调一次，正好对上 BEGIN/RESULT。
 *
 * 状态判定（与旧脚手架语义一致）：
 *   NOTIMPL  本项命中了 UNIMPLEMENTED（notimpl_hit()）
 *   ERROR    测例体抛了别的异常（旧脚手架也是这个口径）
 *   FAIL     有断言失败
 *   PASS     其余
 *
 * ★ NOTIMPL 优先于 FAIL：骨架状态下 NotImplemented 也被 GoogleTest 记成
 *   一次断言失败，先看标记才能把"还没写"和"写错了"分开。
 *
 * ★ 为什么 pass 列只有 0/1：
 *   GoogleTest **只回调失败的断言**（成功的断言不进 TestPartResult），
 *   所以拿不到"这一项跑过多少条 CHECK"。旧脚手架的 pass 计数是它自己数的，
 *   换框架之后就数不出来了。
 *   与其编一个假数字，这里如实定义：
 *       pass = 1 表示这一项通过，0 表示没通过
 *       fail = 失败的断言条数（GoogleTest 能给准确值）
 *   grade.py 只看 status，这两列是给人看的。
 */
class Listener : public ::testing::EmptyTestEventListener {
public:
    void
    OnTestStart(const ::testing::TestInfo &info) override
    {
        twt::notimpl_hit() = false;
        name_ = display_name(std::string(info.test_suite_name()) + "." + info.name());
        twt_hx::set_prefix(project(), name_.c_str()); /* 崩了也能报出是哪个 CASE */

        /*
         * 开跑前先报一声。RESULT 行是每项**跑完**才打的，进程要是中途崩了
         * （段错误 / assert / std::terminate），这一项和它后面的项都会从输出里消失，
         * grade.py 拿到的分母就悄悄变小了 —— "3/49" 会变成 "3/30"，百分比还更高。
         * 有了 BEGIN 行，grade.py 至少能指出「哪一项开跑了却没有结果」。
         */
        std::printf("BEGIN %s\t%s\n", project(), name_.c_str());
        std::fflush(stdout);

        /* 每项一个闹钟：死循环 / 死等锁 → TIMEOUT，而不是整份结果作废 */
        const int tmo = twt_hx::case_timeout();
        if (tmo > 0)
            (void)alarm((unsigned)tmo);

        t0_ = std::chrono::steady_clock::now();
        failed_ = 0;
        exception_ = false;
    }

    void
    OnTestPartResult(const ::testing::TestPartResult &r) override
    {
        if (r.skipped() || !r.failed())
            return;
        ++failed_;
        /*
         * 测例体抛异常时 GoogleTest 记一条 "unknown file" 的失败，
         * 消息以 "C++ exception with description" 开头。旧脚手架把这种情况
         * 记成 ERROR（和 FAIL"断言写错了"分开），这里沿用。
         */
        if (r.message() != nullptr &&
          std::strncmp(r.message(), "C++ exception with description", 31) == 0)
            exception_ = true;
    }

    void
    OnTestEnd(const ::testing::TestInfo &) override
    {
        (void)alarm(0); /* 拆掉闹钟：后面的打印/汇总不能再被它打断 */

        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0_)
                          .count();

        const char *status = "PASS";
        std::string note;
        int pass = 1;
        int fail = failed_;
        if (twt::notimpl_hit()) {
            status = "NOTIMPL";
            note = "unimplemented";
            pass = 0;
            fail = 0; /* 没实现不算"断言写错了"，别在表里报成 1 条失败 */
        } else if (exception_) {
            status = "ERROR";
            note = "exception";
            pass = 0;
        } else if (failed_ > 0) {
            status = "FAIL";
            pass = 0;
        }

        CaseResult r;
        r.proj = project();
        r.name = name_;
        r.status = status;
        r.pass = pass;
        r.fail = fail;
        r.ms = (long)ms;
        r.note = note;
        results().push_back(r);

        /*
         * 每项**跑完立刻**打一行 RESULT，不再等收尾时统一打。
         * 原因：统一打的话，中途崩一次就一行都没有，前面已经跑完的项也一起消失，
         * grade.py 看到的是"这个二进制什么都没测"，分不清是崩溃还是没写。
         * 逐项打之后，崩在哪一项一目了然（那一项只有 BEGIN 没有 RESULT）。
         */
        std::printf("RESULT %s\t%s\t%s\t%d\t%d\t%ld\n", r.proj.c_str(), r.name.c_str(), r.status,
          r.pass, r.fail, r.ms);
        std::fflush(stdout);
    }

private:
    std::string name_;
    std::chrono::steady_clock::time_point t0_ {};
    int failed_ = 0;
    bool exception_ = false;
};

/*
 * 收尾：打印人类可读的汇总 + 机器可读的 SCORE 行，返回进程退出码。
 * RESULT 已经逐项打过了，这里只补汇总。
 */
inline int
report(int gtest_rc)
{
    const char *proj = project();
    int ok = 0, total = 0;
    int failed_asserts = 0;

    std::printf("\n--------------------------------------------------------------------------\n");
    std::printf("  %-4s %-46s %-8s %6s %6s\n", "proj", "test", "status", "pass", "fail");
    std::printf("--------------------------------------------------------------------------\n");
    for (const CaseResult &r : results()) {
        ++total;
        failed_asserts += r.fail;
        if (std::strcmp(r.status, "PASS") == 0)
            ++ok;
        std::printf(
          "  %-4s %-46s %-8s %6d %6d", r.proj.c_str(), r.name.c_str(), r.status, r.pass, r.fail);
        if (!r.note.empty())
            std::printf("  %s", r.note.c_str());
        std::printf("\n");
    }
    std::printf("--------------------------------------------------------------------------\n");
    std::printf("  %s: %d/%d tests, %d failed assertions\n", proj, ok, total, failed_asserts);

    std::printf("SCORE %s\t%d\t%d\n", proj, ok, total);
    std::fflush(stdout);

    return (gtest_rc == 0 && ok == total) ? 0 : 1;
}

} /* namespace twt_test */

/*
 * TWT_TEST(suite, name, "Px/Ty 人类可读名字")
 *
 * 展开成「登记名字 + 原生 TEST」两段：
 *   static const bool twt_reg_... = register_name("suite.name", "Px/Ty ...");
 *   TEST(suite, name) { ... }
 *
 * 后面那个大括号是 TEST 的测例体，所以调用处**不要**加分号。
 */
#define TWT_TEST(suite, name, display)                                                             \
    [[maybe_unused]] static const bool twt_reg_##suite##_##name =                                  \
      ::twt_test::register_name(#suite "." #name, display);                                        \
    TEST(suite, name)

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
