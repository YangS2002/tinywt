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
#   test/<project>/*_test.cpp     每个评分项一个测试二进制
#   test/contract/abi_probe.c     编译期 + 链接期的 ABI 合同
#   third_party/wiredtiger/       真 WiredTiger 头文件（合同来源）

cmake_minimum_required(VERSION 3.16)
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
    src/api/cursor.cpp)

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
# 真 wtperf / wt 用 LD_PRELOAD 顶掉 libwiredtiger.so 时必须一个符号都不缺。
if(TINYWT_BUILD_SHARED)
  add_library(wiredtiger SHARED
      ${TINYWT_CORE_SOURCES}
      ${TINYWT_API_SOURCES})
  set_target_properties(wiredtiger PROPERTIES
      OUTPUT_NAME wiredtiger
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

    add_executable(${tname}_test ${tsrc})
    target_include_directories(${tname}_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
    target_link_libraries(${tname}_test PRIVATE tinywt)
    target_compile_options(${tname}_test PRIVATE
        -Wall -Wextra -Wno-unused-function -Wno-unused-parameter)

    add_test(NAME ${tname} COMMAND ${tname}_test)
  endforeach()
endif()
