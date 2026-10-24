# tinywt —— 便捷封装层
#
# 构建的事实来源是 CMakeLists.txt；本文件只是把常用动作包成短命令，
# 免得每次敲 cmake --build。想接 IDE / CI 就直接用 CMake。
#
#   make                  # 编译 + 跑全部测试
#   make build            # 只编译
#   make grade            # 逐项打分（未实现记 TODO，不中断）
#   make p2-test          # 只跑某一个 Project
#   make p2-test FILTER='P2_Btree.*'   # 只跑匹配的测例（透传 --gtest_filter）
#   make todo             # 我这部分要写哪些函数？（按 Task 分组，带 file:line）
#   make check-docs       # 文档里的数字/路径/链接与代码对账（改完文档跑一次）
#   make format           # 按根目录 .clang-format 格式化 src/ include/ test/
#   make probe            # 编译期合同：错误码数值 + 结构体布局
#   make probe-link       # 链接期合同：缺哪个接口，链接器就报哪个符号
#   make shared           # build/libwiredtiger.so（给真 wtperf / wt 做 drop-in）
#   make wt-tools         # 克隆固定 tag 的真 WT 并编出 wt / wtperf（一次性）
#   make wt-abi           # 量「真工具要的符号 - 我们导出的」还差哪几个
#   make cp1 / cp2        # Checkpoint gate（不计分，没过不准进下一段）
#   make capstone         # C1–C4 结业验收
#   make bench            # 自己的性能曲线（不等 wtperf；REAL=1 对比真 WT）
#   make diff-wt          # 行为对拍：同一操作流，tinywt vs 真 WT，dump 必须一致
#   make crash            # 崩溃一致性：SIGKILL 后重开，已提交前缀不能丢
#   make doctor           # 环境体检：工具链 / 真 WT / 构建产物 / 磁盘
#   make asan / tsan      # 独立的 sanitizer 构建目录，不污染 build/
#   make clean
#
# 构建产物：
#   build/bin/<project>_<name>_test    测试二进制
#   build/lib/libtinywt.a              C++ 内核静态库
#   build/libwiredtiger.so             drop-in 共享库
#   build/bin/wt_client                ABI 级客户端（bench / diff / crash）

BUILD      ?= build
BUILD_ASAN ?= build-asan
BUILD_TSAN ?= build-tsan
CMAKE      ?= cmake
TYPE       ?= Debug
JOBS       ?= $(shell nproc 2>/dev/null || echo 4)

.PHONY: all configure build test grade clean probe probe-link shared perf-shared \
        cp1 cp2 capstone asan tsan todo check-docs format wt-tools wt-abi \
        bench diff-wt crash doctor

all: test

# ---------------------------------------------------------------- 构建
# 显式钉死编译器：WSL 会把 Windows 的 PATH 追加进来，其中
# /mnt/c/Users/26600/mingw64/bin 里是 Windows 的 gcc.exe —— 它不认 /home/...
# 路径，被 CMake 选中后只会报 "is not able to compile a simple test program"。
# command -v gcc 解析的是 Linux 侧的名字（Windows 那个叫 gcc.exe，不会被它抢走）。
TWT_CC  := $(shell command -v gcc)
TWT_CXX := $(shell command -v g++)

configure:
	@$(CMAKE) -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=$(TYPE) \
	  -DCMAKE_C_COMPILER=$(TWT_CC) -DCMAKE_CXX_COMPILER=$(TWT_CXX) >/dev/null

build: configure
	@$(CMAKE) --build $(BUILD) -j $(JOBS)

