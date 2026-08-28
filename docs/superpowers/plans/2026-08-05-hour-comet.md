# 时针彗星与刷新率 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 以 100 FPS 显示灯环，并在时针换格时播放一次白色六尾灯整圈彗星。

**Architecture:** `clock_logic` 依据时间生成时针彗星状态和灯帧，`clock_display` 保持无状态渲染并把任务周期调整为 10ms。彗星和分针共用现有逐像素合成规则。

**Tech Stack:** ESP-IDF、FreeRTOS、RMT WS2812 驱动、C 断言测试。

---

### Task 1: 时针彗星逻辑

**Files:**
- Modify: `main/common/clock_logic.h`
- Modify: `main/common/clock_logic.c`
- Test: `tests/clock_logic_test.c`

- [ ] **Step 1: 写入失败测试**

```c
clock_time_t now = {.hour = 12, .minute = 30, .second = 0, .millisecond = 0};
clock_hand_frame_t hands = {0};
clock_compute_hands(&now, &hands);
assert(hands.hour_comet_active);
assert(hands.hour_comet_head_index == 0);
```

- [ ] **Step 2: 实现彗星状态与六级尾灯渲染**

```c
out->hour_comet_active = hour_phase_ms < CLOCK_HOUR_COMET_DURATION_MS;
out->hour_comet_head_index = ...;
```

- [ ] **Step 3: 验证彗星在 1.2 秒后退出并保留新时针**

```c
clock_time_t finished = {.hour = 12, .minute = 30, .second = 1, .millisecond = 200};
clock_compute_hands(&finished, &hands);
assert(!hands.hour_comet_active);
assert(hands.hour.current_index == 1);
```

### Task 2: 100 FPS 显示周期

**Files:**
- Modify: `main/app/clock_display.c`

- [ ] **Step 1: 把显示周期由 20ms 调整到 10ms**

```c
#define DISPLAY_FRAME_MS 10U
```

- [ ] **Step 2: 静态核对所有时针彗星常量、测试和刷新周期；不构建、不烧录、不读取串口**
