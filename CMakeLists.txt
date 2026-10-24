# tinywt —— 学习 WiredTiger 实现的极简存储引擎
#
# 本文件是构建的唯一事实来源（source of truth）。
# 顶层 Makefile 只是便捷封装，转发到 cmake，方便 `make grade` / `make p2-test`。
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build -j
#   ctest --test-dir build --output-on-failure
#
# 目录约定（与 BusTub 同构）：
#   include/tinywt/<module>/*.h   公共头（学生要读，不要改布局）
#   src/<module>/*.cpp            实现（学生要写函数体）
#   src/api/*.cpp                 C ABI 层；capi.h 是私有头，不对外暴露
#   test/<project>/*_test.cpp     每个 Project 一个测试二进制（GoogleTest 写测例）
#   test/support/harness.hpp      GoogleTest 适配层：BEGIN / RESULT / SCORE + 崩溃看门狗
#   test/contract/abi_probe.c     编译期 + 链接期的 ABI 合同
#   third_party/wiredtiger/       真 WiredTiger 头文件（合同来源）
#   third_party/googletest/       测试框架源码（内置，不依赖系统包）

cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------- 编译器守卫
# 必须放在 project() **之前**：CMake 在 project() 里做编译器探测，
# 一旦选中 Windows 的 mingw gcc.exe，它不认 /home/... 路径，探测阶段就炸，
# 而且只报一句 "The C compiler ... is not able to compile a simple test program"，
# 极难定位。到 project() 之后再检查就晚了。
#
# 覆盖两种真实场景：
#   ① 命令行显式 -DCMAKE_C_COMPILER=/mnt/c/.../gcc.exe
#   ② 上次 configure 已经把坏编译器写进 CMakeCache.txt（缓存会一直复用，
#      换机器 / 换 PATH 之后不删缓存就会一直踩）
#
# 顺带说明：Linux 上 CMake 只会找名字恰好是 `gcc` 的可执行文件，
# Windows 那个叫 `gcc.exe`，所以"PATH 里混进 mingw64/bin"本身不会让自动探测选错；
# Makefile 的 configure 也显式钉死了 gcc/g++（见 Makefile 的 TWT_CC / TWT_CXX）。
foreach(_comp "$CACHE{CMAKE_C_COMPILER}" "$CACHE{CMAKE_CXX_COMPILER}")
  if(_comp AND (_comp MATCHES "\\.exe$" OR _comp MATCHES "^/mnt/"))
    message(FATAL_ERROR
      "编译器落在 Windows 侧：${_comp}\n"
      "在 WSL 里必须用 Linux 的 gcc/g++ —— Windows 的 mingw gcc.exe 不认 /home/... 路径。\n"
      "修法：rm -rf build 后重新 make（Makefile 会钉死 $(command -v gcc) 那两个）。")
  endif()
endforeach()

project(tinywt LANGUAGES C CXX)

# ---------------------------------------------------------------- 全局设置
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

option(TINYWT_BUILD_TESTS "构建评分用测试可执行" ON)
option(TINYWT_BUILD_SHARED "构建 drop-in 用的 libwiredtiger.so" ON)
set(TINYWT_SANITIZE "" CACHE STRING "address / thread / 空（不启用）")

# 我们声明的 ABI 版本。必须等于 third_party/wiredtiger/include/wiredtiger.h 里的
# WIREDTIGER_VERSION_MAJOR.MINOR.PATCH（check_docs 会核对，改头文件时别忘这里）。
# 它决定 .so 的 SONAME：真工具写的是 NEEDED [libwiredtiger.so.11.3.0]，
# SONAME 对不上就只能 LD_PRELOAD 叠加，对上了才能真替换。
set(TINYWT_ABI_VERSION "11.3.0")