# ---------------------------------------------------------------- 测试
# 每个 Project 一个或多个二进制；任一 FAIL 就非零退出。
# FILTER= 透传给 GoogleTest 的 --gtest_filter（按 Suite.Name 通配），
# 调单个测例时不用再记二进制路径和 ASCII 测试名。
test: build
	@rc=0; for b in $(BUILD)/bin/*_test; do \
	  [ -x "$$b" ] || continue; \
	  echo; echo "########## $$b"; \
	  ./$$b $(if $(FILTER),--gtest_filter='$(FILTER)') || rc=1; \
	done; \
	echo; \
	if [ $$rc -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "SOME TESTS FAILED"; fi; \
	exit $$rc

# 单个 Project：make p0-test / make p2-test / make p4-test
p%-test: build
	@found=0; for b in $(BUILD)/bin/p$*_*_test; do \
	  [ -x "$$b" ] || continue; \
	  found=1; echo; echo "########## $$b"; \
	  ./$$b $(if $(FILTER),--gtest_filter='$(FILTER)') || exit 1; \
	done; \
	[ $$found -eq 1 ] || { echo "没有 P$* 的测试二进制"; exit 2; }

# 逐项打分：跑完全部二进制（不看退出码，未实现只记 TODO），由 grade.py 汇总
grade: build
	@python3 scripts/grade.py $(BUILD)/bin/*_test

# ---------------------------------------------------------------- 合同
# 编译期：_Static_assert 把错误码数值、iface 首成员布局钉死
probe: configure
	@$(CMAKE) --build $(BUILD) --target abi_probe_obj -j $(JOBS)

# 链接期：链成共享对象 + --no-undefined，缺哪个符号就报哪个
probe-link: build
	@$(CMAKE) --build $(BUILD) --target abi_link_check -j $(JOBS)

# drop-in 共享库（同 SONAME 顶掉 libwiredtiger.so.11.3.0，不是 LD_PRELOAD 叠加）
shared: build
	@$(CMAKE) --build $(BUILD) --target wiredtiger -j $(JOBS)

# Release 版 drop-in 库 —— **性能对比必须用它**。
# 原因：build/ 是 Debug(-O0)，真 WT 是 Release；拿 -O0 的库去比吞吐，
# 量到的是编译选项的差距，不是实现的差距。C3 用法：
#   SHLIB=$PWD/build-perf/libwiredtiger.so make capstone
perf-shared:
	@$(CMAKE) -S . -B build-perf -DCMAKE_BUILD_TYPE=Release -DTINYWT_BUILD_TESTS=OFF >/dev/null
	@$(CMAKE) --build build-perf -j $(JOBS) --target wiredtiger
	@echo "Release 库：build-perf/libwiredtiger.so.11.3.0"

# ---------------------------------------------------------------- 真工具链
# 一次性准备：克隆固定 tag 的 WT + 编出 wt / wtperf（源码在 external/，不进仓库）
wt-tools:
	@bash scripts/wt_tools.sh

# 量工具面缺口：真工具要的符号 - 我们 .so 导出的符号。
# 这是 C1/C2 的进度条，不猜数字，直接 nm 比。
wt-abi: shared
	@python3 scripts/wt_abi.py

# ---------------------------------------------------------------- 人工门
# cp1 = T3.0–T3.2（合同 / config+catalog / 写路径）
# cp2 = T3.3–T3.5（读路径 / 事务 / WAL）
cp1: build
	@bash scripts/checkpoint.sh 1

cp2: build
	@bash scripts/checkpoint.sh 2

# 结业验收：C1 runner 全家桶 / C2 wt 工具 / C3 对照组 / C4 leaderboard
capstone: build
	@bash scripts/capstone.sh

# 自检 C1/C2 的**判据**：把 SHLIB 指向真库跑一遍，应该全绿。
# 它不验你的实现，验的是"断言有没有写错"—— 判据必须在正确实现上先跑通一次，
# 否则出现"永远 FAIL 的检查"，你会一直以为是自己代码有问题。
# 自检模式跳过 C1 的 runner、C3 对照组和 C4 榜单（两边本来就是同一个库）。
capstone-selftest: build
	@SELFTEST=1 RUNNERS="" SHLIB=$$PWD/external/wiredtiger/build/libwiredtiger.so \
	  bash scripts/capstone.sh

# ---------------------------------------------------------------- 观测与对拍
# 三个都不属于评分面。它们回答三个评分器回答不了的问题：
#   现在多快？         → make bench     （Release 构建，中位数，可选 REAL=1 对比真 WT）
#   行为和 WT 一样吗？ → make diff-wt   （同一确定性操作流，两个引擎 dump 逐字节比）
#   崩了数据还对吗？   → make crash     （SIGKILL 后重开，已提交前缀不能丢）
# 执行体都是 bench/wt_client.c —— 它只走公开 C ABI，和 wtperf 同一套方法表。
bench:
	@bash scripts/bench.sh

diff-wt: build
	@bash scripts/diff_wt.sh

crash: build
	@bash scripts/crash.sh

doctor:
	@bash scripts/doctor.sh

# ---------------------------------------------------------------- sanitizer
# 用独立构建目录：避免「跑完 asan 忘了 make clean」这类坑。
asan:
	@$(CMAKE) -S . -B $(BUILD_ASAN) -DCMAKE_BUILD_TYPE=Debug \
	  -DTINYWT_SANITIZE=address >/dev/null
	@$(CMAKE) --build $(BUILD_ASAN) -j $(JOBS)
	@ctest --test-dir $(BUILD_ASAN) --output-on-failure

tsan:
	@$(CMAKE) -S . -B $(BUILD_TSAN) -DCMAKE_BUILD_TYPE=Debug \
	  -DTINYWT_SANITIZE=thread >/dev/null
	@$(CMAKE) --build $(BUILD_TSAN) -j $(JOBS)
	@ctest --test-dir $(BUILD_TSAN) --output-on-failure

# ---------------------------------------------------------------- 格式
# 根目录 .clang-format 是唯一事实来源（Allman / 4 空格 / 100 列）。
# 没装 clang-format 就**报明确错误**，绝不静默跳过 ——
# 一个"看着成功其实什么都没做"的格式化步骤，比没有格式化更危险。
CLANG_FORMAT ?= $(shell command -v clang-format 2>/dev/null || command -v clang-format-14 2>/dev/null)
FORMAT_DIRS := src include test

format:
	@if [ -z "$(CLANG_FORMAT)" ]; then \
	  echo "clang-format 没装 —— 这一步不会静默跳过。"; \
	  echo "  安装（无需 sudo）：pip install --user clang-format"; \
	  echo "  然后确认 ~/.local/bin 在 PATH 里：export PATH=\"\$$HOME/.local/bin:\$$PATH\""; \
	  exit 1; \
	fi
	@find $(FORMAT_DIRS) \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) -print0 \
	  | xargs -0 $(CLANG_FORMAT) -i
	@echo "格式化完成：$(FORMAT_DIRS)（按 .clang-format）"

# ---------------------------------------------------------------- 导航
# 「我该写什么」：把所有 UNIMPLEMENTED 按 Task 分组列出来。
#   make todo            全量
#   make todo P=P2       只看一段
#   make todo P=P4/T4.3  精确到一个 Task
#   make todo c=1        只看计数
todo:
	@python3 scripts/todo.py $(if $(P),-p $(P)) $(if $(c),-c)

# 文档对账：项目地图的数字、子项目表、路径、链接、make 目标，逐条与仓库比。
# 改完文档，或挪了文件 / 加了测例之后跑一次，防止文档漂移。
check-docs: build
	@python3 scripts/check_docs.py

clean:
	rm -rf $(BUILD) $(BUILD_ASAN) $(BUILD_TSAN) build-perf
