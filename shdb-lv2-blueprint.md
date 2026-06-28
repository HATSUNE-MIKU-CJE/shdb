# shdb Lv2 独立评测 & 子阶段分解

## 一、Lv2 定位

摘自蓝图。Lv2 目标：**崩溃也不丢数据。** 在 Lv1 "能存能查"基础上，补上原子写入、软删除、空闲回收、数据校验四块基石，让 shdb 从玩具变成可用的存储引擎。

---

## 二、Lv2 前置知识检查

| 需要 | 状态 | 来源 |
|------|:--:|------|
| open/read/write/lseek | ✅ | fdb + shdb Lv1 |
| rename(2) 原子替换 | ✅ | fdb |
| fsync(2) 强制刷盘 | ✅ | fdb |
| flock 文件锁 | ✅ | fdb / logd |
| tombstone 标记删除 | ✅ | fdb |
| 空闲链表（数组/链表管理回收槽位） | ✅ | fdb |
| CRC32 校验算法 | 🆕 | 简单算法，现学 |
| errno 全覆盖 | 🆕 | 分散在各项目的零星经验，Lv2 系统化 |

**结论**：7 项已会，2 项新知识（CRC32 + 系统化错误处理）。CRC32 是查表算法，30 行代码；errno 是体力活。**立即可开工。**

---

## 三、Lv2 功能边界

### 新增功能

| 功能 | 命令 | 说明 |
|------|------|------|
| 原子写入 | 内部透明 | write(.tmp) → fsync → rename，崩溃安全 |
| 软删除 | `shdb delete --id N` | 标记 tombstone，不物理删除 |
| 空闲链表 | 内部透明 | 删除后槽位回收复用，insert 时优先填空洞 |
| CRC32 校验 | 内部透明 | 写入时计算存尾，读取时重新比对 |
| 错误处理 | 所有命令 | ENOSPC / EACCES / ENOENT / EINTR 全覆盖 |

### 模块变化

| Lv1 文件 | Lv2 变化 |
|------|------|
| `shdb.h` | 新增 SHDB_TMP_PATH、delete_args、cmd_delete 声明、文件锁相关宏 |
| `save.c` | 原子写入重写 + 空闲链表管理 + CRC32 计算 |
| `read.c` | 跳过 tombstone + CRC32 校验 + 输出中标记损坏记录 |
| `parser.c` | 新增 `shdb delete` 命令分发 + 解析 |
| `main.c` | 不变 |
| `swap.c` | 不变 |
| 新增 `crc32.c` | CRC32 查表法实现 |
| 新增 `note.md` | 设计决策记录 |

### 仍在 Lv3+

| 不做 | 原因 |
|------|------|
| .idx 时间戳索引 | Lv3 |
| 批量缓冲写入 | Lv3 |
| compact 压缩 | Lv3 |
| 守护进程 / FIFO | Lv4 |
| 聚合统计 | Lv5 |

---

## 四、Lv2 风险清单

| 风险 | 等级 | 对策 |
|------|:--:|------|
| rename 跨文件系统失败（/tmp 和项目目录不在同一分区） | 🔴 高 | .tmp 文件写在同一目录下，不跨分区 |
| 空闲链表和 tombstone 状态不一致 | 🔴 高 | 删除时原子操作：写 tombstone + 更新 free_list 在同一次写入事务内 |
| CRC32 性能拖慢写入 | 🟡 中 | CRC32 查表法很快（纳秒级），不会成为瓶颈。如果慢了改用 -O2 |
| 模块拆分后编译依赖混乱 | 🟡 中 | shdb.h 单一接口层不变；新增 .c 文件只引 shdb.h |
| 空闲链表持久化位置 | 🟡 中 | 放文件头扩展区域（如 64B 的元数据区），或内存维护、启动时扫描重建 |

---

## 五、子阶段分解

### 子阶段 1：原子写入 — 崩溃安全（~80 行）

