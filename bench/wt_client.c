/*-
 * bench/wt_client.c —— tinywt 的「作业客户端」（框架件，不是作业）
 *
 * 为什么要有它：
 *   make capstone 用真 wtperf 做终局验收，但 wtperf 要等 P3/T3.6 全做完才跑得起来。
 *   在那之前，学生没有任何手段回答三个问题：
 *     1. 我的库和真 WT 行为一致吗？        → wt_client diffdata + diff-wt
 *     2. 我的库现在多快？慢在哪？          → wt_client bench
 *     3. 崩溃之后数据还对吗？              → wt_client crash-write / crash-verify
 *
 * 它只使用 **third_party/wiredtiger/include/wiredtiger.h** 的公开 ABI，
 * 和 wtperf 走同一套方法表。所以同一份二进制可以在两个库上跑：
 *
 *   LD_LIBRARY_PATH=build                      ./wt_client bench ...
 *   LD_LIBRARY_PATH=external/wiredtiger/build  ./wt_client bench ...   # 对照组
 *
 * 两个库的 SONAME 相同（libwiredtiger.so.11.3.0），所以这是**替换**而不是
 * LD_PRELOAD 叠加 —— 跑真库时我们自己的代码一行都不参与。
 *
 * 退出码约定（脚本靠它区分「没实现」和「实现错了」）：
 *   0  成功
 *   1  断言/校验失败（实现有 bug）
 *   2  用法错误
 *   3  库还没准备好（wiredtiger_open 失败 / 命中 UNIMPLEMENTED）
 *
 * 编译：cmake 目标 wt_client（链接 build/libwiredtiger.so）
 * -*/
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wiredtiger.h"

#define EXIT_BAD 1
#define EXIT_USAGE 2
#define EXIT_NOTREADY 3

#define TABLE_URI "table:t"
#define TABLE_CFG "key_format=S,value_format=S"

static const char *g_home = NULL;
static const char *g_open_cfg = "create,cache_size=64M";

/* ---------------------------------------------------------------- 小工具 */

static void
die_usage(const char *msg)
{
    fprintf(stderr, "wt_client: %s\n", msg);
    fprintf(stderr,
      "usage:\n"
      "  wt_client populate     --home H --keys N [--value-size V] [--config C]\n"
      "  wt_client bench        --home H --op read|update|insert|scan --keys N --seconds S\n"
      "  wt_client dump         --home H [--out F]\n"
      "  wt_client diffdata     --home H --keys N --ops M --seed S [--out F]\n"
      "  wt_client crash-write  --home H --keys N --ckpt-every K --ack F\n"
      "  wt_client crash-verify --home H --ack F\n"
      "\n"
      "--config 是传给 wiredtiger_open 的配置串（默认 \"create,cache_size=64M\"）。\n");
    exit(EXIT_USAGE);
}

static const char *
arg_value(int argc, char **argv, const char *name)
{
    for (int i = 0; i + 1 < argc; ++i)
        if (strcmp(argv[i], name) == 0)
            return argv[i + 1];
    return NULL;
}

static long long
arg_ll(int argc, char **argv, const char *name, long long dflt)
{
    const char *v = arg_value(argc, argv, name);
    if (v == NULL || *v == '\0')
        return dflt;
    char *end = NULL;
    long long n = strtoll(v, &end, 10);
    if (end == v || *end != '\0' || n < 0) {
        fprintf(stderr, "wt_client: bad %s=%s\n", name, v);
        exit(EXIT_USAGE);
    }
    return n;
}

static const char *
arg_str(int argc, char **argv, const char *name, const char *dflt)
{
    const char *v = arg_value(argc, argv, name);
    return (v == NULL || *v == '\0') ? dflt : v;
}

static double
now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 固定格式的键：字典序 == 数值序（B+Tree 测试与 diff 都依赖这一点） */
static void
key_of(char *buf, size_t n, long long i)
{
    snprintf(buf, n, "key-%09lld", i);
}

