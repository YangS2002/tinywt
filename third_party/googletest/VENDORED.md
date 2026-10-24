# googletest（内置副本）

- 版本：**v1.14.0**（2023-08 发布）
- 来源：`https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz`
- 保留：`include/`、`src/`、`LICENSE`、`README.googletest.md`
- 去掉：`samples/`、`test/`、`docs/`、`cmake/`（用不到，删掉少 700 个文件）

## 为什么内置而不是 find_package / apt

`make` 是这套作业的唯一入口，必须做到 **clone 下来就能编**：

- `apt install libgtest-dev` 要 sudo —— 不是所有环境都给的
- `find_package(GTest)` 在没装包的机器上直接 configure 失败
- `FetchContent` 每次 configure 都要联网，断网就编不动

## 怎么编

不跑上游的 CMakeLists（顶层那份会把 googlemock 一起 `add_subdirectory`，我们不用 mock）。
`CMakeLists.txt` 里只编一个 TU：

```cmake
add_library(twt_gtest STATIC third_party/googletest/src/gtest-all.cc)
target_include_directories(twt_gtest PUBLIC third_party/googletest/include
                                       PRIVATE third_party/googletest)
```

`gtest-all.cc` 里是 `#include "src/gtest-*.cc"`，所以私有 include 要给到 gtest 根目录。

## 怎么升级

```bash
cd /tmp && curl -sSL -o gt.tar.gz \
  https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
tar xzf gt.tar.gz
cd /home/ys/tinywt
cp -r /tmp/googletest-1.15.2/googletest/include third_party/googletest/
cp -r /tmp/googletest-1.15.2/googletest/src     third_party/googletest/
cp /tmp/googletest-1.15.2/LICENSE               third_party/googletest/
```

升完跑一次 `make grade`：结果必须还是 `3/49`（骨架状态下），
状态分布也必须是 3 PASS / 1 FAIL / 45 NOTIMPL —— 变了就说明新版本的
`TestEventListener` / `TestPartResult` 语义动了，`test/support/harness.hpp` 要跟着改。
