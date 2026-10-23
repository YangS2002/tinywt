# tinywt 作业指南（Guideline）

> **形式**借 CMU 15-445 BusTub 的项目手册：任务分编、明确的规则、禁止做法清单、
> 独立完成条款、详实的 appendices。
> **内容**一条没借 —— 全是 WiredTiger 自己的机制、自己的目录名、自己的源码行号。

**这份文档管「怎么写作业」**：能改什么、不能做什么、怎么算分、崩了怎么办、怎么交。
**它不重复任务书**。「每个 Task 要写哪些函数、机制是什么」在
[`docs/PROJECTS.md`](PROJECTS.md) 和 [`docs/projects/`](projects/) 里。

| 你想要的 | 去哪看 |
|---|---|
| 我要写哪几个函数、在哪一行 | `make todo P=P2/T2.3` |
| 这个函数对应的 WT 机制是什么 | `docs/projects/p2_btree.md` 的 T2.3 节 |
| 能不能改某个文件、能不能用某个写法 | **本文档 §3 / §4** |
| 为什么我改对了却考不过 | 本文档 §8（诊断顺序）+ 附录 E |
| 分数怎么算、会不会"越缺越好看" | 本文档 §11 |
| 崩了 / TSan 报了 / 想知道真 WT 怎么跑 | 本文档附录 A–D |

---

## 1. 开工前必须承认的三条

BusTub 每份 handout 开头都有一个红框叫 *Instructions*，意思是"这三条不读就别动手"。这里是 tinywt 的版本：

1. **只有标了 `UNIMPLEMENTED(...)` 的函数体是你的。** 其余都是给定的骨架。
2. **`include/tinywt/` 是 ABI 契约，不是草稿纸。** 改结构体成员顺序、改函数签名 = 让真
   `wtperf` 在你手上 segfault，而且链接器一声不吭。
3. **`test/` 和 `scripts/grade.py` 是评分标准。** 改它们等于自己给自己打 100 分 ——
   真工具才不会陪你演。

违反任何一条，`make capstone` 一定会当场爆破，不必等别人审查。

---

## 2. 发布与截止

自学没有 TA 催你，但**没有 deadline 的作业没人写完**。给自己定死线：

| 段落 | 建议 deadline | 硬门槛 |
|---|---|---|
| P0 + P1 | 第 3 天 | `make grade` P0/P1 全绿 |
| P2 T2.1–T2.5 | 第 9 天 | `make grade` P2 ≥ 6/13 |
| P2 全部 | 第 10 天 | P2 全绿 |
| P3 T3.0–T3.2 | 第 14 天 | **`make cp1` 过**（不准进下一段） |
| P3 全部 | 第 18 天 | **`make cp2` 过** + `make wt-abi` 缺口 0 |
| P4 全部 | 第 21 天 | `make grade` 49/49 |
| Capstone | 第 22 天+ | `make capstone` 全过 |

**Late policy（自选）**：拖一天，就在 `docs/LEADERBOARD.md` 里多跑一组 perf 记录作为罚金 ——
因为拖延真正损失的不是分数，是把"量 before/after"的习惯丢掉了。

---

## 3. 你能改什么

| 位置 | 能不能动 | 理由 |
|---|---|---|
| `src/**/*.cpp` 里标了 `UNIMPLEMENTED` 的函数体 | 可 **这就是作业** | 替换成你的实现 |
| `src/` 里自己加辅助函数（`static` / 匿名 namespace） | 可 鼓励 | 但写在对应 `.cpp` 里，不要往 `include/` 塞 |
| `include/tinywt/**` | 禁 | ABI 契约；`make probe` 用 `_Static_assert` 钉死了布局 |
| `test/**` | 禁 | 测试 = 评分标准 |
| `scripts/grade.py` | 禁 | 评分器本体 |
| `third_party/wiredtiger/**` | 禁 | 真 WT 的头 —— 它是「ABI 到底长什么样」的裁判。改了它，整个「兼容 11.3.0」的目标就作废 |
| `CMakeLists.txt` | 慎 只在新增 `.cpp` 时动 | 构建的事实来源 |
| `docs/**` | 慎 只允许加你自己的笔记 | 里面的数字被 `make check-docs` 对着源码逐条核，**改数字 = 骗** |
| `docs/LEADERBOARD.md` 末尾 | 可 由 `make capstone` 自动追加 | 不要手改那一段 |