**目标**：insert 写入不再直接追加到 .shdb，而是先写 .tmp → fsync → rename。

**涉及文件**：`save.c`、`shdb.h`

**改动点**：

| 函数 | 改动 |
|------|------|
| `dp_create(path)` | 创建文件时同样走 .tmp → rename。参数不变，内部路径加 .tmp 后缀 |
| `cmd_insert(argc, argv)` | 不再 `lseek+write` 直接写 data.shdb。改为：打开 data.shdb 读全部记录到内存 → 追加新记录 → 写 data.shdb.tmp → fsync → rename 替换原文件 |

**为什么 insert 也要全读再写**：原子写入的含义是"新文件要么全部出现，要么完全不出现"。直接 lseek+write 追加到原文件，崩溃时可能出现半条记录。改为：原数据 + 新记录 → 写 .tmp → rename。rename 是原子操作（同一个文件系统内），不会出现半成品。

**自检方式**：
```bash
./shdb insert --class person --conf 0.95 --bbox 120,80,200,300
ls -la data.shdb data.shdb.tmp   # .tmp 不应残留，只有 .shdb
hexdump -C data.shdb | head -3   # 格式正确
```

**关键约束**：
- .tmp 文件和 .shdb 在同一目录，保证 rename 原子性
- fsync 必须在 rename 之前调用（先刷盘再替换）
- rename 成功后，.tmp 不残留

**交付物**：`save.c`（重写写入路径）+ `shdb.h`（新增 TMP_PATH 宏）

---

### 子阶段 2：tombstone + 软删除（~120 行）

**目标**：新增 `shdb delete --id N` 命令，不物理删除记录，只置 flags bit0 = 1。

**涉及文件**：`parser.c`、`save.c`、`read.c`、`shdb.h`

**改动点**：

| 文件 | 新增/改动 | 说明 |
|------|------|------|
| `shdb.h` | `struct delete_args` | 只有一个字段 `int id` |
| `shdb.h` | `#define TOMBSTONE_FLAG 0x01` | 墓碑标记位掩码 |
| `shdb.h` | `int cmd_delete(int argc, char *argv[]);` | 新命令声明 |
| `parser.c` | dispatch 表加 `{"delete", cmd_delete}` | 路由 |
| `parser.c` | `parse_delete_args()` | 解析 `--id N` |
| `save.c` | `cmd_delete(argc, argv)` | lseek 到 `16 + id*36` → 读 record → 置 flags bit0 → 写回 |
| `read.c` | `cmd_query()` 改 | 遍历时跳过 `flags & TOMBSTONE_FLAG` 的记录 |
| `read.c` | `cmd_info()` 改 | 统计总记录和有效记录（未标记 tombstone 的） |

**cmd_delete 核心逻辑**：
1. 解析 `--id N`
2. `lseek(fd, 16 + N*36, SEEK_SET)`
3. `read(fd, &rec, 36)`
4. `rec.flags |= TOMBSTONE_FLAG`
5. lseek 回原位置，`write(fd, &rec, 36)`
6. 将 N 加入空闲链表

**自检方式**：
```bash
./shdb insert --class person --conf 0.95 --bbox 120,80,200,300  # id=0
./shdb insert --class car    --conf 0.72 --bbox 50,30,180,250   # id=1
./shdb delete --id 0
./shdb query --from "2026-06-20 00:00:00" --to "2026-06-22 23:00:00"
# 期望：只有 id=1 的记录
./shdb info
# 期望：total_records=2, alive_records=1
```

**交付物**：`save.c`（新增 cmd_delete）+ `parser.c`（delete 解析）+ `read.c`（跳过 tombstone）+ `shdb.h`（新增声明/宏）

---

### 子阶段 3：空闲链表 — 空间回收（~100 行）

**目标**：删除记录后，槽位回收到空闲链表。下次 insert 时优先复用空闲槽位，而不是一直 append。

