# Demo Clock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Provide a reversible, realistic connection demonstration that starts the clock at 12:00:00 without BluFi or network access.

**Architecture:** `main` enables the demo path with one compile-time flag and queues simulated connection events. `clock_display` consumes the existing status pages in order and invokes the time service only after the green page expires. `time_service` derives demo time from ESP timer elapsed time and disables real synchronization while active.

**Tech Stack:** ESP-IDF 5.3, FreeRTOS queue/task, `esp_timer`, existing clock logic host tests.

---

### Task 1: Define Demo-Time Test Contract

**Files:**
- Modify: `tests/clock_logic_test.c`
- Modify: `main/common/clock_logic.h`
- Modify: `main/common/clock_logic.c`

- [x] **Step 1: Add a failing elapsed-time conversion test**

```c
static void test_demo_time_starts_at_noon_and_accumulates_elapsed_time(void)
{
    clock_time_t now = {0};
    clock_time_from_elapsed_ms(0, &now);
    assert(now.hour == 12 && now.minute == 0 && now.second == 0 && now.millisecond == 0);
    clock_time_from_elapsed_ms(3723004, &now);
    assert(now.hour == 13 && now.minute == 2 && now.second == 3 && now.millisecond == 4);
}
```

- [ ] **Step 2: User verification after local build is available**

Run the existing host test command in the project environment. Before implementation, it must fail because `clock_time_from_elapsed_ms` does not exist. This workspace intentionally does not run compilation by user instruction.

- [x] **Step 3: Implement the conversion helper**

Add `clock_time_from_elapsed_ms(uint64_t elapsed_ms, clock_time_t *out)` to `clock_logic`. Convert `12:00:00` plus elapsed milliseconds modulo 24 hours into all four clock fields; return without writing when `out` is null.

- [ ] **Step 4: User verification after implementation**

Run the same host test. It must pass, including the new noon and elapsed-time assertions.

### Task 2: Add the Reversible Demo Source

**Files:**
- Create: `main/common/app_mode.h`
- Modify: `main/middle/time_service.h`
- Modify: `main/middle/time_service.c`

- [x] **Step 1: Add the compile-time switch**

```c
#pragma once
#define ECLOCK_DEMO_MODE 1
```

The one switch is the only production toggle. Setting it to `0` restores real boot synchronization and BluFi provisioning.

- [x] **Step 2: Add `time_service_start_demo()`**

Record `esp_timer_get_time()` as the demo epoch, set the time-valid state, and request a display refresh. In `time_service_get_local`, use `clock_time_from_elapsed_ms()` when demo time is active. Make `time_service_start_sync()` and the daily 03:00 worker return without network activity while demo mode is active.

- [ ] **Step 3: User verification after local build is available**

After flashing, wait for the green page to finish. The rendered frame must be the 12:00 overlap colour at that instant, then progress in real elapsed seconds.

### Task 3: Serialize the Simulated Connection Journey

**Files:**
- Modify: `main/common/clock_logic.h`
- Modify: `main/app/clock_display.c`
- Modify: `main/main.c`

- [x] **Step 1: Add a no-colour `CLOCK_EVENT_DEMO_READY` event**

Extend the existing event enum after the status events. It represents the transition point, not an LED page.

- [x] **Step 2: Start the demo only when the event reaches the display head**

In `clock_display`, process `CLOCK_EVENT_DEMO_READY` by calling `time_service_start_demo()`, rather than treating it as a status colour. Queue order remains BLE blue, Wi-Fi green, then demo ready.

- [x] **Step 3: Select demo events in `app_main`**

When `ECLOCK_DEMO_MODE` is `1`, queue `CLOCK_EVENT_BLE_CONNECTED`, `CLOCK_EVENT_WIFI_CONNECTED`, and `CLOCK_EVENT_DEMO_READY`; do not request boot Wi-Fi synchronization. When `0`, retain the existing saved-Wi-Fi startup behavior.

- [ ] **Step 4: User hardware verification after flashing**

Observe exactly: five-second white comet, one-second blue full ring, one-second green full ring, then the `12:00:00` cyan overlap. Hold BOOT for three seconds in demo mode: no real BluFi advertising or network traffic must begin.

### Task 4: Document the Temporary Mode

**Files:**
- Modify: `README.md`

- [x] **Step 1: Add demo mode instructions**

Document the enabled default, the visible event sequence, the `12:00:00` transition, and the single `ECLOCK_DEMO_MODE` switch required to restore production networking.

### Review Notes

- The design gives the display task exclusive control over the transition, so the start time cannot drift by the boot/status-page duration.
- The networking modules remain compiled but have no connect, provisioning, HTTP, or SNTP start path in demo mode.
- Build and flash verification are intentionally delegated to the user.
