#!/usr/bin/env bash
#
# scripts/lib_wt.sh —— 被 checkpoint.sh / capstone.sh source 的真 WT 依赖定位层
#
# 只有一份实现，避免两个脚本各判一次、判法还不一样。
# 关键约定：**目录存在 ≠ 源码有效**。旧逻辑用 [ -d third_party/wt ] 判断，
# 结果一个只有 wiredtiger.in 的空壳目录就能让全部门 SKIP —— 那不是"没准备好"，
# 是"假装找过了"。这里改为检查真实文件。
#
# 提供：
#   wt_src_root    选真 WT 源码根（无效树一律跳过）
#   wt_find_bin    按候选顺序找第一个可执行文件
#   wt_howto       缺依赖时打印补齐方式（统一口径，不各写一套）

# 真 WT 源码树的判据：wtperf 的入口源 + 内部头目录同时在。
_twt_is_wt_tree() {
    [ -n "${1:-}" ] && [ -f "$1/bench/wtperf/wtperf.c" ] && [ -d "$1/src/include" ]
}

# 由 wt_setup_dropin 填充。默认空 = 不替换，直接跑真工具（C3 的对照组要这个）。
WT_MODE=""
WT_ENV=()
WT_DESC=""

# wt_src_root → stdout。优先 $WT_SRC（显式指定必须有效，否则报错而不是静默降级）。
wt_src_root() {
    if [ -n "${WT_SRC:-}" ]; then
        if _twt_is_wt_tree "$WT_SRC"; then
            printf '%s' "$WT_SRC"
            return 0
        fi
        echo "WT_SRC=$WT_SRC 不是完整的 WiredTiger 源码树（缺 bench/wtperf/wtperf.c）" >&2
        return 1
    fi

    local c
    for c in third_party/wt external/wiredtiger; do
        if _twt_is_wt_tree "$c"; then
            printf '%s' "$c"
            return 0
        fi
    done

    # 都没找到：给一个"本来就是它该在的地方"作为默认值，方便报错信息可读。
    printf '%s' "external/wiredtiger"
    return 1
}

# wt_find_bin <候选...> → 打印第一个可执行的；全都不行时打印第一个候选并返回 1。
wt_find_bin() {
    local cand
    for cand in "$@"; do
        if [ -x "$cand" ]; then
            printf '%s' "$cand"
            return 0
        fi
    done
    printf '%s' "${1:-}"
    return 1
}

# wt_needed_soname <binary> → 打印可执行文件 NEEDED 里的 libwiredtiger*.so*
wt_needed_soname() {
    readelf -d "$1" 2>/dev/null |
        sed -n 's/.*NEEDED.*\[\(libwiredtiger[^]]*\)\].*/\1/p' | head -1
}

# wt_setup_dropin <binary> <our_shlib>
#
# 决定"怎么让真工具用上我们的库"，设置三个全局：
#   WT_MODE = replace | preload
#   WT_ENV  = 调用工具时要加的 env 前缀（数组）
#   WT_DESC = 给人看的一句话说明
#
# ★★ 只能在**当前 shell** 里调用，绝对不能写成 `desc=$(wt_setup_dropin ...)` ——
#    命令替换开子 shell，WT_ENV 一赋值就随子 shell 消失，调用方拿到的是空数组，
#    于是工具照常按 RUNPATH 找到真库跑得飞快、全部通过。这是"最贵的假通过"：
#    检查脚本本身是对的，但它检查的是真 WT。要拿说明文字请用 wt_dropin_desc。
#
# ★ 为什么优先 replace 而不是 LD_PRELOAD：
#   工具的可执行文件里写死了 NEEDED [libwiredtiger.so.11.3.0]，所以 LD_PRELOAD 时
#   **真库仍然会被加载**。我们没导出的 __wt_* 会被真库静默兜底 —— 于是"跑通了"
#   可能是一半我们的实现 + 一半真 WT，这种通过是假的（实测 __wt_getopt 就绑到真库）。
#   只要我们的 SONAME 和 NEEDED 一致，就可以用 LD_LIBRARY_PATH 把它**替换**掉，
#   真库根本不进场，缺符号会硬报 undefined symbol。
wt_setup_dropin() {
    local bin="$1" shlib="$2" need dir
    need=$(wt_needed_soname "$bin")
    dir=$(dirname "$shlib")
    # 先清空：上一次的设置（比如 C1 用 wtperf、C2 用 wt）不能漏到下一次
    WT_MODE=""
    WT_ENV=()
    WT_DESC=""

    if [ -z "$need" ]; then
        WT_MODE=preload
        WT_ENV=(env LD_PRELOAD="$shlib")
        WT_DESC="preload（读不出 NEEDED，退回预加载）"
        return 0
    fi

    if [ -f "$dir/$need" ]; then
        WT_MODE=replace
        WT_ENV=(env LD_LIBRARY_PATH="$dir")
        WT_DESC="replace（$need 同名同 SONAME，真库不加载）"
    else
        WT_MODE=preload
        WT_ENV=(env LD_PRELOAD="$shlib")
        WT_DESC="preload（缺 $need：真库仍参与解析，__wt_* 缺口会被它兜底）"
    fi
    return 0
}

