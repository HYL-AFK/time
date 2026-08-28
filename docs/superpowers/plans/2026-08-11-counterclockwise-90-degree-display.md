# 全部灯环显示逆时针 90 度 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将灯环的所有逻辑像素输出统一逆时针旋转 90 度。

**Architecture:** 保持时间和渲染逻辑使用现有逻辑索引，在 `led_ring_map_logical_to_physical()` 的最终逻辑到物理转换中叠加 6 灯偏移（本硬件物理编号方向使加法对应几何逆时针）。所有调用该映射的时钟、彗星、校准和状态输出自动共享同一旋转。

**Tech Stack:** ESP-IDF C 固件、C assert 测试。

---

### Task 1: 固定逆时针旋转的失败测试

**Files:**
- Modify: `tests/clock_logic_test.c`

- [x] **Step 1: 更新映射预期**

```c
assert(led_ring_map_logical_to_physical(0, 0, true) == 6);
assert(led_ring_map_logical_to_physical(6, 0, true) == 12);
assert(led_ring_map_logical_to_physical(0, 5, true) == 11);
assert(led_ring_map_logical_to_physical(1, 5, true) == 12);
assert(led_ring_map_logical_to_physical(1, 0, false) == 5);
```

- [x] **Step 2: 运行测试确认旧实现失败**

运行：`cc -std=c11 -Wall -Wextra -I main/common -I main/drivers tests/clock_logic_test.c main/common/clock_logic.c main/drivers/led_ring_map.c -o tmp/clock_logic_test.exe; tmp/clock_logic_test.exe`

预期：断言在第一个旋转映射处失败。

### Task 2: 在映射层实现逆时针 90 度

**Files:**
- Modify: `main/drivers/led_ring_map.c`

- [x] **Step 1: 加入 6 灯固定偏移**

先按现有 `clockwise` 方向计算逻辑索引，再将结果减去 6 个物理位置并对 24 取模；逆时针配置同样使用相同的物理旋转，以确保所有硬件方向配置都代表同一个表盘旋转。

### Task 3: 回归验证

**Files:**
- No additional files

- [x] **Step 1: 重新运行完整 C 测试**

运行同 Task 1 的编译和执行命令，预期输出 `clock_logic_test: PASS`。

- [x] **Step 2: 检查调用覆盖**

确认 `main/drivers/led_ring.c` 的所有逐像素输出和校准输出都经过 `led_ring_map_logical_to_physical()`，无需在渲染层增加重复旋转。