static void
val_of(char *buf, size_t n, long long i, int vlen, unsigned tag)
{
    int k = snprintf(buf, n, "value-%09lld-%08x-", i, tag);
    if (k < 0)
        k = 0;
    for (int j = k; j < vlen - 1 && (size_t)j + 1 < n; ++j)
        buf[j] = (char)('a' + (j % 26));
    if (vlen > 0 && (size_t)vlen <= n)
        buf[vlen - 1] = '\0';
}

static uint64_t
xorshift64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

/* ---------------------------------------------------------------- 连接封装 */

typedef struct {
    WT_CONNECTION *conn;
    WT_SESSION *ses;
    WT_CURSOR *cur;
} db_t;

static int
db_open(db_t *db, const char *what)
{
    int rc;
    memset(db, 0, sizeof(*db));

    if ((rc = wiredtiger_open(g_home, NULL, g_open_cfg, &db->conn)) != 0) {
        /*
         * 这一条要单独说清楚：空桩库在这里返回 WT_ERROR（不是崩溃）。
         * 脚本看到 exit 3 就 SKIP，不把它算成"实现错了"。
         */
        fprintf(stderr, "wt_client: wiredtiger_open(%s) -> %d (%s)\n", g_home, rc,
          wiredtiger_strerror(rc));
        fprintf(stderr, "wt_client: 库还没准备好（P3/T3.1 之前这是正常的）\n");
        return EXIT_NOTREADY;
    }
    if ((rc = db->conn->open_session(db->conn, NULL, NULL, &db->ses)) != 0) {
        fprintf(stderr, "wt_client: open_session -> %d (%s)\n", rc, wiredtiger_strerror(rc));
        return EXIT_NOTREADY;
    }
    (void)what;
    return 0;
}

static int
db_create(db_t *db)
{
    int rc = db->ses->create(db->ses, TABLE_URI, TABLE_CFG);
    if (rc == 0 || rc == EEXIST /* WT 用 EEXIST 表示已存在 */)
        return 0;
    /* 真 WT 对已存在的表返回 EEXIST；有些实现返回 WT_ERROR —— 开游标再判断 */
    return rc;
}

static int
db_cursor(db_t *db)
{
    int rc = db->ses->open_cursor(db->ses, TABLE_URI, NULL, NULL, &db->cur);
    if (rc != 0)
        fprintf(stderr, "wt_client: open_cursor -> %d (%s)\n", rc, wiredtiger_strerror(rc));
    return rc;
}

static void
db_close(db_t *db, int clean)
{
    if (db->cur != NULL)
        (void)db->cur->close(db->cur);
    if (db->ses != NULL)
        (void)db->ses->close(db->ses, NULL);
    if (db->conn != NULL)
        (void)db->conn->close(db->conn, clean ? NULL : "leak_memory=true");
}

static int
put_one(db_t *db, const char *k, const char *v)
{
    int rc;
    db->cur->set_key(db->cur, k);
    db->cur->set_value(db->cur, v);
    rc = db->cur->insert(db->cur);
    if (rc == WT_DUPLICATE_KEY) {
        db->cur->set_key(db->cur, k);
        db->cur->set_value(db->cur, v);
        rc = db->cur->update(db->cur);
    }
    return rc;
}

/* ---------------------------------------------------------------- 数据生成 */

