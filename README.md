# ESP32-C3 Circular WiTime Clock

Firmware for a 24-pixel WS2812 circular clock. The dial is logical-indexed from 12 o'clock clockwise: logical `0` is 12 o'clock and logical `12` is the USB-port side at 6 o'clock.

## Implemented behavior

- Wi-Fi uses saved credentials only for initial synchronization and daily synchronization at 03:00. Synchronization has a 15-second total budget and at most three association attempts.
- Time is accepted only when SNTP and IP-derived UTC offset both succeed. The current UTC offset is stored in NVS and refreshed each day.
- BluFi starts automatically at boot for a five-minute provisioning window named `ESPARK-ECLOCK-<MAC last 6 hex digits>`. A three-second BOOT hold cancels network work and reopens that window.
- Boot animation is a seven-second clockwise white comet from 12 o'clock with a six-pixel tail. It always finishes before a connection status page is shown.
- The hour hand is white and moves as one LED every 30 minutes. The minute hand is green and crossfades during the final second of every 2.5-minute step. Overlap is teal green.
- Logical brightness is 100% from 06:00 through 21:00 and 40% from 21:01 through 05:59. The LED driver limits full logical output to 10% electrical RGB output.

## Temporary Demo Mode

`main/common/app_mode.h` currently sets `ECLOCK_DEMO_MODE` to `0`. It is retained only for validating the clock before a BluFi client is available:

1. The normal seven-second white boot comet makes four full rotations without interruption.
2. Simulated BLE provisioning breathes blue for eight seconds, then presents a full blue success ring for one second.
3. Simulated Wi-Fi association and time acquisition breathes green for six seconds, then presents a full green success ring for one second.
4. When the green success page finishes, the local clock starts at `12:00:00` and advances from the ESP monotonic timer.

This path does not start BluFi, advertise Bluetooth, connect Wi-Fi, request IP location, or start SNTP. It also ignores the daily 03:00 sync trigger. Set `ECLOCK_DEMO_MODE` to `1` only to exercise this path, then restore it to `0` for the real WiTime provisioning and synchronization flow.

With real mode enabled, the ring breathes blue while waiting for BluFi, green quickly after Bluetooth connects while Wi-Fi credentials are expected, and green at the normal rate while associating (up to ten seconds). After association, the cyan page covers a Baidu connectivity ping of up to three packets (700 ms each; the first reply proceeds immediately), SNTP (up to five seconds), and IP timezone lookup (up to three seconds), in that order. The serial log identifies the failing stage. Success is a same-color double flash followed by one second of solid color; time-sync success is solid cyan for one second before the normal clock resumes. Wi-Fi and sync failures breathe red for one second and return to blue provisioning. A five-minute BLE timeout breathes purple for one second, clears the ring, and enters deep sleep; use the physical reset or power-cycle the device to start again.

## First hardware calibration

The following values are provisional and centralized in the indicated headers:

| Item | Current candidate | Change location |
| --- | --- | --- |
| WS2812 data pin | GPIO3 | `main/drivers/led_ring.h` |
| BOOT button pin | GPIO9 | `main/drivers/button.h` |
| Pixel order | GRB, 800 kHz | `main/drivers/led_ring.c` |
| 12 o'clock pixel and direction | offset 0, clockwise | `main/drivers/led_ring.h` |

Before normal use, temporarily call `led_ring_show_calibration_step()` from `app_main()` to light logical pixels `0` through `23` one at a time, then verify red, green, and blue. Use the observed sequence to set `LED_RING_FIRST_PIXEL_OFFSET` and `LED_RING_CLOCKWISE`. Verify that holding GPIO9's button for three seconds opens BluFi; if it does not, stop using GPIO9 and replace the button pin with the board's confirmed schematic value.

The project declares `espressif/led_strip` in `main/idf_component.yml`; ESP-IDF Component Manager resolves it during the user's normal build.
