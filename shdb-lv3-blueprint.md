# shdb Lv3 独立评测 & 子阶段分解

## 一、Lv3 定位

摘自蓝图。Lv3 目标：**查得快。** Lv2 已经是可靠的存储引擎——数据不丢、删除不毁、损坏可检。但 10 万条记录时线性扫描要几十秒，不可接受。Lv3 补上索引层，把查询从 O(N) 降到 O(log N)。

**一句话**：给每条记录的时间戳建一个独立索引文件，mmap 映射到内存，二分查找定位起始 offset，批量缓冲写入减少 fsync。

---

## 二、Lv3 前置知识检查

| 需要 | 状态 | 来源 |
|------|:--:|------|
| open/read/write/lseek | ✅ | Lv1-2 |
| 定长记录 + offset 计算 | ✅ | Lv1-2 |
| 原子写入 + fsync | ✅ | Lv2 |
| struct packed 字节布局 | ✅ | Lv1 |
| 大小端 swap | ✅ | Lv1 |
| mmap / munmap | 🆕 | fdb 用过浅层 mmap，Lv3 深化：MAP_SHARED + msync |
| 二分查找（bsearch / 手写） | 🆕 | 基础算法，20 行代码 |
| 批量缓冲（ringbuf / array） | 🆕 | fdb 有类似概念（缓冲区满 flush），Lv3 系统化 |
| compact 压缩 | 🆕 | 等于 Lv2 的原子写入 + 跳过 tombstone，不新 |
| 索引一致性修复 | 🆕 | 启动时 .dat 和 .idx 对照差异，设计问题而非算法问题 |

**结论**：5 项已会，5 项新知识。新知识中 mmap 是核心（调用简单但理解页缓存和 msync 需要功夫），bsearch 和 compact 是体力活。**mmap 是 Lv3 的门票。**

---

## 三、Lv3 功能边界

### 新增功能

| 功能 | 命令/触发 | 说明 |
|------|------|------|
| .idx 时间戳索引 | 内部自动 | `[timestamp(8B), offset(4B)]` 对，按时间升序，append-only |
| 二分查找查询 | `shdb query` | mmap .idx → bsearch 定位 → lseek 直接从起始 offset 读，跳过无关记录 |
| 批量缓冲写入 | `shdb insert` | 内存 buffer[64] 攒满 64 条或超时 500ms 才 flush 写盘 |
| compact 压缩 | `shdb compact` | 遍历 .dat → 跳过 tombstone → 写入新 .tmp → rename 替换 |
| 索引自动修复 | 启动时 | .idx 缺失→全盘扫描重建；.idx 比 .dat 少→补索引条目 |
| note.md 更新 | — | shdb vs fdb 设计对比 + 索引设计决策 |

### 磁盘格式变化

新增 `.idx` 文件，与 `.dat` 并存：

```
data.shdb       — 数据文件（不变）
data.shdb.idx   — 索引文件

.idx 文件格式：
┌─────────────────────────────────┐
│ index_entry[0]  (12B)           │
│ timestamp(8B) | offset(4B)     │
├─────────────────────────────────┤
│ index_entry[1]                  │
│ ...                             │
└─────────────────────────────────┘

每条 entry：uint64_t timestamp + uint32_t record_offset
record_offset = 从 .dat 文件开头的绝对偏移量
按 timestamp 升序排列，append-only
.idx 文件大小 = N × 12 字节
```

### 仍在 Lv4+

| 不做 | 原因 |
|------|------|
| 守护进程 / FIFO | Lv4 |
| 聚合统计（min/max/avg/count/hour） | Lv5 |
| mutex 并发保护 | Lv4 |
| SIGHUP 运行中重建索引 | Lv4 |

---

## 四、Lv3 风险清单