static int
populate(int argc, char **argv)
{
    long long n = arg_ll(argc, argv, "--keys", 100000);
    int vlen = (int)arg_ll(argc, argv, "--value-size", 120);
    char k[64], v[1024];
    db_t db;
    int rc;

    if (vlen <= 0 || vlen >= (int)sizeof(v))
        die_usage("--value-size 必须在 1..1023");
    if ((rc = db_open(&db, "populate")) != 0)
        return rc;
    (void)db_create(&db);
    if ((rc = db_cursor(&db)) != 0) {
        db_close(&db, 1);
        return EXIT_BAD;
    }

    double t0 = now_sec();
    for (long long i = 0; i < n; ++i) {
        long long t = i & 0x7fffffffLL;
        key_of(k, sizeof(k), i);
        val_of(v, sizeof(v), i, vlen, (unsigned)t);
        if ((rc = put_one(&db, k, v)) != 0) {
            fprintf(stderr, "wt_client: insert key=%s -> %d (%s)\n", k, rc, wiredtiger_strerror(rc));
            db_close(&db, 0);
            return EXIT_BAD;
        }
    }
    double dt = now_sec() - t0;

    if ((rc = db.ses->checkpoint(db.ses, NULL)) != 0)
        fprintf(stderr, "wt_client: checkpoint -> %d (%s)\n", rc, wiredtiger_strerror(rc));
    db_close(&db, 1);

    printf("{\"op\":\"populate\",\"keys\":%lld,\"seconds\":%.3f,\"ops_per_sec\":%.0f}\n", n, dt,
      dt > 0 ? (double)n / dt : 0.0);
    return 0;
}

static int
bench(int argc, char **argv)
{
    long long n = arg_ll(argc, argv, "--keys", 100000);
    double seconds = (double)arg_ll(argc, argv, "--seconds", 5);
    const char *op = arg_str(argc, argv, "--op", "read");
    int vlen = (int)arg_ll(argc, argv, "--value-size", 120);
    char k[64], v[1024];
    db_t db;
    int rc;

    if (seconds <= 0)
        die_usage("--seconds 必须 > 0");
    if ((rc = db_open(&db, "bench")) != 0)
        return rc;
    (void)db_create(&db);
    if ((rc = db_cursor(&db)) != 0) {
        db_close(&db, 1);
        return EXIT_BAD;
    }

    uint64_t rng = 0x9e3779b97f4a7c15ULL;
    long long done = 0;
    double t0 = now_sec(), deadline = t0 + seconds;

    if (strcmp(op, "read") == 0 || strcmp(op, "update") == 0) {
        while (now_sec() < deadline) {
            for (int i = 0; i < 256; ++i) {
                long long idx = (long long)(xorshift64(&rng) % (uint64_t)n);
                key_of(k, sizeof(k), idx);
                if (strcmp(op, "read") == 0) {
                    db.cur->set_key(db.cur, k);
                    rc = db.cur->search(db.cur);
                    if (rc == 0) {
                        char *got = NULL;
                        (void)db.cur->get_value(db.cur, &got);
                    } else if (rc != WT_NOTFOUND) {
                        fprintf(stderr, "wt_client: search -> %d (%s)\n", rc, wiredtiger_strerror(rc));
                        db_close(&db, 0);
                        return EXIT_BAD;
                    }
                } else {
                    val_of(v, sizeof(v), idx, vlen, (unsigned)(rng & 0xffffffffu));
                    if ((rc = put_one(&db, k, v)) != 0) {
                        fprintf(stderr, "wt_client: update -> %d (%s)\n", rc, wiredtiger_strerror(rc));
                        db_close(&db, 0);
                        return EXIT_BAD;
                    }
                }
                ++done;
            }
        }
    } else if (strcmp(op, "insert") == 0) {
        for (long long i = n; now_sec() < deadline; ++i) {
            key_of(k, sizeof(k), i);
            val_of(v, sizeof(v), i, vlen, 0);
            if ((rc = put_one(&db, k, v)) != 0) {
                fprintf(stderr, "wt_client: insert -> %d (%s)\n", rc, wiredtiger_strerror(rc));
                db_close(&db, 0);
                return EXIT_BAD;
            }
            ++done;
        }
    } else if (strcmp(op, "scan") == 0) {
        long long seen = 0;
        rc = db.cur->next(db.cur);
        while (rc == 0) {
            ++seen;
            rc = db.cur->next(db.cur);
        }
        if (rc != WT_NOTFOUND) {
            fprintf(stderr, "wt_client: scan -> %d (%s)\n", rc, wiredtiger_strerror(rc));
            db_close(&db, 0);
            return EXIT_BAD;
        }
        done = seen;
    } else {
        die_usage("--op 只支持 read / update / insert / scan");
    }

    double dt = now_sec() - t0;
    db_close(&db, 1);
    printf("{\"op\":\"%s\",\"ops\":%lld,\"seconds\":%.3f,\"ops_per_sec\":%.0f}\n", op, done, dt,
      dt > 0 ? (double)done / dt : 0.0);
    return 0;
}