if(TINYWT_SANITIZE)
  message(STATUS "sanitizer: ${TINYWT_SANITIZE}")
  add_compile_options(-fsanitize=${TINYWT_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${TINYWT_SANITIZE})
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------- 源文件清单
# 一个模块一个目录，和 include/tinywt/<module>/ 一一对应。
set(TINYWT_CORE_SOURCES
    src/common/defs.cpp
    src/storage/block.cpp
    src/storage/cell.cpp
    src/storage/page.cpp
    src/compression/compress.cpp
    src/btree/btree.cpp
    src/btree/reconcile.cpp
    src/txn/txn.cpp
    src/txn/history_store.cpp
    src/wal/log.cpp
    src/concurrency/hazard.cpp)

set(TINYWT_API_SOURCES
    src/api/config.cpp
    src/api/connection.cpp
    src/api/cursor.cpp
    # 工具面内部 ABI（34 个 __wt_*）：只被真 wt / wtperf 调用，单测不碰。
    src/api/wt_tool_abi.cpp)

# ---------------------------------------------------------------- 静态库
add_library(tinywt STATIC
    ${TINYWT_CORE_SOURCES}
    ${TINYWT_API_SOURCES})

target_include_directories(tinywt
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wiredtiger/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)   # 只为 src/api/capi.h 这种私有头

target_compile_options(tinywt PRIVATE
    -Wall -Wextra -Wno-unused-function -Wno-unused-parameter)

# 静态库也编成 PIC：abi_link_check 要把整个内核链进一个共享对象，
# 非 PIC 的 .o 在这一步会直接报 "recompile with -fPIC"。
set_target_properties(tinywt PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_link_libraries(tinywt PUBLIC Threads::Threads)

# ---------------------------------------------------------------- drop-in 共享库
# 真 wtperf / wt 换成我们的 .so 时（LD_LIBRARY_PATH + 同 SONAME）必须一个符号都不缺。
if(TINYWT_BUILD_SHARED)
  add_library(wiredtiger SHARED
      ${TINYWT_CORE_SOURCES}
      ${TINYWT_API_SOURCES})
  set_target_properties(wiredtiger PROPERTIES
      OUTPUT_NAME wiredtiger
      # ★ SONAME 必须和真库一模一样（libwiredtiger.so.11.3.0）：
      #   真工具的可执行文件里写的是 NEEDED [libwiredtiger.so.11.3.0]，
      #   只有同名同 SONAME 才能靠 LD_LIBRARY_PATH **真正替换**它。
      #   用 LD_PRELOAD 叠加是不够的：真库还会被一起加载，我们没导出的 __wt_*
      #   会被真库静默兜底 —— 那种"通过"是假的。
      #   SOVERSION 写 11.3.0 而不是 11.3：WT 的 SONAME 带 patch 号。
      VERSION ${TINYWT_ABI_VERSION}
      SOVERSION ${TINYWT_ABI_VERSION}
      POSITION_INDEPENDENT_CODE ON
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  target_include_directories(wiredtiger
      PUBLIC
          ${CMAKE_CURRENT_SOURCE_DIR}/include
          ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wiredtiger/include
      PRIVATE
          ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_options(wiredtiger PRIVATE
      -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -fPIC)
  target_link_libraries(wiredtiger PUBLIC Threads::Threads)
endif()

# ---------------------------------------------------------------- 作业客户端
# bench/wt_client.c 只 include 公开的 wiredtiger.h，走和 wtperf 同一套方法表。
# 链接我们的 .so 只是为了拿到 NEEDED [libwiredtiger.so.11.3.0]；运行时
# 用 LD_LIBRARY_PATH 换库 —— 同一个二进制既能跑 tinywt 也能跑真 WT。
#
# 它不属于评分面（不产生 RESULT 行），是 make bench / diff-wt / crash 的执行体。
if(TINYWT_BUILD_SHARED)
  add_executable(wt_client bench/wt_client.c)
  target_include_directories(wt_client PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wiredtiger/include)
  target_link_libraries(wt_client PRIVATE wiredtiger)
  target_compile_options(wt_client PRIVATE -Wall -Wextra)
  set_target_properties(wt_client PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# ---------------------------------------------------------------- ABI 合同
# 1) abi_probe_obj：编译期合同（_Static_assert 钉死错误码数值、结构体首成员）
add_library(abi_probe_obj OBJECT test/contract/abi_probe.c)
target_include_directories(abi_probe_obj PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wiredtiger/include)
set_target_properties(abi_probe_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(abi_probe_obj PRIVATE -Wall -Wextra)

# 2) abi_link_check：链接期合同 —— 缺哪个接口，链接器直接报哪个符号
add_library(abi_link_check MODULE $<TARGET_OBJECTS:abi_probe_obj>)
target_link_libraries(abi_link_check PRIVATE tinywt)
target_link_options(abi_link_check PRIVATE -Wl,--no-undefined)
set_target_properties(abi_link_check PROPERTIES
    PREFIX "" OUTPUT_NAME "abi_link_check"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ---------------------------------------------------------------- 测试
# 一个测试二进制 = 一个 Project 的一组评分项（harness 内部再细分 RUN/CASE）。
# 命名 <project>_<name>_test，和 make p2-test 的通配一致。
if(TINYWT_BUILD_TESTS)
  enable_testing()

  # -------------------------------------------------------------- GoogleTest
  # 测试框架用 GoogleTest，但源码**内置**在 third_party/googletest：
  #   - 这台机器 apt 装 libgtest-dev 要 sudo（本项目要求 clone 下来就能 make）
  #   - 不走 find_package(GTest)，同一个仓库在任何机器上编出来都一样
  #   - 也不跑上游顶层 CMakeLists：它会把 googlemock 一起 add_subdirectory，
  #     而我们不用 mock。融合编译只编 gtest-all.cc，一个静态库零外部依赖。
  add_library(twt_gtest STATIC
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest/src/gtest-all.cc)
  target_include_directories(twt_gtest
      PUBLIC
          ${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest/include
      PRIVATE
          # gtest-all.cc 里是 #include "src/..."，所以要给到 gtest 根目录
          ${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest)
  target_link_libraries(twt_gtest PUBLIC Threads::Threads)
  target_compile_options(twt_gtest PRIVATE -w)  # 上游代码，不套本项目的告警策略

  set(TINYWT_TESTS
      p0_defs:test/p0/defs_test.cpp
      p1_block_cell:test/p1/block_cell_test.cpp
      p2_btree:test/p2/btree_test.cpp
      p2_codec:test/p2/codec_test.cpp
      p2_compress:test/p2/compress_test.cpp
      p3_api:test/p3/api_test.cpp
      p3_cursor:test/p3/cursor_test.cpp
      p3_txn:test/p3/txn_test.cpp
      p3_wal:test/p3/wal_test.cpp
      p4_concurrency:test/p4/concurrency_test.cpp
      p4_hs_rts:test/p4/hs_rts_test.cpp)

  foreach(entry IN LISTS TINYWT_TESTS)
    string(REPLACE ":" ";" parts "${entry}")
    list(GET parts 0 tname)
    list(GET parts 1 tsrc)

    # gtest_main.cpp 提供 main()：挂监听器、改 flag、调 RUN_ALL_TESTS。
    # 它每个 target 编一遍（就一个 TU），因为 TWT_PROJECT 是 target 的属性。
    add_executable(${tname}_test ${tsrc} test/support/gtest_main.cpp)
    target_include_directories(${tname}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
    target_link_libraries(${tname}_test PRIVATE tinywt twt_gtest)

    # Project 标签从 target 名推：p3_txn → P3。BEGIN/RESULT 行的第一列，
    # grade.py 靠它分组算分，check_docs.py 靠它把测例归到 Task 上。
    string(SUBSTRING ${tname} 0 2 _proj_lower)
    string(TOUPPER ${_proj_lower} _proj)
    target_compile_definitions(${tname}_test PRIVATE TWT_PROJECT="${_proj}")

    target_compile_options(${tname}_test PRIVATE
        -Wall -Wextra -Wno-unused-function -Wno-unused-parameter)

    add_test(NAME ${tname} COMMAND ${tname}_test)
  endforeach()
endif()