**涉及文件**：`save.c`、`shdb.h`、`read.c`

**空闲链表设计**：

由于 Lv1 的记录数通常不大（测试阶段几千条算多了），空闲链表用**数组**而非侵入式链表，简单可控。

```
文件空闲链表（存在内存，程序退出后重启扫描重建）：
int free_slots[MAX_RECORDS];
int free_count;
```

启动时扫描全部记录，收集所有 `flags & TOMBSTONE` 的槽位 ID，填入 `free_slots[]`。

**改动点**：

| 位置 | 改动 |
|------|------|
| `save.c` | 启动时（首次 open 时）全表扫描收集空闲槽位 |
| `save.c - cmd_insert` | 写之前检查 `free_count > 0`，有就取一个空闲槽位号，写回那个位置；没有才 append |
| `save.c - cmd_delete` | 删除时把 id 加入 `free_slots[]` |
| `read.c - cmd_info` | 显示 `free_slots: N` |

**insert 写入逻辑变化**：
```
if (free_count > 0)
    取 free_slots[--free_count] 作为 id
    lseek 到 16 + id*36，覆盖写入
else
    lseek 到文件末尾，追加写入
```

**注意**：复用槽位前需要走原子写入流程（Lv2.1），不能直接覆盖——因为覆盖写入也面临崩溃风险。但为了简化，Lv2 可以只在 append 时用原子写入，复用槽位时直接覆盖（因为原位置已经有一条有效数据占位，覆盖写不会改变文件大小）。

**自检方式**：
```bash
./shdb insert --class person --conf 0.95 --bbox 10,10,10,10  # id=0
./shdb insert --class car    --conf 0.72 --bbox 20,20,20,20  # id=1
./shdb insert --class dog    --conf 0.88 --bbox 30,30,30,30  # id=2
./shdb delete --id 1
./shdb insert --class person --conf 0.99 --bbox 99,99,99,99  # 应复用 id=1 的槽位
./shdb query --from "2026-06-20 00:00:00" --to "2026-06-22 23:00:00"
# id=1 应该是新数据（person, 0.99），不是旧 car 数据
./shdb info
# total_records=3, alive_records=3
```

**交付物**：`save.c`（空闲链表管理）+ `read.c`（info 显示）

---

### 子阶段 4：CRC32 校验（~80 行）

**目标**：写入时计算记录前 32 字节的 CRC32 校验和存入末尾，读取时重新计算比对，检测数据损坏。

**涉及文件**：`crc32.c`（新增）、`shdb.h`、`save.c`、`read.c`

**CRC32 算法**：查表法，IEEE 802.3 多项式（0xEDB88320）。约 30 行代码，无外部依赖。

**改动点**：

| 文件 | 改动 |
|------|------|
| `crc32.c` | `uint32_t crc32(const uint8_t *data, size_t len)` |
| `shdb.h` | `uint32_t crc32(const uint8_t *data, size_t len);` 声明 |
| `save.c - cmd_insert` | 构造 record 后，`crc32((uint8_t*)&rec, 32)` 填 `rec.crc32` |
| `read.c - cmd_query` | 读取 record 后，重新算 CRC32，与 `rec.crc32` 比对；不匹配则标记 "corrupted"，跳过或警告 |

**CRC 不匹配时的处理**（Lv2 策略）：
- 输出警告：`fprintf(stderr, "warning: record %d CRC mismatch, skipping\n", i);`
- 不输出该条数据，继续读下一条

**自检方式**：
```bash
./shdb insert --class person --conf 0.95 --bbox 120,80,200,300
# 用 hexdump 手动改 data.shdb 的一个字节
# 再次 query，应报 CRC mismatch 并跳过该记录
```

**交付物**：`crc32.c` + `save.c`（CRC 计算）+ `read.c`（CRC 校验）+ `shdb.h`（声明）

---

### 子阶段 5：错误处理 + note.md（~120 行）