/* ---------------------------------------------------------------- 规范化 dump */

static int
hex_write(FILE *f, const char *s)
{
    if (s == NULL)
        return fprintf(f, "-");
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p)
        fprintf(f, "%02x", *p);
    return 0;
}

static int
dump(int argc, char **argv)
{
    const char *out = arg_str(argc, argv, "--out", NULL);
    FILE *f = stdout;
    db_t db;
    int rc;

    if ((rc = db_open(&db, "dump")) != 0)
        return rc;
    if ((rc = db_cursor(&db)) != 0) {
        db_close(&db, 1);
        return EXIT_NOTREADY;
    }

    if (out != NULL && (f = fopen(out, "w")) == NULL) {
        fprintf(stderr, "wt_client: fopen(%s): %s\n", out, strerror(errno));
        db_close(&db, 1);
        return EXIT_USAGE;
    }

    fprintf(f, "#tinywt-dump-v1\n");
    rc = db.cur->next(db.cur);
    while (rc == 0) {
        char *k = NULL, *v = NULL;
        if (db.cur->get_key(db.cur, &k) != 0 || db.cur->get_value(db.cur, &v) != 0) {
            fprintf(stderr, "wt_client: get_key/get_value 失败\n");
            db_close(&db, 0);
            if (f != stdout)
                fclose(f);
            return EXIT_BAD;
        }
        hex_write(f, k);
        fputc(' ', f);
        hex_write(f, v);
        fputc('\n', f);
        rc = db.cur->next(db.cur);
    }
    if (rc != WT_NOTFOUND) {
        fprintf(stderr, "wt_client: next -> %d (%s)\n", rc, wiredtiger_strerror(rc));
        db_close(&db, 0);
        if (f != stdout)
            fclose(f);
        return EXIT_BAD;
    }
    if (f != stdout)
        fclose(f);
    db_close(&db, 1);
    return 0;
}

/*
 * diffdata —— 给 diff-wt 用：同一份确定性操作流，两个引擎各跑一遍，
 * 最后 dump 出来的文本必须逐字节相同。
 *
 * 只做 put / remove（不碰事务），因为这是 P2 + T3.2 的最小交集：
 * 哪怕 T3.4/T3.5 还没写，也能先验"同一操作序列产生同一份数据"。
 */
