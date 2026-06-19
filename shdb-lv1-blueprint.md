# shdb Lv1 独立评测 & 子阶段分解

## 一、Lv1 定位

摘自蓝图第十三节。Lv1 目标就一句话：**存进去、查出来。** 不做任何优化，先验证磁盘格式设计和基本读写逻辑。

---

## 二、Lv1 前置知识检查

| 需要 | 状态 | 来源 |
|------|:--:|------|
| open/read/write/lseek | ✅ | fdb |
| 定长记录 + O(1) 偏移跳转 | ✅ | fdb |
| dispatch 表（函数指针） | ✅ | bfile |
| strtok_r 命令行解析 | ✅ | bfile / shell |
| 大小端 swap | ✅ | bfile |
| `__attribute__((packed))` | ✅ | bfile |
| snprintf 格式化输出 | ✅ | logd |
| `clock_gettime()` 时间戳 | ✅ | 系统编程基础 |

**结论**：Lv1 零新知识，8 项全会。**立即可开工。**

---

## 三、Lv1 功能边界

### 做

| 功能 | 命令 | 说明 |
|------|------|------|
| 写入记录 | `shdb insert --class person --conf 0.95 --bbox 120,80,200,300` | 四个参数全必填，系统自动打毫秒时间戳 |
| 时间范围查询 | `shdb query --from 09:00 --to 18:00 [--class X] [--min-conf 0.8]` | from/to 必填，class/min-conf 可选 |
| 文件信息 | `shdb info` | 打印文件头 + 计算总记录数 |

### 故意不做（留给 Lv2+）

| 不做 | 原因 |
|------|------|
| 原子写入（.tmp → rename → fsync） | Lv2，Lv1 先验证格式 |
| tombstone + 空闲链表 | Lv2 |
| .idx 时间戳索引 | Lv3 |
| CRC32 校验（写 0，读忽略） | Lv2 |
| 批量缓冲写入 | Lv3 |
| 守护进程 / FIFO | Lv4 |
| 聚合统计 | Lv5 |

### 磁盘格式

```
┌──────────────────────────────────────────┐
│  文件头 16B                               │
│  magic(4) | version(2) | record_size(2)  │
│  created_at(8)                           │
├──────────────────────────────────────────┤
│  Record 36B × N                           │
│  timestamp(8) | class_id(1) | conf(4)    │
│  bbox_x(2) | bbox_y(2) | bbox_w(2)      │
│  bbox_h(2) | thumb_off(4) | thumb_sz(4) │
│  flags(1) | reserved(2) | crc32(4)       │
└──────────────────────────────────────────┘
```

---

## 四、Lv1 风险清单

| 风险 | 等级 | 对策 |
|------|:--:|------|
| 磁盘格式定错，后续全改 | 🔴 高 | 文件头 version 字段兜底；子阶段1 反复推敲再动笔 |
| insert 字段名和蓝图不一致 | 🟡 中 | 所有 `--xxx` 映射集中写在 parser.c，不散落 |
| 时间字符串解析写错 | 🟡 中 | 子阶段3 单独验证时间解析 → 时间戳转换 |
| 文件不存在时的行为不一致 | 🟢 低 | 明确规则：insert 自动创建；query/info 报 ENOENT |

---

## 五、子阶段分解

### 子阶段 1：数据结构定型（~60 行）

**目标**：shdb.h 写完，编译通过，结构体大小验证正确。

**文件**：`shdb.h`

**内容**：
- 命令枚举
- `detect_record` 结构体（36B，`__attribute__((packed))`）
- 文件头结构体（16B）
- 命令结构体（name + handler）
- 函数指针类型 `handle_fun_t`
- 常量：`MAGIC`、`VERSION`、`RECORD_SIZE`、`HEADER_SIZE`、数据文件默认路径
- insert/query 参数结构体
- 所有公共函数声明

**自检方式**：
```c
// 临时 test_size.c
_Static_assert(sizeof(struct detect_record) == 36, "record must be 36 bytes");
_Static_assert(sizeof(struct file_header) == 16, "header must be 16 bytes");
```

**交付物**：`shdb.h`

---

### 子阶段 2：写入链路 — insert 能跑（~180 行）

**目标**：输一条命令，数据落到磁盘。

**文件**：
- `save.c`：文件初始化 + 文件头写入 + 时间戳 + 构造 record + 追加写入
- `parser.c`（局部）：`--class/--conf/--bbox` 解析 → `insert_args` 结构体
- `main.c`（临时 stub）：手写 `argv[1]` 判断，只调 `cmd_insert`

**函数清单**：

| 文件 | 函数 | 职责 |
|------|------|------|
| save.c | `db_init(fd)` | 文件不存在则创建 + 写文件头；存在则打开 |
| save.c | `cmd_insert(argc, argv)` | 解析 args → 构造 record → lseek 到末尾 → write |
| parser.c | `parse_insert_args(argc, argv, &args)` | `--class` 字符串转 class_id，`--conf` 转 float，`--bbox` 拆四个 uint16 |