| 风险 | 等级 | 对策 |
|------|:--:|------|
| .dat 和 .idx 不一致（崩溃时索引尾条丢失） | 🔴 高 | 启动时检测：取 .idx 最后一条 timestamp，扫描 .dat 中该时间之后的记录，补索引条目。单一数据源原则：.dat 是 truth，.idx 可从 .dat 重建 |
| mmap 映射大文件失败（IMX6ULL 32 位地址空间） | 🟡 中 | Lv3 阶段数据量不超过 10 万条，.idx = 1.2MB，没问题。32 位进程 3GB 用户空间绰绰有余 |
| 批量缓冲的最后一批丢失（进程崩溃时 buffer 未 flush） | 🟡 中 | 时序数据允许少量丢失。文档明确"最多丢失最后 500ms 数据"。需要零丢失时提供 `--sync` 模式 |
| compact 时 rename 跨设备 | 🟢 低 | .tmp 文件写同一目录，不跨分区。已在 Lv2 验证 |
| bsearch 定位后 .dat 中该 offset 的记录已被 tombstone | 🟢 低 | 定位的是 offset 而非记录号，tombstone 不改变 offset。查询时从该 offset 顺序读，遇到 tombstone 跳过 |
| 缓冲区满时 insert 还没拿到足够记录 | 🟢 低 | 直接模式的 CLI 每次只插一条，缓冲意义不大。**Lv3 的缓冲主要为 Lv4 守护进程持续写入做准备**，Lv3 可以先简化：单条 insert → 立即写 .idx |

---

## 五、子阶段分解

### 子阶段 1：.idx 索引文件格式 + 基础读写（~120 行）

**目标**：insert 时同时追加索引条目到 .idx，query 从 .idx 读索引进而定位 .dat。

**涉及文件**：`index.c`（新增）、`shdb.h`、`storage.c`、`query.c`

**.idx 索引条目定义**：

```c
struct index_entry {
    uint64_t timestamp;      // record.timestamp（大端存盘）
    uint32_t record_offset;  // record 在 .dat 中的绝对偏移（大端存盘）
} __attribute__((packed));   // 12 字节
```

**改动点**：

| 文件 | 改动 |
|------|------|
| `shdb.h` | 加 `index_entry` 结构体、`IDX_SUFFIX ".idx"` 宏、索引相关函数声明 |
| `index.c` | `idx_append(fd, timestamp, offset)` — lseek 到 .idx 末尾 → write entry |
| `index.c` | `idx_open(path)` — open .idx，不存在则创建 |
| `storage.c` | `cmd_insert` 写 .dat 后，调 `idx_append` 追加索引条目 |
| `query.c` | `cmd_query`：先打开 .idx → 线性扫描 .idx（Lv3.1 先不用二分），通过 entry.offset 直接 lseek 到 .dat 对应位置读 record |

**为什么 Lv3.1 不用二分查找**：先验证 .idx 格式正确——insert 能写对、query 能通过 .idx 定位到正确记录。格式不对二分白搭。等 Lv3.2 mmap + bsearch 一步到位。

**自检方式**：
```bash
# 清空旧数据
rm -f data.shdb data.shdb.idx

# 插入几条
./shdb insert --class person --conf 0.95 --bbox 120,80,200,300
./shdb insert --class car    --conf 0.72 --bbox 50,30,180,250
./shdb insert --class dog    --conf 0.88 --bbox 10,10,300,400

# 验证 .idx 文件
hexdump -C data.shdb.idx
# 应有 3 条 entry，每条 12B；timestamp 递增；offset 分别指向 3 条 record

# 验证 query 通过索引定位（先用线性扫描 .idx）
./shdb query --from "2026-06-20 09:00:00" --to "2026-06-22 23:00:00"
# 输出应与直接线性扫描 .dat 一致
```

**交付物**：`index.c`（idx_append + idx_open）+ `shdb.h`（index_entry + 声明）+ storage.c/query.c 改动

---

### 子阶段 2：mmap + 二分查找（~100 行）

**目标**：query 时不再从头扫描 .idx，改为 mmap 映射 → bsearch 二分查找定位起始 offset。

**涉及文件**：`index.c`、`query.c`

**mmap 映射 .idx**：

```c
void *idx_map = mmap(NULL, idx_size, PROT_READ, MAP_SHARED, idx_fd, 0);
```

- `PROT_READ`：只读映射，不需要写
- `MAP_SHARED`：内核页缓存直接映射，零拷贝
- 映射后可以像数组一样访问：`((struct index_entry *)idx_map)[i].timestamp`

**二分查找**：手写（不用 bsearch，因为要处理"第一个 >= target"的定位问题）。给定 target timestamp，在 .idx 中找到第一个 `entry.timestamp >= target` 的位置，返回 `entry.record_offset`。