# wt_dropin_desc → 打印 WT_DESC。纯读全局，放 $( ) 里是安全的。
wt_dropin_desc() {
    printf '%s' "$WT_DESC"
}

# wt_check_dropin <binary> <our_shlib>
#
# 校验"替换真的生效了"：直接问动态链接器最后解析到哪个文件。
# replace 模式下必须是我们的 .so —— 环境变量写对了但被 RUNPATH/RPATH 抢走、
# 或者像上面那样压根没传下去，都会在这里露出来，而不是变成一次通过。
# 成功返回 0；失败时把**实际解析到的路径**打到 stdout，供报错用。
wt_check_dropin() {
    local bin="$1" shlib="$2" dir got
    dir=$(dirname "$shlib")
    case "$WT_MODE" in
    replace)
        got=$(LD_LIBRARY_PATH="$dir" ldd "$bin" 2>/dev/null |
            awk '/libwiredtiger/ { print $3; exit }')
        # 比 realpath，不比字面量：ldd 报的是 SONAME 那个文件，我们传进来的是
        # libwiredtiger.so 这个软链 —— 直接字符串比较会把对的判成错的。
        if [ -n "$got" ] && [ "$(readlink -f "$got")" = "$(readlink -f "$shlib")" ]; then
            return 0
        fi
        printf '%s' "$got"
        return 1
        ;;
    preload)
        # LD_PRELOAD 下 ldd 看不出来（它只列 NEEDED）；只能确认库文件本身在
        [ -f "$shlib" ] && return 0
        printf '%s' "库文件不存在：$shlib"
        return 1
        ;;
    *)
        printf '%s' "WT_MODE 还是空的 —— wt_setup_dropin 没在同一个 shell 里调用"
        return 1
        ;;
    esac
}

# wt_howto —— 缺 wtperf / wt 时统一提示。
wt_howto() {
    echo "        → make wt-tools        # 浅克隆固定 tag + cmake 构建 wt / wtperf"
    echo "        → 或 WT_SRC=/path/to/wiredtiger 指向一个已构建的完整克隆"
}

