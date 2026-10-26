# WiTime 测试 Demo 固件

ESP32-C3 侧合并固件，单文件烧录（bootloader + 分区表 + 应用已合并）。

## 文件

| 文件 | 说明 |
|---|---|
| `time-demo-v0.4.bin` | ESP32-C3 合并固件，烧录地址 `0x0` |

**ASRPRO 语音模块的固件不在本目录**，需用天问Block 单独生成模型 + 编译下载（源码见 `tmp/asr_link_test2.cpp`）。

## 烧录

```bash
esptool.py --chip esp32c3 -p COM16 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 2MB --flash_freq 80m \
  0x0 time-demo-v0.4.bin
```

或用 ESP-IDF：

```bash
idf.py -p COM16 flash
```

## 硬件

| 连接 | 说明 |
|---|---|
| ESP32-C3 TX (GPIO1) | → ASRPRO RX (PA5) |
| ESP32-C3 RX (GPIO0) | ← ASRPRO TX (PA6) |
| GND | 必须共地 |

**TF 卡**：FAT32，根目录放音频文件（见下表），插在 ASRPRO 上。

## TF 卡音频

| 编号 | 用途 | 规格 |
|---|---|---|
| `001.mp3` ~ `012.mp3` | 整点报时提示音 | 48 kbps / 44.1 kHz / 单声道 |
| `101.mp3` | 闹钟铃声 | 同上 |
| `201.mp3` | 倒计时到点背景音 | 同上 |

编号 = 文件名数字，`playMp3Folder(0, 编号)` 直接按数字查找。
转换后的音频在 `tmp/tf_audio/`。

## 功能

- 语音问时间 / 日期 / 天气
- 整点报时（001~012 提示音）
- 闹钟：多步语音设置（日期 → 时段 → 时间）、响铃、停止、取消
- 正计时 / 倒计时（13 档：1/2/3/4/5/8/10/15/20/25/30/45/60 分钟）
- 倒计时到点：TF 卡音乐 4 秒 + 语音「您好，时间到了」
- 只重启 ESP32 会自动重新问候并校时

## 已知限制

- ASRPRO 是整句匹配，闹钟需按引导分步说，不支持「今天下午三点的闹钟」这类一句话说法
- TF 卡 MP3 与语音播报不能混音，只能顺序播放
- 语音音量与 TF 卡音量分别映射（见 `apply_volume`）

## 版本

`v0.4-demo` — 2026-10-26
