# P0 任务书 —— 基础件热身

> 对标 BusTub P0（C++ Primer）。BusTub 用 Trie 热身，tinywt 用编码件热身。
> 总纲与评分见 [PROJECTS.md](../PROJECTS.md)。验收：3 个测试项。

| 子项目 | 内容 | 函数 | 评分项 |
|---|---|---|---|
| T0.1 | 构建骨架 + 错误码契约（**已给**） | 0 | 1 |
| T0.2 | varint(LEB128) + crc32 | 5 | 2 |

## 前置

无。但请先把 `include/tinywt/common/defs.h` 从头读到尾 —— 后面三个 Project 的所有接口
都建在它上面：`Status`、`Slice`、`Addr`、`Key/Val`、`pack_uint`、`crc32`。

```bash
make p0-test
```

## T0.1 构建骨架（1 项）

**不需要写代码**，但有两件事必须理解：

1. `Makefile` 用 `-MMD -MP` + `-include *.d`。少了它，改一个头文件不会触发重编，
   你会看到「明明改了却还是旧行为」的鬼故事。
2. `include/tinywt/common/defs.h` 里的 `Status` **数值必须与真 WT 一致**：

   | 常量 | 值 | 真 WT |
   |---|---|---|
   | `S_ROLLBACK` | -31800 | `WT_ROLLBACK` |
   | `S_DUPLICATE` | -31801 | `WT_DUPLICATE_KEY` |
   | `S_ERROR` | -31802 | `WT_ERROR` |
   | `S_NOTFOUND` | -31803 | `WT_NOTFOUND` |
   | `S_PANIC` | -31804 | `WT_PANIC` |
   | `S_RESTART` | -31805 | `WT_RESTART` |

   `S_NOTSUP / S_IO / S_CORRUPT` 是 tinywt 自己的扩展，不属于契约。

   `test/contract/abi_probe.c` 用 `_Static_assert` 钉死了这组数字。改错一位，
   `make probe` 立刻编译失败 —— 这比运行时调试便宜得多。

## T0.2 varint + crc32（2 项）

### varint（LEB128）

```
   0..127        → 1 字节        0xxxxxxx
   128..16383    → 2 字节        1xxxxxxx 0xxxxxxx
   ...

   300 = 0b1_0010_1100
       → 低 7 位 0101100 | 续位 = 0xAC
       → 高 2 位 0000010 | 停止 = 0x02
```

四个函数：

```cpp
size_t pack_uint(u8 *dst, u64 v);                        // 返回写入字节数
size_t uint_size(u64 v);                                 // 只算长度，不写
size_t unpack_uint(const u8 *src, size_t avail, u64 &out); // 返回消耗字节，失败 0
void   append_uint(std::vector<u8> &buf, u64 v);
```

三个坑：

1. **`unpack_uint` 必须检查 `avail`**。截断的输入要返回 0（失败），
   不能越界读 —— 磁盘上的数据可能是坏的。
2. **越界编码要拒绝**：第 10 个字节还带续位，说明这个数超出 `u64`，返回 0。
3. **`uint_size` 与 `pack_uint` 必须严格一致**：`uint_size(v) == pack_uint(buf, v)`
   对 `0, 1, 127, 128, 2^64-1` 全部成立。对不上会让 cell 长度算错，
   进而整页镜像解析错位 —— 这是最贵的一类 bug。

为什么用 LEB128 而不是固定 4 字节：cell 头里 key/value 长度各占一次，
小整数（短 key、短 value）占 1 字节而不是 4，页内密度直接提升 20%+。
代价是解码要循环。

### crc32（IEEE，反射多项式 0xEDB88320）

```
"123456789" → 0xCBF43926     ← 标准检查值，先把这个跑对
```

真 WT 用的是 **CRC32C**（Castagnoli，硬件指令加速），这里用 IEEE 版本，
语义等价（只要求「同数据同值」+「单比特翻转必被发现」）。
两者的多项式不同，所以同一个输入的输出**不一样** —— 不要拿真 WT 的
checksum 来对拍。

`crc32(nullptr, 0)` 必须返回 0，不能崩。

## 评分测试

| 测试 | 覆盖 |
|---|---|
| `test/p0/defs_test.cpp` | 错误码契约、strerror、varint 往返与边界、crc32 标准值 + 单比特翻转 |

## 复习题

1. varint 为什么选 LEB128 而不是固定 4B？在 cell 头里的代价模型是什么？
2. CRC 为什么放在 `BlockHeader` 而不是页尾？
3. 为什么 `unpack_uint` 失败返回 0 而不是 -1？（提示：返回类型是 `size_t`）