static int
diffdata(int argc, char **argv)
{
    long long n = arg_ll(argc, argv, "--keys", 1000);
    long long ops = arg_ll(argc, argv, "--ops", 5000);
    uint64_t seed = (uint64_t)arg_ll(argc, argv, "--seed", 1);
    char k[64], v[1024];
    db_t db;
    int rc;

    if (n <= 0)
        die_usage("--keys 必须 > 0");
    if ((rc = db_open(&db, "diffdata")) != 0)
        return rc;
    (void)db_create(&db);
    if ((rc = db_cursor(&db)) != 0) {
        db_close(&db, 1);
        return EXIT_NOTREADY;
    }

    uint64_t rng = seed ? seed : 1;
    for (long long i = 0; i < ops; ++i) {
        long long idx = (long long)(xorshift64(&rng) % (uint64_t)n);
        key_of(k, sizeof(k), idx);
        unsigned op = (unsigned)(xorshift64(&rng) % 100);

        if (op < 70) { /* 70% 写 */
            val_of(v, sizeof(v), idx, 64, (unsigned)(rng & 0xffffffffu));
            if ((rc = put_one(&db, k, v)) != 0) {
                fprintf(stderr, "wt_client: put -> %d (%s)\n", rc, wiredtiger_strerror(rc));
                db_close(&db, 0);
                return EXIT_BAD;
            }
        } else if (op < 85) { /* 15% 删 */
            db.cur->set_key(db.cur, k);
            rc = db.cur->remove(db.cur);
            if (rc != 0 && rc != WT_NOTFOUND) {
                fprintf(stderr, "wt_client: remove -> %d (%s)\n", rc, wiredtiger_strerror(rc));
                db_close(&db, 0);
                return EXIT_BAD;
            }
        } else { /* 15% 读：只校验不存在的语义，不比较值（值由 dump 比） */
            db.cur->set_key(db.cur, k);
            rc = db.cur->search(db.cur);
            if (rc != 0 && rc != WT_NOTFOUND) {
                fprintf(stderr, "wt_client: search -> %d (%s)\n", rc, wiredtiger_strerror(rc));
                db_close(&db, 0);
                return EXIT_BAD;
            }
        }
    }
    if ((rc = db.ses->checkpoint(db.ses, NULL)) != 0)
        fprintf(stderr, "wt_client: checkpoint -> %d (%s)\n", rc, wiredtiger_strerror(rc));
    db_close(&db, 1);

    /* 再开一次，从磁盘读回来 dump —— 同时验 checkpoint/重开 */
    return dump(argc, argv);
}

/* ---------------------------------------------------------------- 崩溃测试 */

/*
 * crash-write: 按 key 递增顺序写，每条提交后往 ack 文件打一行 "A <i>"。
 * 父进程用 timeout -s KILL 杀它；ack 文件里留下的是**已经提交**的前缀。
 *
 * 不变式（crash-verify 检查）：
 *   1. 已完成 checkpoint 覆盖到的 key 必须全部存在（落进数据页的东西不能丢）
 *   2. key 集合必须是 [0, max] 的连续前缀（不允许中间空洞）
 *   3. 没 ack 的 key 允许存在（提交了但还没打印就被杀）—— 但不能破坏第 2 条
 *   4. 不允许出现 [0, keys) 之外的键
 *
 * ★ 第 1 条为什么以 checkpoint 为界、而不是以 ack 为界：commit 返回只保证
 *   记录进了日志缓冲区，SIGKILL 会带走进程里还没 write() 的尾部。要求
 *   "acked 全存活"就得每个 commit 都 log_flush(sync=true)，那是 T3.5 的
 *   上层语义，不在本脚本的判据里。
 */