> **关于加成员/加文件**：BusTub 允许你给类加私有成员。tinywt 这里**不行** ——
> `include/tinywt/` 的每个字段都是对着真 WT 结构体排的（`src/include/btmem.h` 那一系），
> 加一个字段，`make probe` 的红就得你啃。真要加状态，放进 `src/` 的一个 file-local
> `static` map，或者放在已有的临时字段里。这是刻意的，不是漏考虑。

---

## 4. 禁止事项（动手之前先读一遍）

BusTub 的 "you must NOT" 清单是整个作业里最有教育意义的部分 —— 每一条都对应一个
"能过测试但学不到东西"的写法。tinywt 的十条：

| # | 不许 | 为什么它能骗过测试却是错的 |
|---|---|---|
| 1 | **针对测例做特判**：`src/` 里出现 `test`/`#ifdef`/具体 key 字符串分支 | 你的库是给真 `wtperf` 用的，`wtperf` 不认你的 key。特判一定在 C1 现形 |
| 2 | **用 `sleep` 掩盖竞态** | P4 的 `evict-btree` 是 16 读线程 + 4 淘汰线程；sleep 出来的正确性是概率正确性，`make tsan` 会亲口告诉你 |
| 3 | **P4 用一把全局锁交差** | T4.1 允许 tree 级 `shared_mutex`（这是本版简化），但 **T4.2 要求 `WT_REF` 状态机 + hazard**。少了 hazard，页淘汰就是 use-after-free，pthread 下崩不崩看运气 |
| 4 | **`memset` 整个 value / 覆盖写代替 append-update** | T3.4 的"回滚只摘自己的版本"要求你能从链里**单独摘掉**一个事务的版本。整条覆盖会让回滚连别人的写一起丢 |
| 5 | **硬编码边界值**（varint 只推到 127、addr cookie 只推到 4KB） | 测例推到 `1ULL << 40`、测例 `overflow page` 专门考超长 value |
| 6 | **错误码中途丢失**（拿到 `Status` 不往上传，直接 `return S_OK`） | C ABI 那头 `wt` 会认为操作成功。`make wt-abi` 不管这个，但 Capstone C2 的 dump 对比会直接不等 |
| 7 | **`while (!ready) {}` 忙等** | Debug 下可能死锁；而且它会把 TSan 的时间窗口搅乱，反而让真 race 更难复现。要用条件变量 / barrier |
| 8 | **给被测行为加随机性**：比如给 split 随机 sleep、"模拟真实调度" | 真实性要从 WT 源码学，随机性只会让 bug 不可复现。测试要的是**确定性** |
| 9 | **抄 WT 的实现但不看它为什么**（详见 §5） | 你明天就忘，面试官问到第二层就当场露馅 |
| 10 | **为了 leaderboard 好看牺牲正确性** | 正确性由 `make grade`（49 项）判，性能只是附录。反过来的写法会两者双输 |

> 第 9 条要单独说一句：**没读懂就抄，过不了你自己那关。**
> 判定标准很简单 —— 每个 `commit` 都要能对着 WT 源码解释「这一行为什么是这么写的」。
> 答不上来就别提交。没人查你，但你骗的是自己。

---

## 5. 学术诚信（自学版）

BusTub 写的是 "You must not look at another student's solution"。这里是自学的，所以对外的约束换成对内的：

- **读 WT 源码是目的，不是作弊。** 每个 Task 的任务书里都给了真 WT 的**文件与行号**。
  先读、再写，commit message 里留下 `ref:` 行。
- **禁止整份引入别人的 tinywt / WT 衍生实现。** 参考可以，照抄不知道自己在抄什么 = 白做。
- **AI/他人代码可以当起点，但门槛是解释力。** 提交前问自己：这个函数里每一行我都能说出为什么？
  说不出就回去读源码 —— 这不是在交作业，这是在给自己攒面试材料。
- **leaderboard 的数字不许手改。** 由 `make capstone` 自动追加。

---

## 6. 开发循环（一次一件事）