**自检方式**：
```bash
shdb insert --class person --conf 0.95 --bbox 120,80,200,300
hexdump -C data.shdb | head -3      # 验证 magic "SHDB" + 36字节记录
# 期望文件大小：16 + 36 = 52 字节
```

**关键约束**：
- insert 首次调用自动创建文件 + 写文件头
- 时间戳用 `clock_gettime(CLOCK_REALTIME, ...)`，毫秒级
- bbox 逗号分隔解析用 `strtok_r`
- 文件头字段大于 1 字节的，写之前做 大小端 swap（小端→大端写盘，读回时 swap 回来）

**交付物**：`save.c` + `parser.c`（insert 部分）+ 临时 `main.c`

---

### 子阶段 3：查询链路 — query + info 能跑（~180 行）

**目标**：能查回来，能看文件状态。

**文件**：
- `read.c`：线性扫描查询 + 过滤 + JSON 输出 + 文件信息
- `parser.c`（扩展）：`--from/--to/--class/--min-conf` 解析

**函数清单**：

| 文件 | 函数 | 职责 |
|------|------|------|
| read.c | `cmd_info(argc, argv)` | 读文件头 → 打印元信息 → `(file_size - 16) / 36` 算总数 |
| read.c | `cmd_query(argc, argv)` | 线性扫描 → 时间戳比对 → class/conf 过滤 → JSON 一行一条 |
| parser.c | `parse_time(str, &ts)` | `"2026-06-05T09:00:00"` 或 `"09:00"` → `uint64_t` 毫秒时间戳 |

**查询核心逻辑**：
```c
while (read(fd, &rec, RECORD_SIZE) == RECORD_SIZE) {
    if (rec.timestamp < args.from_ts || rec.timestamp > args.to_ts)
        continue;
    if (args.has_class && rec.class_id != args.class_id)
        continue;
    if (args.has_min_conf && rec.confidence < args.min_conf)
        continue;
    print_json(&rec);   // 一行 JSON
}
```

**JSON 输出格式**：
```json
{"ts":1717549200000,"class":"person","conf":0.95,"bbox":[120,80,200,300]}
```

**自检方式**：
```bash
# 先插几条
shdb insert --class person --conf 0.95 --bbox 120,80,200,300
shdb insert --class car --conf 0.72 --bbox 50,30,180,250

# 查
shdb query --from 2026-06-06T09:00:00 --to 2026-06-06T18:00:00
# 期望输出：两条 JSON（假设时间戳在范围内）

shdb query --from 09:00 --to 18:00 --class person --min-conf 0.8
# 期望输出：只有 person 那条

shdb info
# 期望输出：version=1, record_size=36, total_records=2
```

**交付物**：`read.c` + `parser.c`（完整）

---

### 子阶段 4：串联 & 打磨（~80 行）

**目标**：完整 CLI，编译零 warning。

**文件**：
- `main.c`：入口，调 dispatch
- `Makefile`：构建规则
- `note.md`：设计决策记录

**main.c 内容**：
```c
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "usage: shdb <insert|query|info> ...\n");
        return 1;
    }
    return dispatch(argc - 1, argv + 1);
}
```

**Makefile 要求**：
- `CFLAGS = -Wall -Wextra -std=c11 -O2`
- 五个 .c → 一个 `shdb` 可执行文件
- `make clean` 清理 .o 和可执行文件

**自检方式**：
```bash
make clean && make          # 零 warning
./shdb                       # 打印 usage
./shdb insert --class person --conf 0.95 --bbox 120,80,200,300
./shdb info                  # total_records=1
./shdb query --from 09:00 --to 18:00  # 输出 JSON
```

**交付物**：`main.c` + `Makefile` + `note.md`

---

## 六、Lv1 子阶段总览

| 子阶段 | 目标 | 新增文件 | 行数 | 可测 |
|:--:|------|------|:--:|:--:|
| Lv1.1 | 数据结构定型 | shdb.h | ~60 | `_Static_assert` 验证 |
| Lv1.2 | insert 能跑 | save.c + parser.c(insert) | ~180 | hexdump .dat |
| Lv1.3 | query + info 能跑 | read.c + parser.c(完整) | ~180 | 插入→查询→比对 |
| Lv1.4 | 串联打磨 | main.c + Makefile + note.md | ~80 | 完整 happy path |

**Lv1 总计**：~500 行，4 个子阶段，每个子阶段结束时有可验证的交付物。

---

## 七、Lv1 完成标准

- [ ] `make` 零 warning（`-Wall -Wextra`）
- [ ] `shdb insert` 写入记录，hexdump 验证磁盘格式正确
- [ ] `shdb query` 按时间范围 + 可选过滤查出正确结果
- [ ] `shdb info` 显示正确的文件头信息和记录数
- [ ] 文件不存在时 insert 自动创建，query/info 报错
- [ ] note.md 记录了至少 2 个设计决策（为什么定长记录？为什么 append-only？）
