# tinywt —— 学习 WiredTiger 实现的极简存储引擎
#
# BusTub 式组织：每个 Project 一个独立测试可执行，互不依赖。
#
#   make             # 编译 + 跑全部测试（未实现即失败）
#   make grade       # 编译 + 逐项打分（未实现记 TODO，不中断）
#   make p0-test     # 只跑 P0（p1-test / p2-test ... 同理）
#   make build       # 只编译
#   make probe       # 编译期合同：compat/wiredtiger.h + 错误码数值
#   make probe-link  # 链接期合同：缺哪个接口，链接器就报哪个符号
#   make shared      # 产出 build/libwiredtiger.so（给真 wtperf / wt 做 drop-in）
#   make cp1 / cp2   # Checkpoint gate（不计分，没过不准进下一段）
#   make capstone    # C1–C4 结业验收
#   make asan/tsan   # sanitizer 构建（ASan 抓越界，TSan 抓数据竞争）
#   make clean
#
# 注意：-MMD -MP + -include 不能省。头文件改动必须触发重编。
#       P4 起大量用 std::thread，所以 -pthread 是必需项而不是可选项。

CXX      ?= g++
CC       ?= gcc
AR       ?= ar
# 骨架阶段必然有大量「声明了但还没用上」的静态函数，屏蔽这两个警告，
# 让学生的注意力集中在 FAIL / TODO 上。
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
            -Iinclude -Icompat -MMD -MP -pthread
CFLAGS   ?= -std=c11 -O1 -g -Wall -Wextra
LDFLAGS  ?= -pthread

SRC_CORE := $(wildcard src/*.cpp)
SRC_CAPI := $(wildcard src_c/*.cpp)
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC_CORE)) \
       $(patsubst src_c/%.cpp,build/capi_%.o,$(SRC_CAPI))
LIB := build/libtinywt.a

# 共享库版本（drop-in 替换真 libwiredtiger）：必须 -fPIC 单独编一遍
PICOBJ := $(patsubst src/%.cpp,build/pic/%.o,$(SRC_CORE)) \
          $(patsubst src_c/%.cpp,build/pic/capi_%.o,$(SRC_CAPI))
SHLIB := build/libwiredtiger.so

TESTSRC   := $(wildcard test/*_test.cpp)
TESTBIN   := $(patsubst test/%_test.cpp,build/%_test,$(TESTSRC))
TESTNAMES := $(patsubst test/%_test.cpp,%,$(TESTSRC))

.PHONY: all build test grade clean probe probe-link shared cp1 cp2 capstone asan tsan

all: test

build: $(LIB) $(TESTBIN)

builddir:
	@mkdir -p build

$(OBJ) $(TESTBIN) build/probe.o build/probe: | builddir

$(LIB): $(OBJ)
	$(AR) rcs $@ $(OBJ)

build/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/capi_%.o: src_c/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# 测试：源码 + 脚手架 + 静态库一次编出（测试源很小，不做增量）
build/%_test: test/%_test.cpp test/harness.hpp $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $< -Lbuild -ltinywt $(LDFLAGS)

# 每个作业一个目标：make p0-test / p1-test ...
# 一个作业可以有多个测试二进制（P3 起：p3_api / p3_cursor / p3_txn / p3_wal）
p%-test: $(TESTBIN)
	@found=0; for b in build/p$*_*_test; do \
	  [ -x "$$b" ] || continue; \
	  found=1; echo; echo "########## $$b"; \
	  ./$$b || exit 1; \
	done; \
	[ $$found -eq 1 ] || { echo "没有 P$* 的测试二进制（build/p$*_test）"; exit 2; }

test: $(TESTBIN)
	@rc=0; for b in $(TESTBIN); do \
	  echo; echo "########## $$b"; \
	  ./$$b || rc=1; \
	done; \
	echo; \
	if [ $$rc -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "SOME TESTS FAILED"; fi; \
	exit $$rc

# 逐项打分：跑完全部二进制（不看退出码，未实现只会记 TODO），
# 由 scripts/grade.py 汇总每个 Project 的通过项。
grade: $(TESTBIN)
	@python3 scripts/grade.py $(TESTBIN)

# P3-T3.0 预置：真 wiredtiger.h 合同（compat/）能否独立编译 + 错误码数值是否对齐。
# 生成方式见 scripts/gen_wiredtiger_h.py（从 WT 11.3.x 的 wiredtiger.in 生成）。
probe: build/probe.o

build/probe.o: compat/probe.c
	$(CC) $(CFLAGS) -Icompat -c -o $@ $<

# 「链接器当评分器」：真正的评分在链接期 —— 缺哪个接口就缺哪个符号。
# 注意这里链成**共享对象**而不是可执行文件：probe.c 没有 main，我们只要
# 符号全部可解析；--no-undefined 强制把"没实现"变成链接错误而不是运行期崩溃。
probe-link: build/probe $(LIB)

build/probe: build/probe.o $(LIB)
	$(CXX) -shared -Wl,--no-undefined -o $@ build/probe.o -Lbuild -ltinywt $(LDFLAGS)

# drop-in 共享库：真 wtperf 用它替换 libwiredtiger.so，一个符号都不能缺。
shared: $(SHLIB)

build/pic/%.o: src/%.cpp
	@mkdir -p build/pic
	$(CXX) $(CXXFLAGS) -fPIC -c -o $@ $<

build/pic/capi_%.o: src_c/%.cpp
	@mkdir -p build/pic
	$(CXX) $(CXXFLAGS) -fPIC -c -o $@ $<

$(SHLIB): $(PICOBJ)
	$(CXX) -shared -o $@ $(PICOBJ) $(LDFLAGS)

# Checkpoint：不是评分项，是「过了才准进下一段」的人工门。
# cp1 = T3.0–T3.2（合同 / config+catalog / 写路径）
# cp2 = T3.3–T3.5（读路径 / 事务 / WAL）
cp1: $(LIB)
	@bash scripts/checkpoint.sh 1

cp2: $(LIB)
	@bash scripts/checkpoint.sh 2

# sanitizer 构建：P4/T4.2 要求「TSan 跑干净」，P2 的 reconcile/eviction 用 ASan 抓越界。
# ★ 会先 clean、把 sanitizer 版本编进 build/，跑完请再 make clean 回到正常构建。
asan:
	@echo "!! sanitizer 构建：跑完请 make clean"
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory test \
	  CXXFLAGS="$(CXXFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer" \
	  LDFLAGS="$(LDFLAGS) -fsanitize=address,undefined"

tsan:
	@echo "!! sanitizer 构建：跑完请 make clean"
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory test \
	  CXXFLAGS="$(CXXFLAGS) -fsanitize=thread -fno-omit-frame-pointer" \
	  LDFLAGS="$(LDFLAGS) -fsanitize=thread"

# 结业验收：C1 runner 全家桶 / C2 wt 工具 / C3 对照组 / C4 leaderboard
capstone: $(TESTBIN)
	@bash scripts/capstone.sh

-include $(OBJ:.o=.d) $(PICOBJ:.o=.d)

clean:
	rm -rf build