```
┌─① make todo P=P2/T2.3 ─────────────┐
│  列出这一节要写的函数 + file:line   │
└──────────────┬──────────────────────┘
               ▼
┌─② 读 docs/projects/p2_btree.md 的 T2.3 ─┐
│  顺着它给的 WT 源码位置去读一遍          │
└──────────────┬───────────────────────────┘
               ▼
┌─③ 写函数体（一次一个函数，别贪多）──┐
└──────────────┬────────────────────────┘
               ▼
┌─④ make p2-test ──────┐── FAIL ──▶ 回到 ②（通常是机制没读透，不是打字错）
│  只跑这个 Project     │
└──────────────┬───────┘
               ▼
┌─⑤ make grade ────────┐
│  看 49 项里多了几个绿 │
└──────────────┬───────┘
               ▼
┌─⑥ git commit -m "P2/T2.3: ..." ─┐
└─────────────────────────────────┘
```

**填满 ≠ 通过。** `make todo` 的输出空了，`make grade` 还红着，这是常态 ——
它说明你的某个基础件（多半是 P0 的 varint 或 P1 的 extent 回收）写错了，而不是
"这以后所有的 Task 都错了"。

---

## 7. commit 纪律

**一个子任务一个 commit，消息带编号**：

```
P2/T2.3: 叶分裂 + 向父页插入中间键

- 分裂点取中间，右半落到新页，中间键上提（internal key 要截断，见 T2.6）
- root 分裂时新建 root，树高 +1
- fix: 之前分裂完忘了给父页打 dirty，checkpoint 时丢掉半个子树

ref: src/btree/bt_split.c:1128
```

三条规矩：

1. **红不留夜。** 跑不过的不要 commit —— 否则明天的 diff 里混淆"你写的"和"你昨天的 bug"。
2. **commit message 第二段写 fix。** 一周后回看，最有用的是那行"fix:"。
3. **换机器前 push。** 这仓库只有你一个人，唯一的 reviewer 是三周后的你自己。

---

## 8. 卡住时的诊断顺序

`make pN-test` 一片红时，按这个顺序排除，**不要一上来就怀疑算法**：

```
1. make todo P=Pn            ← 是不是漏了函数？（最常见）
2. make pN-test | head       ← 第一个红的是哪个 Task？从小编号开始
3. make p(n-1)-test          ← 往前一个 Project 跑一遍，多半在这里
4. make asan                 ← UAF / 越界 / double free，比猜快 10 倍
5. 对照真 WT（附录 B）       ← 同一份数据跑真库，diff 出行为差异
6. gdb                       ← 只有在 1–5 都没结论时才用
```

经验分布：**① 和 ③ 占了八成**。真正的算法错误不到一成，剩下的一成是低级失误 ——
比如 `Status` 传播看漏（禁止事项 #6）、结构体成员忘了初始化、`size`/`offset` 写反。

---

## 9. 格式与静态检查

| 检查 | 命令 | 说明 |
|---|---|---|
| 代码格式 | `make format` | 按根目录 `.clang-format`（Allman / 4 空格 / 100 列）。**提交前跑一次** |
| 文档漂移 | `make check-docs` | 文档里的数字、路径、链接、make 目标，逐条对着仓库核 |
| 地址 sanitizer | `make asan` | 独立 `build-asan/` 目录，不污染 `build/` |
| 线程 sanitizer | `make tsan` | 只在 P4 有意义；T4.1/T4.2 必须过 |

`make format` 需要 `clang-format`。没装时它会**报明确错误并给出安装提示**，不会静默跳过：

```bash
pip install --user clang-format     # 没有 sudo 也能装
```

> 为什么是 error 而不是 warning：一个"看起来成功但实际什么都没做"的 linter，
> 比没有 linter 更危险 —— 你会以为代码已经符合要求了。

---

## 10. 三层验收（对照 BusTub 的 check-tests / Gradescope）

BusTub 有编译期 `_Static_assert`、`make check-tests`、Gradescope 三层。tinywt 是七层，按成本从低到高：