**目标**：所有 syscall 返回值检查覆盖 ENOSPC / EACCES / ENOENT / EINTR 四大类错误；重建模块文件结构；写设计决策笔记。

**错误处理检查清单**：

| 文件 | syscall | 需检查 |
|------|------|------|
| save.c | `open()` | ENOENT（不存在→创建）、EACCES（权限不足→报错退出） |
| save.c | `write()` | ENOSPC（磁盘满→报错退出）、EINTR（信号中断→重试） |
| save.c | `fsync()` | EIO（IO 错误→报错退出） |
| save.c | `rename()` | EXDEV（跨设备→报错退出） |
| save.c | `lseek()` | 返回值 == -1 → 报错 |
| read.c | `open()` | ENOENT（文件不存在→报错） |
| read.c | `read()` | EINTR（信号中断→重试） |
| parser.c | `strtof()` / `strtoul()` | errno = ERANGE（溢出→报错） |

**模块文件变化**：

| Lv1 | Lv2 | 原因 |
|------|------|------|
| `save.c` | `storage.c` | 职责扩大（增删改+原子写+空闲链表），名字应反映"存储层"而非仅"保存" |
| `read.c` | `query.c` | 职责专注：查询（query + info） |
| `parser.c` | 不变 | |
| `swap.c` | 不变 | |
| `main.c` | 不变 | |
| 无 | `crc32.c` | |
| 无 | `note.md` | |

**note.md 内容大纲**：
1. 为什么选择 tombstone 而非物理删除？（数据恢复 + append-only 简化）
2. 为什么空闲链表用数组而非侵入式链表？（Lv2 数据量小，数组简单；Lv3 切 list_head）
3. rename 原子写入为什么要求 .tmp 和 .shdb 在同一目录？
4. CRC32 定位——检测数据损坏，不是安全用途（不防篡改，只防位翻转）
5. shdb vs fdb 设计对比（时间范围扫描 vs 键值精确查找）

**自检方式**：
- `make clean && make` 零 warning
- 模拟磁盘满：`dd if=/dev/zero of=fake bs=1M count=1; sudo mount fake /mnt/test; ...`
- 模拟权限不足：`chmod 000 data.shdb; ./shdb insert ...`

**交付物**：各 .c 文件重命名 + 错误处理补全 + `note.md`

---

## 六、Lv2 子阶段总览

| 子阶段 | 目标 | 涉及文件 | 行数 | 可测 |
|:--:|------|------|:--:|:--:|
| Lv2.1 | 原子写入 | save.c, shdb.h | ~80 | rename 后 .tmp 无残留 |
| Lv2.2 | tombstone + 软删除 | parser.c, save.c, read.c, shdb.h | ~120 | delete → query 跳过 |
| Lv2.3 | 空闲链表 | save.c, shdb.h, read.c | ~100 | delete 后 insert 复用槽位 |
| Lv2.4 | CRC32 校验 | crc32.c, save.c, read.c, shdb.h | ~80 | 人工损坏记录 → 报警 |
| Lv2.5 | 错误处理 + 模块重组 + note.md | 全文件 | ~120 | make 零 warning |

**Lv2 总计**：~500 行新增/修改，5 个子阶段，项目总行数从 ~550 增长到 ~1050。

---

## 七、Lv2 完成标准

- [ ] `make` 零 warning（`-Wall -Wextra`）
- [ ] insert 走原子写入路径（.tmp → fsync → rename），崩溃安全
- [ ] `shdb delete --id N` 正确标记 tombstone
- [ ] query 跳过 tombstone 记录
- [ ] info 显示 total / alive / free_slots 三类计数
- [ ] insert 优先复用空闲槽位
- [ ] CRC32 写入计算 + 读取校验，损坏记录报警
- [ ] 所有 syscall 有 errno 处理和 perror 输出
- [ ] 模块文件重命名完成（save.c → storage.c, read.c → query.c）
- [ ] note.md 记录了至少 4 条设计决策