**改动点**：

| 文件 | 改动 |
|------|------|
| `index.c` | `idx_find_offset(idx_map, count, from_ts)` — 二分查找返回起始 offset |
| `index.c` | `idx_map_open(path, size)` — mmap .idx，返回映射地址和 entry 数量 |
| `query.c` | `cmd_query`：调 `idx_find_offset` 定位起始 offset → lseek + 顺序读 .dat → 遇到 timestamp > to_ts 则停止（不用扫到底） |

**性能提升**：10 万条记录，线性扫描 .idx = 120 万字节全读。二分查找 = log2(100000) ≈ 17 次比较，O(log N)。

**自检方式**：
```bash
# 插 100 条测试数据，query 查最后 10 条
# 预期：输出正确，且不输出时间范围之外的记录
```

**交付物**：`index.c`（mmap + 二分查找）

---

### 子阶段 3：批量缓冲写入（~100 行）

**目标**：insert 不再每次写盘，改为积攒 buffer[64] 满了或超时 500ms 才 flush。

**涉及文件**：`storage.c`、`shdb.h`

**缓冲区设计**：

```c
#define BATCH_SIZE 64

struct insert_buffer {
    struct detect_record records[BATCH_SIZE];
    int count;                   // 当前已攒条数
    time_t last_flush_ms;        // 上次 flush 时间
};

static struct insert_buffer batch;
```

**flush 逻辑**：open .tmp → 复制原 .dat → 批量写 buffer 中所有 record 和 index entry → fsync → rename。

**触发 flush 的条件**：
1. `batch.count >= BATCH_SIZE`（缓冲区满）
2. `now_ms - batch.last_flush_ms > 500`（超时）
3. 程序正常退出时（SIGTERM，Lv4 才用）

**注意**：Lv3 CLI 模式每次 insert 启动一个新进程，buf 不会跨命令持久化。批量缓冲真正发挥作用是在 Lv4 守护进程模式。Lv3 先实现 buffer 结构 + flush 函数，当前每条 insert 后调 flush 保证立即写盘即可。等到 Lv4 shdbd 后台运行时，自然就变成攒 64 条才刷。

**自检方式**：
```bash
./shdb insert --class person --conf 0.95 --bbox 120,80,200,300
hexdump -C data.shdb | tail -3  # 记录正确落盘
# .idx 也应有对应条目
```

**交付物**：`storage.c`（insert_buffer + batch_flush 函数）+ shdb.h（BATCH_SIZE 宏）

---

### 子阶段 4：compact 压缩（~80 行）

**目标**：新增 `shdb compact` 命令。遍历 .dat → 跳过 tombstone → 写入新 .tmp → 重建 .idx → rename。

**涉及文件**：`storage.c`、`parser.c`、`index.c`、`shdb.h`

**compact 核心逻辑**：
1. 打开 data.shdb，遍历所有 record
2. 跳过 `flags & TOMBSTONE` 的记录
3. 存活的 record 写入 data.shdb.tmp（新 ID 重新编号或保留旧 ID？→ 保留旧 ID，因为外部可能引用了 ID）
4. 同时重建 data.shdb.idx.tmp（只包含存活记录）
5. fsync + rename（data.shdb.tmp → data.shdb，data.shdb.idx.tmp → data.shdb.idx）

**注意**：compact 后文件变小，空闲链表就无用了（tombstone 全清）。free_slots 数组自然为空，和 Lv2 的空闲链表逻辑兼容。

**改动点**：

| 文件 | 改动 |
|------|------|
| `storage.c` | `cmd_compact(argc, argv)` — 全量重写 |
| `parser.c` | dispatch 表加 `{"compact", cmd_compact}` |
| `shdb.h` | `cmd_compact` 声明、`COMMAND_NUMBER` → 5 |

**自检方式**：
```bash
./shdb insert --class person --conf 0.95 --bbox 10,10,10,10  # id=0
./shdb insert --class car    --conf 0.72 --bbox 20,20,20,20  # id=1
./shdb delete --id 1
./shdb compact
./shdb query --from "2026-06-20 09:00:00" --to "2026-06-22 23:00:00"
# 只有 id=0 的记录，id=1 被永久删除
./shdb info
# total_records=1, alive_records=1
```