| # | 层 | 命令 | 判什么 | 什么时候跑 |
|---|---|---|---|---|
| 1 | 编译期合同 | `make probe` | 错误码数值、iface 首成员布局（`_Static_assert`） | 动了 `include/` 之后（你不该动） |
| 2 | 链接期合同 | `make probe-link` | 少哪个 ABI 符号，链接器当场报哪个 | 加/删 API 之后 |
| 3 | 单元测试 | `make pN-test` | 子项目正确性，FAIL 就非零退出 | **每写完一个函数** |
| 4 | 评分 | `make grade` | 49 项逐项过 | 每完成一个 Task |
| 5 | 人工门 | `make cp1` / `make cp2` | cp1 = T3.0–T3.2，cp2 = T3.3–T3.5；**没过不准进下一段** | 段落收尾 |
| 6 | 工具面 | `make wt-abi` | 真 `wt` / `wtperf` 要的符号我们还差几个（`nm` 实测，不猜） | P3 之后持续看 |
| 7 | 结业验收 | `make capstone` | C1–C4：真 `wtperf` 跑起来 + 真 `wt` 工具 + 与真 WT 对照 + 榜单 | 全部写完之后 |

**层 5 是人为设置的**，理由：P3 写歪了不会立刻扣分，但会让后面 P4 的所有红色结果
伪装成"看起来像 P4 的问题"。先过门，再往下走。

---

## 11. 评分怎么算（以及它为什么不骗你）

```
make grade      # 输出：每项 PASS/FAIL/TODO + 每段小计 + TOTAL
```

**评分器的三条诚实原则**（写在 `scripts/grade.py` 里，改它会红）：

1. **分母固定。** 分母取 `README.md` / `docs/PROJECTS.md` 里写的「N 个评分项」，
   **不是**"本次实际跑出多少行"。少一个测试二进制，`3/49` 不会变成 `3/30` ——
   否则缺项越多，账面比率反而越好看。
2. **缺项要点名。** 崩了 / 二进制不在 / 超时，该项记 **MISS** 并打印原因，
   而且 `make grade` 以非零退出。MISS ≠ PASS。
3. **逐项打印，不在结尾统一打。** harness 在每个测例前打 `BEGIN`、跑完立刻打 `RESULT`，
   所以中途 segfault 也能定位到是哪一项炸的 —— 不会出现"一崩全红"。

**交接前的最后一关：**

```bash
make grade                          # TOTAL 必须 49/49
grep -rn UNIMPLEMENTED src/         # 必须为空
make cp1 && make cp2 && make capstone
```

### 慎 关于"假通过"（Capstone 专属陷阱）

真工具链是 **`LD_LIBRARY_PATH` + 相同的 SONAME**（`libwiredtiger.so.11.3.0`），
**不是 `LD_PRELOAD`**。原因：`LD_PRELOAD` 只是叠加，工具的 `NEEDED` 依然会去加载真库，
没导出的 `__wt_*` 被真库悄悄补上 —— **门全绿，但跑的是真 WiredTiger**。

验证自己没踩这个坑：

```bash
ldd external/wiredtiger/build/wt | grep wiredtiger     # 必须解析到 build/libwiredtiger.so
make capstone-selftest                                 # 把 SHLIB 指向真库，先验判据全绿
```

`capstone-selftest` 验的是**判据本身**（换真库跑一遍应该全绿），不是验你的实现。
它必须先通过，否则你遇到的是一个"永远 FAIL 的检查"，会一直误以为是自己代码有问题。

---

## 12. 排行榜：性能数字怎么用

[`docs/LEADERBOARD.md`](LEADERBOARD.md) 不进评分，但它是"优化有没有用"的唯一证据。

规矩三条（BusTub 的 leaderboard 也这么要求）：
同一机器同构建同 `icount` 才能横比；每组跑 3 次取**中位数**（不是最好那次）；
表里必须写清 `cache_size` / 线程数 / 压缩器 / git commit。

---

## 附录 A —— 崩了怎么办（三步）

```bash
# 1) 先在 Debug/-O0 下复现（优化会混淆栈）
make p2-test

# 2) 拿 sanitizer 的栈 —— 十次里有八次它直接指到行
make asan

# 3) 还不行就 gdb
gdb --args build/bin/p2_btree_test
(gdb) run
(gdb) bt 12
(gdb) frame 3; p *page; p ref->state
```

想要 core：

```bash
ulimit -c unlimited
```

---

