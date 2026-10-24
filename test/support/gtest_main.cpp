/*-
 * gtest_main.cpp —— 每个测试二进制的 main()（由 CMake 逐个链进来）
 *
 * 为什么不用 GoogleTest 自带的 gtest_main：
 *   我们需要「每项打 BEGIN/RESULT + 每项一个闹钟 + 崩溃也算出结果」，
 *   这些都要在 main 里挂监听器、改 flag，自带 main 没地方插。
 *
 * 为什么 TWT_PROJECT 走编译宏而不是写死在测例里：
 *   监听器在**这一个** TU 里，测试用例在别的 TU 里；Project 标签是二进制的属性，
 *   由 CMake 按 target 名（p3_txn → P3）定义，测例文件里不用重复写。
 * -*/
#include "support/harness.hpp"

#include <map>

#ifndef TWT_PROJECT
#define TWT_PROJECT "P?"
#endif

namespace twt_test {

namespace {

/*
 * "Suite.Name" → 人类可读名字。
 * 函数内 static：静态初始化顺序问题不存在（第一次调用时才构造）。
 */
std::map<std::string, std::string> &
registry()
{
    static std::map<std::string, std::string> m;
    return m;
}

} /* namespace */

bool
register_name(const std::string &gtest_name, const char *display)
{
    registry()[gtest_name] = (display != nullptr && *display != '\0') ? display : gtest_name;
    return true;
}

const std::string &
display_name(const std::string &gtest_name)
{
    auto it = registry().find(gtest_name);
    if (it == registry().end())
        it = registry().emplace(gtest_name, gtest_name).first; /* 没登记就退回 gtest 名 */
    return it->second;
}

const char *
project()
{
    return TWT_PROJECT;
}

} /* namespace twt_test */

int
main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    /*
     * 默认参数走 argv 注入，而不是在 InitGoogleTest 之后改 FLAGS_gtest_*：
     * gtest 在 InitGoogleTest 内部就按 brief 选好了 result printer，之后再改已经晚了
     * （[ RUN ] / [ OK ] 照样会打）。命令行显式传 --gtest_brief=0 仍然能覆盖。
     *
     * catch_exceptions=1 是硬要求：骨架里每个待实现的函数体都是一行
     * UNIMPLEMENTED(...)，抛 NotImplemented。接不住的话异常穿过 RUN_ALL_TESTS
     * 直接 std::terminate，后面的测例一条都跑不到 —— 这正是旧脚手架 try/catch 兜的。
     */
    std::vector<char *> args(argv, argv + argc);
    args.push_back(const_cast<char *>("--gtest_brief=1"));
    args.push_back(const_cast<char *>("--gtest_catch_exceptions=1"));
    int n = (int)args.size();
    args.push_back(nullptr);

    ::testing::InitGoogleTest(&n, args.data());

    /*
     * 信号处理器后装：gtest 会在 InitGoogleTest 里装它自己的（fail fast / absl），
     * 而崩溃要报成 RESULT ... CRASH(SEGV) 这样一行 —— grade.py 只认 RESULT 行，
     * 一行栈回溯对算分没用。
     */
    twt_hx::install();

    ::testing::UnitTest::GetInstance()->listeners().Append(new twt_test::Listener);

    const int gtest_rc = RUN_ALL_TESTS();
    return twt_test::report(gtest_rc);
}