**交付物**：`storage.c`（cmd_compact）+ `parser.c`（dispatch 更新）+ `index.c`（idx_rebuild）

---

### 子阶段 5：索引自动修复 + note.md 更新（~80 行）

**目标**：每次启动时自动检测 .idx 状态，修复不一致；补 note.md Lv3 部分。

**启动索引检查流程**（`storage.c` 打开文件时自动触发）：

1. .dat 存在但 .idx 不存在 → 全盘扫描 .dat 重建 .idx
2. .dat 和 .idx 都存在 → 取 .idx 最后一个 entry 的 timestamp → 扫描 .dat 中该时间之后的 record → 补索引条目到 .idx
3. .idx 中 entry 数量 > .dat 中 record 数量 → 截断 .idx 到匹配

**note.md 新增内容**：
1. 为什么时间戳索引用独立 .idx 文件而不是存在 mmap 里？——可重建、不占用进程地址空间、崩溃恢复简单
2. .idx 为什么 append-only？——索引条目不修改，只有新增和重建，写入模式简单可靠
3. shdb vs fdb 设计对比：
   - fdb 按 key 哈希查找 → 精确点查，O(1)
   - shdb 按时间戳范围扫描 → 区间查询，索引定位 O(log N) + 顺序扫描
   - fdb 索引是哈希表（内存结构，重启重建）
   - shdb 索引是有序数组（磁盘持久化，append-only）
4. 为什么二分不用 bsearch() 标准库而是手写？——需要查"第一个 >= target"，bsearch 找到的是"任意一个 = target"，不满足需求

**自检方式**：
```bash
# 模拟 .idx 丢失
rm data.shdb.idx
./shdb query --from "2026-06-20 09:00:00" --to "2026-06-22 23:00:00"
# 应自动重建 .idx，查询正常

# 模拟 .idx 不完整
# 手动截断 .idx 文件后，query 应仍正常（自动补齐）
```

**交付物**：`index.c`（idx_check_and_repair）+ note.md 更新

---

## 六、Lv3 子阶段总览

| 子阶段 | 目标 | 涉及文件 | 行数 | 核心新技术 |
|:--:|------|------|:--:|------|
| Lv3.1 | .idx 格式 + 基础读写 | index.c（新增）, shdb.h, storage.c, query.c | ~120 | 索引格式设计 |
| Lv3.2 | mmap + 二分查找 | index.c, query.c | ~100 | mmap, bsearch |
| Lv3.3 | 批量缓冲写入 | storage.c, shdb.h | ~100 | buffer 管理 |
| Lv3.4 | compact 压缩 | storage.c, parser.c, index.c, shdb.h | ~80 | 全量重写 + 索引重建 |
| Lv3.5 | 索引自动修复 + note.md | index.c, note.md | ~80 | 崩溃恢复设计 |

**Lv3 总计**：~480 行新增/修改，项目总行数从 ~1050 增长到 ~1530。

---

## 七、Lv3 完成标准

- [ ] `make` 零 warning（`-Wall -Wextra`）
- [ ] insert 同时追加 .idx 索引条目，格式正确（hexdump 验证）
- [ ] query 通过 mmap + 二分查找定位，输出与 Lv2 线性扫描一致
- [ ] .idx 不存在时自动全盘扫描重建
- [ ] .idx 比 .dat 少时自动补索引条目
- [ ] `.idx 比 .dat 多时自动截断
- [ ] `shdb compact` 正确删除 tombstone 记录，重建索引，文件变小
- [ ] 批量缓冲 insert_buffer 结构实现，flush 逻辑正确
- [ ] note.md 记录了 shdb vs fdb 设计对比、索引设计决策

---

## 八、Lv3 面试叙事

> "我设计了 .idx 时间戳索引文件，[timestamp, offset] 定长条目按时间升序 append-only。查询时 mmap 映射到进程空间零拷贝，手写二分查找找起始 offset 然后顺序扫描。索引的设计原则是'从数据重建'——.dat 是唯一真相来源，.idx 崩溃丢失了全盘扫描就能恢复。同时做了 compact 空间回收和批量缓冲写入优化。"
>
> **面试官**："你比大多数应届生理解得深——知道时间戳索引为什么和 KV 索引设计不同，怎么做崩溃恢复。"