## 附录 B —— 用真 WT 做"行为对照"

tinywt 自带一整套真 WT 源码（`external/wiredtiger/`）和编出来的真 `wt` / `wtperf`。
**调试的最高手段是：同一份数据，两个引擎跑一遍，diff 结果。**

```bash
# 真库写一个库
external/wiredtiger/build/wt -h /tmp/wt-ref create -c "key_format=S" table:t
# ... 插同样的数据 ...
external/wiredtiger/build/wt -h /tmp/wt-ref dump table:t > /tmp/ref.txt

# 我们的库写同样的库
LD_LIBRARY_PATH=/tmp/abi_test external/wiredtiger/build/wt -h /tmp/wt-mine create ...
external/wiredtiger/build/wt -h /tmp/wt-mine dump table:t > /tmp/mine.txt

diff /tmp/ref.txt /tmp/mine.txt
```

**dump 的文本契约**（`util_load.c:253-280`，C2 的断言照着它写）：
首行 `WiredTiger Dump (...)` → `Format=print`/`hex` → `Header` → uri/config 对 → `Data`
→ 每个 key 一行、value 一行。`-x` 是**连续** hex（`k1\0` → `6b3100`），不是 `6b 31 00`。

---

## 附录 C —— 按段落推荐的 gdb 断点

| 段落 | 断点 | 看什么 |
|---|---|---|
| P1 | `BlockManager::write` / `read` | addr cookie 解出来对不对、offset 是不是 0（0 是描述块） |
| P2 | `reconcile_page` | 三个来源（`entries` / `upd` 链 / `ins_head`）有没有都归并进去 |
| P2 | `split_leaf` | 父页有没有被打 dirty |
| P3 | `Btree::put` / `get` 的 C ABI wrapper | 变参拆包后的 key/value 是不是你以为的那个 |
| P3 | `WiredTigerImpl::search_near` | 三边界（exact / 前驱 / 后继） |
| P4 | `evict_one` | 要淘汰的页有没有先查 hazard 登记点 |
| P4 | `ts rollback_to_stable` | 回滚是否只摘目标事务的版本 |

---

## 附录 D —— TSan 报告怎么读

```bash
make tsan            # ctest --output-on-failure
```

报告里只看三件事：

1. **`WARNING: ThreadSanitizer: data race`** 下面的两个 stack trace —— 一个写、一个读/写。
   两个都不是你说的"出错的那个函数"，而是竞争的两个访问点。
2. **`Location is heap block of size N`** —— 被竞争的对象是什么。如果是 `Page` / `WT_REF`，
   说明 hazard 没兜住（禁止事项 #3）。
3. **`Mutex M N created at`** —— 它告诉你竞争的锁是谁创建的。如果指向 `Btree::latch()`，
   说明你在**假定持锁的上下文**里访问了共享数据，但实际进来的路径没拿那把锁
   （典型是把 T4.1 那条"公开入口加锁、内部函数假定已持锁"写反了 —— 见
   `docs/projects/p4_concurrency_mvcc.md` 的 T4.1 节）。

**别用 `while (!ready) {}` 让 TSan 闭嘴。** 那只是把竞争窗口挪了位置。

---

## 附录 E —— 四类典型段错误（按出现频率）

| 现象 | 最可能的原因 | 怎么验 |
|---|---|---|
| 真工具一跑就在 `wiredtiger_open` 后崩 | **方法表里有空指针** —— `WT_CONNECTION` / `WT_SESSION` / `WT_CURSOR` 三个结构的成员就是函数指针，是**结构体**不是符号，**链接器查不出漏填** | gdb 里 `p *session` / `p *cursor`，逐个方法指针看是否为 NULL |
| `cursor->next()` 隔几步就炸 | 读路径持有了一个**已被 reconcile/rollback 摘掉**的 `Update*` | 打印 `upd->txnid` 看是不是已经 commit 后被清了 |
| 插入随机数据崩，`#include` 顺序一变就不崩 | **跳表层级越界** —— `WT_INSERT_HEAD` 的 skiplist 数组大小随插入增长，没扩容就写越界 | `make asan` 直接指到 `ins_head[...]` |
| `page_in` 返回 OK 但读出来是垃圾 | **addr cookie 解错** —— offset/size/checksum 三个字段错位 | 打印 `(uint64_t)cookie`、`size`、`checksum` 三项对不对得上 |