# resolve_runner <runners_dir> <name> → 打印 runner 配置的真实路径
#
# ★ WT 仓库里的 runner 命名**不统一**：有的就叫 <name>.wtperf，
#   有的带规模前缀（500m-btree- / medium-btree- …）。
#   直接拼 "$name.wtperf" 会静默找不到，表现成"runner 全 FAIL"，看着像实现错了。
#   三级回退：精确名 → 500m-btree- 前缀 → 任意 *-<name>.wtperf。
resolve_runner() {
    local dir="$1" name="$2" c
    for c in "$dir/$name.wtperf" \
        "$dir/500m-btree-$name.wtperf" \
        "$dir"/*-"$name".wtperf; do
        if [ -f "$c" ]; then
            printf '%s' "$c"
            return 0
        fi
    done
    return 1
}

# wt_new_home <dir> —— 清空并重建 home。
# ★ 必须 mkdir：wtperf 第一步是 fopen(<home>/CONFIG.wtperf) 把配置抄进去，
#   目录不存在时它报的不是 ENOENT 而是 EPERM（config_opt_log），
#   然后 __wt_abort 直接 core dump —— 看起来像"库崩了"，其实是没建目录。
wt_new_home() {
    rm -rf "$1"
    mkdir -p "$1"
}

# 缩水参数（wt_shrink_opts 填充）。为什么必须改这几项：
#   icount      —— 数据量，500000000 条跑不动
#   run_time    —— 时间型 runner 原值是 7200 秒（2 小时），只能缩；
#                  并且必须 > report_interval（10），否则 wtperf 直接报
#                  "interval value longer than the run-time" 一条都不跑
#   warmup      —— 原值 120 秒，缩 run_time 后预热比跑的时间还长
#   compression —— 原配置是 snappy，但 (a) 机器上没编出 snappy 扩展，
#                  连接时会 __wt_dlopen 失败；(b) 本项目只实现 NONE/RLE。
#                  不覆盖就永远连不上库，跟实现对不对没关系。
#   verbose     —— 这个构建上 verbose=1（默认值）**一个字都不打**，连
#                  "load ops/sec" 都没有；要 ≥2 才输出统计。没有数字的
#                  "ok" 等于没测，所以显式给 2。
#   * _interval —— wtperf 有一条硬校验："任何 interval 都不能长于 run_time"，
#                  否则直接 `interval value longer than the run-time` 一条都不跑。
#                  checkpoint-stress 原配置是 checkpoint_interval=60，光缩 run_time
#                  就会撞上这条。所以几个 interval 一起压到 ≤ run_time。
WT_SHRINK=()
wt_shrink_opts() { # <icount> <run_time> <warmup>
    WT_SHRINK=(-o "icount=$1,run_time=$2,warmup=$3,compression=none,verbose=2"
        -o "report_interval=2,sample_interval=2,checkpoint_interval=5,backup_interval=0")
}

# wt_ops_from_log <log> —— 从 wtperf 日志里算总吞吐（拿不到就什么都不打）
#
# ★ 两件事都是踩出来的：
#   1. 汇总行是"每种操作一行、每行一个速率"，**不能取最后一行** ——
#      最后几行常常是 "Executed 0 scan operations (0%) 0 ops/sec"，
#      纯读的 runner（evict-btree）会被读成 0，而它实际跑了 449 万 ops/sec。
#      正确做法是把各操作类型的速率**求和**（总吞吐）。
#   2. populate 走的是另一种格式 "load ops/sec: N"（数字在冒号后面），
#      连 "ops/sec" 前面的数字都没有，正则得单独写。
wt_ops_from_log() {
    local v
    v=$(awk '
        /^Executed/ && match($0, /[0-9]+ ops\/sec/) {
            tot += substr($0, RSTART, RLENGTH - 8)   # 去掉末尾 " ops/sec" 这 8 个字符
        }
        END { if (tot > 0) printf "%d", tot }
    ' "$1")
    if [ -z "$v" ]; then
        v=$(grep -oE 'load ops/sec: [0-9]+' "$1" | tail -1 | grep -oE '[0-9]+$')
    fi
    printf '%s' "$v"
}

# wt_run_wtperf <wtperf> <cfg> <home> <timeout> <log> <use_our_lib> [额外参数...]
# 在**已经准备好**的 home 上跑一次 wtperf，成功打印 ops/sec，失败打印 "-"。
# 不做 rm/mkdir —— 因为 create=false 的 runner 需要"先 populate 再跑"，
# 两步必须共用同一个 home，第二步不能把它清掉。
# use_our_lib=no 时用空 env（C3 的对照组跑"纯真 WT"）。
wt_run_wtperf() {
    local wtperf="$1" cfg="$2" home="$3" tmo="$4" log="$5" use_lib="${6:-yes}"
    shift 6 || true
    local rc=0 envp=() v

    if [ "$use_lib" = "yes" ]; then
        # 第二道防线：要跑我们的库，却没有 env —— 那就是在跑真 WT，宁可不跑
        if [ -z "$WT_MODE" ] || [ ${#WT_ENV[@]} -eq 0 ]; then
            echo "!! WT_ENV 为空（WT_MODE='${WT_MODE:-}'）：wt_setup_dropin 没在同一 shell 调用" >"$log"
            echo "-"
            return 0
        fi
        envp=("${WT_ENV[@]}")
    fi

    timeout "$tmo" "${envp[@]}" "$wtperf" -O "$cfg" "$@" -h "$home" >"$log" 2>&1 || rc=$?

    if [ $rc -ne 0 ]; then
        if [ $rc -eq 124 ]; then
            echo "!! 超时 ${tmo}s：多半是某个 __wt_* 空桩让主线程在等 worker（不是慢）" >>"$log"
        fi
        echo "-"
        return 0
    fi

    # ★ 汇总行是"每种操作一行、每行一个速率"，**不能取最后一行**：
    #   最后几行常常是 "Executed 0 scan operations (0%) 0 ops/sec"，
    #   于是纯读的 runner（evict-btree）会被读成 0，而它实际跑了 449 万 ops/sec。
    #   正确做法是把各操作类型的速率**求和**（总吞吐）。
    #   populate 没有这些行，它给的是 "load ops/sec: N"（数字在冒号后）。
    v=$(wt_ops_from_log "$log")
    if [ -n "$v" ] && [ "$v" -gt 0 ] 2>/dev/null; then
        printf '%s' "$v"
        return 0
    fi

    # 退出码 0 但没有数字（或全是 0）：这是"跑了但什么都没测到"，不能算 ok。
    echo "!! wtperf 退出码 0 但没有非零 ops/sec（verbose 太低，或 run_time 太短）" >>"$log"
    echo "-"
}

# wt_runner_needs_populate <cfg> —— 这个 runner 是否假定 home 里已有数据？
# 判据就是 wtperf 配置里的 create=false。这类 runner 直接跑只会得到
#   "find_table_count: open_cursor failed: No such file or directory"
# 因为它在找一个还没被创建的表。
wt_runner_needs_populate() {
    grep -qE '^[[:space:]]*create[[:space:]]*=[[:space:]]*false' "$1"
}