static int
crash_write(int argc, char **argv)
{
    long long n = arg_ll(argc, argv, "--keys", 5000000);
    long long ckpt = arg_ll(argc, argv, "--ckpt-every", 5000);
    const char *ack = arg_str(argc, argv, "--ack", NULL);
    char k[64], v[256];
    FILE *af = NULL;
    db_t db;
    int rc;

    if (ack == NULL)
        die_usage("crash-write 需要 --ack F");
    if ((af = fopen(ack, "w")) == NULL) {
        fprintf(stderr, "wt_client: fopen(%s): %s\n", ack, strerror(errno));
        return EXIT_USAGE;
    }
    setvbuf(af, NULL, _IONBF, 0); /* 关键：被杀时已提交的记录必须在文件里 */

    if ((rc = db_open(&db, "crash-write")) != 0) {
        fclose(af);
        return rc;
    }
    (void)db_create(&db);
    if ((rc = db_cursor(&db)) != 0) {
        db_close(&db, 1);
        fclose(af);
        return EXIT_NOTREADY;
    }

    for (long long i = 0; i < n; ++i) {
        key_of(k, sizeof(k), i);
        val_of(v, sizeof(v), i, 64, 0);

        if ((rc = db.ses->begin_transaction(db.ses, NULL)) != 0) {
            fprintf(stderr, "wt_client: begin_transaction -> %d (%s)\n", rc, wiredtiger_strerror(rc));
            break;
        }
        db.cur->set_key(db.cur, k);
        db.cur->set_value(db.cur, v);
        if ((rc = db.cur->insert(db.cur)) != 0 && rc != WT_DUPLICATE_KEY) {
            fprintf(stderr, "wt_client: insert -> %d (%s)\n", rc, wiredtiger_strerror(rc));
            (void)db.ses->rollback_transaction(db.ses, NULL);
            break;
        }
        if ((rc = db.ses->commit_transaction(db.ses, NULL)) != 0) {
            fprintf(stderr, "wt_client: commit -> %d (%s)\n", rc, wiredtiger_strerror(rc));
            break;
        }

        fprintf(af, "A %lld\n", i);
        if (ckpt > 0 && (i + 1) % ckpt == 0) {
            if (db.ses->checkpoint(db.ses, NULL) == 0)
                fprintf(af, "C %lld\n", i); /* checkpoint 完成 → 这批必须扛得住崩溃 */
        }
    }

    db_close(&db, 0); /* 0 = 不干净收尾（模拟崩溃时的资源状态） */
    fclose(af);
    return EXIT_BAD; /* 正常情况下会被 timeout 杀掉，走到这里说明提前结束 */
}