> 第 1 类最贵：它不报 `undefined symbol`，只报 `Segmentation fault`。
> 这也是为什么 `include/tinywt/` 不许改 —— 方法表的**位置**就是结构体布局。

---

## 附录 F —— FAQ

**Q：为什么 `make grade` 一开始就有 3 项绿的？**
A：那 3 项是**契约自检**（错误码数值、`strerror`），不调用任何 tinywt 代码，只比对常量
是否等于真 `wiredtiger.h`（`WT_NOTFOUND == -31803` 之类）。它绿说明骨架的头没写错；
你改坏布局它就红 —— 这是防 ABI 漂移的哨兵，不是"我事先实现了什么"。

**Q：能不能跳过 P2 直接写 P3？**
A：可以编译，走不远。P3 的每个 cursor 操作最后都落到 P2 的 `descend` / `split` /
`reconcile`。而且 `make cp1` 会把你挡回去。

**Q：`make todo` 空了但 `make grade` 还是红，怎么办？**
A：九成是前一个 Project 的某个函数写错了。按 §8 的顺序，重点看 P0 的 varint 和
P1 的 extent 回收 —— 这两个错了，症状会伪装成 P2/P3 的问题。

**Q：P3 有 106 个函数，占了一半以上，是最难的吗？**
A：**不是。** P3 面广但每个函数短（多数是 ABI 转接 + 变参拆包）。真正的坎是
**P2 的 41 个**（reconcile / split / eviction）和 **P4 的 18 个**（并发）。

**Q：为什么 `wiredtiger_version` 要报 11.3.0 而不是"tinywt 0.1"？**
A：工具拿它跟自己编译时的头比对，报别的值会被当成"装错了库"。版本号和你实现得完不完整
**无关** —— 它是接口契约，不是自夸。

**Q：wtperf 报 `interval value longer than the run-time`？**
A：`*_interval` 超过了缩水后的 `run_time`。`scripts/lib_wt.sh` 的 `wt_shrink_opts`
会连 `*_interval` 一起缩；如果你手写 wtperf config，就得自己把 interval 调到
`run_time` 以内（`verbose` 记着设 2，否则这个 build 不打印任何东西）。

**Q：`create=false` 的 runner（50r50u / 80r20u / rdonly）报找不到数据？**
A：它们需要先跑一次 `populate` 进同一个 home。`scripts/lib_wt.sh` 的
`wt_runner_needs_populate` 就是判这个的。

**Q：单线程对、多线程错，但有时又对？**
A：典型 x86 上几百次才炸一次。直接 `make tsan`，比插 printf 有效得多
（printf 会改变时序，常常"加了 printf 就不崩"）。

---

## 附录 G —— checklist（复制去用）

### 每个 Task 开工前

- [ ] `make todo P=Pn/Tn.m` 看完整清单
- [ ] 读了 `docs/projects/pN_*.md` 里这一节的机制讲解
- [ ] 至少顺着它给的 WT 源码位置看了一遍真实现
- [ ] `git status` 干净，`git log -1` 是上一个 Task 的 commit

### 每个 Task 提交前

- [ ] `make pN-test` 全绿
- [ ] `make grade` 这一段的数字涨了
- [ ] `make format` 跑过
- [ ] 没有违反 §4 的十条
- [ ] commit message 带编号 + `ref:` 行 + `fix:` 行

### 进下一 Project 前

- [ ] `make cp1` / `make cp2` 过了（P3 的两个门）
- [ ] `make wt-abi` 缺口为 0
- [ ] `make check-docs` 全过（文档没漂）
- [ ] `make tsan` 过了（P4 之后）
- [ ] `docs/LEADERBOARD.md` 更新了这一段的数字

### 交卷（Capstone）

- [ ] `make grade` **49/49**
- [ ] `grep -rn UNIMPLEMENTED src/` 为空
- [ ] `ldd external/wiredtiger/build/wt \| grep wiredtiger` 解析到 `build/libwiredtiger.so`
- [ ] `make capstone-selftest` 全绿（先验判据）
- [ ] `make capstone` C1–C4 全过
