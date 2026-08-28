# 稳定校时实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** IP 时区定位失败时，只要 SNTP 成功，时钟仍使用已保存时区或首次的 UTC+8 默认值显示。

**Architecture:** 在纯表盘逻辑中提供可测试的偏移选择函数。同步服务把 IP 定位由阻断条件改为可选刷新；仅定位成功时持久化新偏移，SNTP 是是否进入正常时钟的唯一时间来源。

**Tech Stack:** ESP-IDF、FreeRTOS、NVS、ESP Netif SNTP、现有主机侧 C 断言测试。

---

### Task 1: 偏移选择逻辑

**Files:**
- Modify: `main/common/clock_logic.h`
- Modify: `main/common/clock_logic.c`
- Test: `tests/clock_logic_test.c`

- [ ] **Step 1: 写入失败测试**

```c
assert(clock_select_utc_offset(false, 0, false, 0) == 28800);
assert(clock_select_utc_offset(true, 3600, false, 0) == 3600);
assert(clock_select_utc_offset(true, 3600, true, -18000) == -18000);
```

- [ ] **Step 2: 在主机侧执行测试，确认新增符号尚不存在**

由于用户要求不构建，此步骤在本次只保留测试代码，交由用户构建前执行。

- [ ] **Step 3: 实现最小偏移选择函数**

```c
int32_t clock_select_utc_offset(bool has_saved_offset, int32_t saved_offset,
                                bool has_fresh_offset, int32_t fresh_offset)
{
    if (has_fresh_offset) return fresh_offset;
    if (has_saved_offset) return saved_offset;
    return 8 * 60 * 60;
}
```

### Task 2: 定位失败降级

**Files:**
- Modify: `main/middle/time_service.c`
- Test: `tests/clock_logic_test.c`

- [ ] **Step 1: 将偏移来源与定位结果拆开**

```c
bool location_succeeded = request_utc_offset(...);
offset = clock_select_utc_offset(s_config.has_utc_offset, s_config.utc_offset_seconds,
                                 location_succeeded, offset);
```

- [ ] **Step 2: 定位失败只记录事件并继续等待 SNTP**

```c
if (!location_succeeded) {
    runtime_event_log_append(RUNTIME_EVENT_LOCATION_FAILURE, location_error,
                             location_status, location_provider);
}
```

- [ ] **Step 3: 仅在定位成功时保存新偏移**

```c
apply_sync(offset, location_succeeded);
```

- [ ] **Step 4: 验证源码无冲突标记；不构建、不烧录、不读取串口**
