# WiTime 文档索引

**版本**：v0.4-demo · 2026-10-28

## 交付文档

| 文档 | 用途 | 读者 |
|---|---|---|
| [产品规格书](product/PRODUCT-SPEC.md) | 功能矩阵、技术参数、交互流程、已知限制 | 产品 / 测试 / 硬件 |
| [验收测试清单](acceptance/ACCEPTANCE-TEST-PLAN.md) | 60+ 测试用例、通过标准、验收判定 | 测试 |
| [UART 协议 V1.1](protocol/UART-PROTOCOL-V1.1.md) | 帧格式、CRC、帧类型、时序、调试 | 固件 |

## 历史设计文档

| 目录 | 内容 |
|---|---|
| `superpowers/specs/` | 早期设计（时钟动画、校时、整点报时、表针角度、语音对话流程） |
| `superpowers/plans/` | 对应实现计划 |
| `protocol/ASRPRO-ESP32 专用通信协议 V1.0.docx` | 旧版协议（已废弃，仅存档） |

## 快速导航

**我要验收** → [验收测试清单](acceptance/ACCEPTANCE-TEST-PLAN.md)（先看 §0 测试环境，再按 A~I 顺序跑）

**我要看功能有什么** → [产品规格书 §3 功能矩阵](product/PRODUCT-SPEC.md#3-功能矩阵)

**我要改协议** → [UART 协议 V1.1](protocol/UART-PROTOCOL-V1.1.md)（注意两端固件必须同步升级）

**链路 OFFLINE** → [协议 §8 调试要点](protocol/UART-PROTOCOL-V1.1.md#8-调试要点)

## 代码位置

```
main/
├── app/clock_display.c       灯环显示、页面、动画
├── common/
│   ├── app_config.c          NVS 配置（WiFi/闹钟/坐标/时区）
│   ├── clock_logic.c         时间 → 表针位置
│   └── runtime_event_log.c   崩溃/重启事件记录
├── drivers/
│   ├── led_ring.c            WS2812 驱动（GPIO3）
│   └── button.c              BOOT 按键（短按静音，长按擦 NVS）
├── middle/
│   ├── link_probe.c          UART 协议 + 心跳
│   ├── time_service.c        SNTP/天气/闹钟/整点报时
│   ├── timer_service.c       正计时/倒计时
│   ├── wifi_manager.c        WiFi 连接与重连
│   └── ble_provision.c       BluFi 配网
└── main.c                    启动流程、按键回调

tmp/asr_link_test2.cpp        ASRPRO 侧固件（天问Block）
```

## 硬件连接

| ESP32-C3 | ASRPRO V5.0 | 说明 |
|---|---|---|
| GPIO1 (TX) | PA5 (RX) | 9600 8N1 |
| GPIO0 (RX) | PA6 (TX) | 9600 8N1 |
| GND | GND | **必须共地** |
| GPIO3 | — | WS2812 数据（24 像素） |
| GPIO9 | — | BOOT 按键 |

## 音频文件（TF 卡）

| 编号 | 用途 | 规格 |
|---|---|---|
| `001.mp3` ~ `012.mp3` | 整点报时提示音 | 48 kbps / 44.1 kHz / 单声道 |
| `101.mp3` | 闹钟铃声 | 同上 |
| `201.mp3` | 倒计时到点 | 同上 |

**转换命令**：
```bash
ffmpeg -i input.mp3 -ar 44100 -ac 1 -b:a 48k output.mp3
```
