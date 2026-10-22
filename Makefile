# tinywt —— 学习 WiredTiger 实现的极简存储引擎
#
# BusTub 式组织：每个 Project 一个独立测试可执行，互不依赖。
#
#   make             # 编译 + 跑全部测试（未实现即失败）
#   make grade       # 编译 + 逐项打分（未实现记 NOTIMPL，不中断）
#   make p0-test     # 只跑 P0（p1-test / p2-test ... 同理）
#   make build       # 只编译
#   make probe       # 校验 compat/wiredtiger.h 合同头可独立编译
#   make clean
#
# 注意：-MMD -MP + -include 不能省。头文件改动必须触发重编。

CXX      ?= g++
CC       ?= gcc
AR       ?= ar
# 骨架阶段必然有大量「声明了但还没用上」的静态函数，屏蔽这两个警告，
# 让学生的注意力集中在 FAIL / NOTIMPL 上。
CXXFLAGS ?= -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
            -Iinclude -Icompat -MMD -MP
CFLAGS   ?= -std=c11 -O1 -g -Wall -Wextra
LDFLAGS  ?=

SRC_CORE := $(wildcard src/*.cpp)
SRC_CAPI := $(wildcard src_c/*.cpp)
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC_CORE)) \
       $(patsubst src_c/%.cpp,build/capi_%.o,$(SRC_CAPI))
LIB := build/libtinywt.a

TESTSRC   := $(wildcard test/*_test.cpp)
TESTBIN   := $(patsubst test/%_test.cpp,build/%_test,$(TESTSRC))
TESTNAMES := $(patsubst test/%_test.cpp,%,$(TESTSRC))

.PHONY: all build test grade clean probe

all: test

build: $(LIB) $(TESTBIN)

builddir:
	@mkdir -p build

$(OBJ) $(TESTBIN) build/probe.o: | builddir

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
# 一个作业可以有多个测试二进制（P3 起：p3_api / p3_cursor / p3_wal）
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

# 逐项打分：跑完全部二进制（不看退出码，未实现只会记 NOTIMPL），
# 由 scripts/grade.py 汇总每个 Project 的通过项。
grade: $(TESTBIN)
	@python3 scripts/grade.py $(TESTBIN)

# P3-T3.0 预置：真 wiredtiger.h 合同（compat/）能否独立编译。
# 生成方式见 scripts/gen_wiredtiger_h.py（从 WT 11.3.x 的 wiredtiger.in 生成）。
probe: build/probe.o

build/probe.o: compat/probe.c
	$(CC) $(CFLAGS) -Icompat -c -o $@ $<

-include $(OBJ:.o=.d)

clean:
	rm -rf build
