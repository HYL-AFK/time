# Demo Clock Design

The temporary demonstration mode gives hardware validation a deterministic path when no BluFi client is available. It never starts BLE provisioning, associates Wi-Fi, performs HTTPS location lookup, or starts SNTP.

At boot, the display task remains the sole owner of LED timing. `main` enqueues a simulated BLE-connected event, a simulated Wi-Fi-connected event, and a no-colour demo-ready event. The existing boot animation runs for five seconds; the display then presents blue for one second, green for one second, processes demo-ready, and starts the demo time source at exactly 12:00:00. This avoids advancing the time while the queued pages are still visible.

`time_service` owns the demo time source. It records the monotonic microsecond timestamp at demo-ready and derives hour, minute, second, and millisecond from elapsed time. While the source is active, regular synchronization requests and the 03:00 daily worker are ignored. Production behavior remains selected by a single compile-time flag in `main/common/app_mode.h`.

The demo mode is intended only for hardware validation. Set `ECLOCK_DEMO_MODE` to `0` before testing real BluFi provisioning and WiTime synchronization.