static int
crash_verify(int argc, char **argv)
{
    const char *ack = arg_str(argc, argv, "--ack", NULL);
    long long n = arg_ll(argc, argv, "--keys", 0);
    db_t db;
    int rc;

    if (ack == NULL)
        die_usage("crash-verify 需要 --ack F");

    FILE *af = fopen(ack, "r");
    if (af == NULL) {
        fprintf(stderr, "wt_client: 没有 ack 文件（子进程没起来？）\n");
        return EXIT_NOTREADY;
    }

    /* acked[i] = 1 表示这条已提交；C 行表示一次成功完成的 checkpoint */
    unsigned char *acked = NULL;
    long long acked_max = -1, acked_n = 0, ckpt_max = -1, ckpt_n = 0;
    if (n > 0 && (acked = calloc((size_t)n, 1)) == NULL)
        die_usage("calloc 失败");
    /*
     * 用 fgets 而不是 fscanf("A %lld")：后者的格式串以字面量 'A' 开头，
     * 不会跳过上一行剩下的换行 —— 第二次调用就匹配失败，结果只读到一条。
     * （这个坑是实测出来的：145000 条已提交，acked 却只报 1。）
     */
    char line[64];
    while (fgets(line, sizeof(line), af) != NULL) {
        long long idx;
        if (sscanf(line, "A %lld", &idx) == 1) {
            if (acked != NULL && idx >= 0 && idx < n)
                acked[idx] = 1;
            if (idx > acked_max)
                acked_max = idx;
            ++acked_n;
        } else if (sscanf(line, "C %lld", &idx) == 1) {
            if (idx > ckpt_max)
                ckpt_max = idx;
            ++ckpt_n;
        }
    }
    fclose(af);
    if (acked == NULL && acked_max >= 0)
        die_usage("crash-verify 需要 --keys N（和 crash-write 一致）");

    if ((rc = db_open(&db, "crash-verify")) != 0) {
        free(acked);
        return rc;
    }
    if ((rc = db_cursor(&db)) != 0) {
        db_close(&db, 1);
        free(acked);
        /*
         * 崩在建表之前：ack 里本来就一条已提交记录都没有，这不算失败。
         * 这是"崩得太早"，不是"恢复错了"。
         */
        if (acked_n == 0) {
            printf("{\"acked\":0,\"acked_max\":-1,\"present\":0,\"max\":-1,"
                   "\"holes\":0,\"missing_acked\":0,\"out_of_range\":0}\n");
            return 0;
        }
        return EXIT_NOTREADY;
    }

    long long seen = 0, prev = -1, holes = 0, missing_acked = 0, out_of_range = 0;
    rc = db.cur->next(db.cur);
    while (rc == 0) {
        char *k = NULL;
        (void)db.cur->get_key(db.cur, &k);
        long long v = -1;
        if (k != NULL && sscanf(k, "key-%lld", &v) == 1) {
            if (v < 0 || (n > 0 && v >= n))
                ++out_of_range;
            if (prev >= 0 && v != prev + 1)
                ++holes;
            prev = v;
        }
        ++seen;
        rc = db.cur->next(db.cur);
    }
    db_close(&db, 1);

    if (rc != WT_NOTFOUND && rc != 0) {
        fprintf(stderr, "wt_client: 扫描失败 -> %d (%s)\n", rc, wiredtiger_strerror(rc));
        free(acked);
        return EXIT_BAD;
    }

    /* 只要求「已 checkpoint 的那一段」一条不少：日志里未落盘的尾部允许丢 */
    if (acked != NULL && ckpt_max >= 0)
        for (long long i = 0; i <= ckpt_max && i < n; ++i)
            if (!acked[i])
                ++missing_acked;

    long long lost = (acked_max > prev) ? acked_max - prev : 0;

    /*
     * 判据（三条）：
     *   1. 不能有空洞：present 必须是 [0, max] 的连续前缀
     *   2. 已完成的 checkpoint 覆盖的前缀必须完整（丢了 = 数据页/元数据坏了）
     *   3. 不允许出现配置范围外的键（越界 = 恢复把垃圾读进来了）
     *
     * ★ 不能要求「acked 全部存活」：commit 返回只保证记录进了日志缓冲区，
     *   SIGKILL 会带走进程内存里还没 write() 的日志尾部 —— 刚才实测就丢了
     *   约 900 条。真正的持久性需要 log_flush(sync=true) 或 write_timestamp，
     *   那是 T3.5 的上层语义，这里不强制。lost 只作为观察值打印。
     */
    printf("{\"acked\":%lld,\"acked_max\":%lld,\"ckpt\":%lld,\"ckpt_max\":%lld,"
           "\"present\":%lld,\"max\":%lld,\"holes\":%lld,\"missing_acked\":%lld,"
           "\"out_of_range\":%lld,\"lost_after_ack\":%lld}\n",
      acked_n, acked_max, ckpt_n, ckpt_max, seen, prev, holes, missing_acked, out_of_range, lost);

    int bad = (holes != 0) || (missing_acked != 0) || (out_of_range != 0) ||
      (ckpt_max >= 0 && prev < ckpt_max);
    free(acked);
    if (bad) {
        fprintf(stderr,
          "wt_client: 崩溃恢复不变式被破坏 "
          "(holes=%lld missing_acked=%lld out_of_range=%lld max=%lld ckpt_max=%lld)\n",
          holes, missing_acked, out_of_range, prev, ckpt_max);
        return EXIT_BAD;
    }
    return 0;
}

/* ---------------------------------------------------------------- main */

int
main(int argc, char **argv)
{
    if (argc < 2)
        die_usage("缺少子命令");

    g_home = arg_str(argc, argv, "--home", NULL);
    g_open_cfg = arg_str(argc, argv, "--config", "create,cache_size=64M");
    if (g_home == NULL)
        die_usage("缺少 --home");

    const char *cmd = argv[1];
    if (strcmp(cmd, "populate") == 0)
        return populate(argc, argv);
    if (strcmp(cmd, "bench") == 0)
        return bench(argc, argv);
    if (strcmp(cmd, "dump") == 0)
        return dump(argc, argv);
    if (strcmp(cmd, "diffdata") == 0)
        return diffdata(argc, argv);
    if (strcmp(cmd, "crash-write") == 0)
        return crash_write(argc, argv);
    if (strcmp(cmd, "crash-verify") == 0)
        return crash_verify(argc, argv);

    die_usage("未知子命令");
    return EXIT_USAGE;
}
