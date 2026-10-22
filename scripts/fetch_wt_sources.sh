#!/usr/bin/env bash
#
# scripts/fetch_wt_sources.sh —— 【已废弃】转发到 scripts/wt_tools.sh
#
# 旧版只从 raw.githubusercontent 下载 19 个 .c/.h 文件到 third_party/wt/。
# 那套做法**编不出任何二进制**：
#   - wtperf 要链 libwiredtiger，源码子集没有完整的 CMake 工程
#   - test/utility 还依赖更多内部头
#   - C1 要的 104 个 runner 配置（bench/wtperf/runners/*.wtperf）一个都没下
# 结果就是 capstone 永远 SKIP，而且看起来"已经准备好了"。
#
# 现在统一走完整克隆 + cmake 构建。保留此文件只为不让旧文档/旧命令直接报错。
set -euo pipefail

echo "fetch_wt_sources.sh 已废弃：改走完整克隆 + 构建（make wt-tools）" >&2
exec bash "$(cd "$(dirname "$0")" && pwd)/wt_tools.sh" "$@"
