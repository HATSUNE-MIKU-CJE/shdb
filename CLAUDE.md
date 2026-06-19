# shdb — 嵌入式时序数据库

旗舰 Boss 项目，融合 fdb、bfile、logd、mini-shell 四个前置项目的技术，写一个完整的嵌入式时序数据采集、存储、查询系统。

## 前置上下文

本项目是 [linux-learn](https://github.com/HATSUNE-MIKU-CJE/linux-learn) Phase 1 的收尾项目。完整教学计划、进度追踪、用户画像见:

- /home/miku/.claude/CLAUDE.md（用户画像 + 交互风格，全局共享）
- /home/miku/.claude/projects/-home-miku-linux-learn/memory/（教学计划/进度/项目规划）
    - teaching-plan.md — 完整学习路线
    - project-portfolio.md — 五个项目详细功能说明
    - user-profile-deep.md — 深层画像
    - strict-sister-style.md — 交互风格（严格大姐姐）

## 项目定位

- 1500-2500 行 C，6-8 个源文件
- 单机单进程，跑在正点原子 Linux 板子上
- 通过命名管道（FIFO）收传感器数据 → 自定义 .sdb 二进制格式落盘 → 支持时间范围查询

## 技术栈

fork / exec / pipe / dup2 / signal / mmap / lseek / rename / flock / mkfifo
函数指针表 / 环形队列 / container_of / 文件格式设计
