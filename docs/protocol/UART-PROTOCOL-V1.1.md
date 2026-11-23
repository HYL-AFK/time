# ASRPRO ↔ ESP32 通信协议 V1.1

**版本**：V1.1（定长帧）  
**日期**：2026-10-28  
**适用固件**：ESP32 `v0.4-demo` / ASRPRO `BUILD V1.6 1967L`



## 0. 协议版本与适用范围

本文档描述 V1.1 协议，自 2026 年 10 月起作为 ESP32 与 ASRPRO 之间的通信标准，覆盖时间、天气、闹钟、计时全部业务。

| 版本 | 帧长 | 帧头标识 | 状态 | 说明 |
|---|---|---|---|---|
| V1.0 | 9 字节 | LEN = 0x05 | 已停用 | 早期链路验证版本，仅支持心跳与基础播报 |
| V1.1 | 57 字节 | LEN = 0x35 | 现行 | 支持全部业务指令，载荷固定 48 字节 |

### 0.1 从 V1.0 到 V1.1 的变更原因

V1.1 将变长帧改为定长帧，主要出于三点考虑：

- 接收方可以按固定长度一次性收帧，不必逐字节推断帧边界，降低串口分包时的解析复杂度
- 载荷统一按 48 字节补齐，空载荷用 0xFF 填充，同一套校验逻辑适用于所有帧类型
- 新增闹钟、计时等业务后，固定结构更利于两端并行开发和联调

### 0.2 版本配套要求

ESP32 与 ASRPRO 的固件按配套版本发布。若两端版本不一致，帧长校验将无法通过，链路停留在未连接状态；此时应先核对两端固件版本，再排查硬件接线。


---

## 1. 物理层

| 项目 | 参数 |
|---|---|
| 接口 | UART（ESP32 UART1） |
| 波特率 | **9600**（不能用 115200，实测第 8 字节起漂移） |
| 数据格式 | 8 数据位 / 无校验 / 1 停止位（8N1） |
| 接线 | ESP32 GPIO1(TX) → ASRPRO PA5(RX)；ESP32 GPIO0(RX) ← ASRPRO PA6(TX)；**必须共地** |
| 流向 | A = ESP32（主机），E = ASRPRO（从机） |

---

## 2. 帧格式（定长 57 字节）

```
偏移  长度  字段        说明
 0     2    HEAD        固定 0x53 0x54（"ST"）
 2     1    LEN         固定 0x35（= 53）
 3     1    SEQ         帧序号，0x00~0xFF 循环
 4     1    TYPE        帧类型（见 §3）
 5    48    PAYLOAD     载荷，不足补 0xFF
53     2    CRC         CRC16-Modbus，小端（低字节在前）
55     2    TAIL        固定 0x43 0x21（"C!"）
```

**总长度**：`48 + 9 = 57` 字节。

### 2.1 CRC 计算

```c
// CRC16-Modbus：多项式 0xA001，初值 0xFFFF
// 覆盖范围：LEN 到 PAYLOAD 末尾，共 3 + 48 = 51 字节
uint16_t crc16_modbus(const uint8_t *p, int n)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
        }
    }
    return crc;
}
```

### 2.2 校验流程（接收方）

```
1. HEAD == 53 54 ?        否 → 丢弃
2. LEN  == 0x35 ?         否 → 丢弃（V1.0 的 0x05 会被拒）
3. TAIL == 43 21 ?        否 → 丢弃
4. CRC  == 计算值 ?       否 → 丢弃，peer_crc_err++
5. 解析 TYPE + PAYLOAD
```

---

## 3. 帧类型表

### 3.1 ESP32 → ASRPRO（A → E）

| TYPE | 名称 | 载荷 | 说明 |
|---|---|---|---|
| `0x01` | PING | 无 | 心跳，每 1 秒一次 |
| `0x02` | STAT | 无 | 查询状态（同 PING 回复） |
| `0x30` | ASK_TIME | 无 | 用户问时间，ASRPRO 播「现在是」+ 时间片段 |
| `0x31` | ASK_WEATHER | 无 | 用户问天气 |
| `0x32` | ALARM_SET | `[day][period][hour][minute]` | 闹钟设置 |
| `0x34` | ALARM_OFF | 无 | 取消闹钟 |
| `0x35` | ALARM_FULL | 无 | 两槽位已满，ASRPRO 播提示并进入取消确认 |
| `0x40` | TIMER_START | 无 | 正计时开始 |
| `0x41` | TIMER_STOP | 无 | 正计时停止，ESP32 播时长 |
| `0x42` | TIMER_CANCEL | 无 | 取消计时 |
| `0x43` | COUNTDOWN_SET | `[sec_hi][sec_lo]` | 倒计时，秒数大端 |
| `0x44` | COUNTDOWN_CANCEL | 无 | 取消倒计时 |

### 3.2 ASRPRO → ESP32（E → A）

| TYPE | 名称 | 载荷 | 说明 |
|---|---|---|---|
| `0x80` | PONG | `[status][rx_hi][rx_lo][crc_hi][crc_lo][power]` | 心跳回复 |
| `0x20` | SAY | 片段 ID 序列（16 位大端，`0xFFFF` 结束） | 对用户提问的回应（进 20 秒唤醒态） |
| `0x21` | ANNOUNCE | 同 SAY | 机器主动播报（**不进唤醒态**） |
| `0x10` | SLEEP | 无 | 进入低功耗 |
| `0x11` | WAKE | 无 | 退出低功耗 |
| `0x12` | STOP | 无 | 停止播放 |
| `0x13` | VOL | `[volume]` | 设置音量 0~100 |
| `0x14` | PLAY | `[index]` | 播放 TF 卡文件（数字编号） |

### 3.3 PONG 载荷

```
payload[0] = status      0x00 成功 / 0x02 失败
payload[1:2] = peer_rx    ASRPRO 累计收帧数（大端）
payload[3:4] = peer_crc   ASRPRO 累计 CRC 错帧数（大端）
payload[5] = power_mode  0x00 正常 / 0x01 待机 / 0x02 播放中
```

### 3.4 ALARM_SET 载荷

```
payload[0] = day     0=今天 1=明天 2=每天
payload[1] = period  0=早上 1=上午 2=下午 3=晚上 4=凌晨
payload[2] = hour    0~23（时段已折算）
payload[3] = minute  0/5/10/…/55
```

### 3.5 SAY / ANNOUNCE 载荷

```
payload[0:1] = id0   （16 位大端）
payload[2:3] = id1
...
payload[46:47] = id23
```

**结束标记**：`0xFFFF` 之后忽略。最多 24 个片段（48 字节 / 2）。

---

## 4. 时序

### 4.1 心跳

```
ESP32                          ASRPRO
  |---- PING (SEQ=n) ----------->|
  |<--- PONG (SEQ 回显, status) --|   RTT ≈ 195 ms
  |         (每 1 秒重复)
```

**超时**：单次等待 **500 ms**；连续 **3 次**超时 → `OFFLINE`。

### 4.2 状态机

```
INIT ──收到合法 PONG──► LINK ──连续 2 次成功──► READY
  ▲                                            │
  │                                    3 次超时│
  └──────── 收到 PONG ◄────────── OFFLINE ◄────┘
```

### 4.3 播报

```
ESP32 ---- T_SAY / T_ANNOUNCE (24 个 ID) ----> ASRPRO
                                              │
                                        逐段 PLAY
                                              │
ESP32 <---- PONG (先回包，不等播完) ----------┘
```

**关键**：ASRPRO 收到播报帧后**先回 PONG**，再播报，避免 ESP32 心跳超时。

---

## 5. 错误处理

| 场景 | 处理 |
|---|---|
| CRC 错 | 丢帧，`peer_crc_err++`（不回复） |
| LEN 不符 | 丢帧（V1.0 帧会被识别为非法） |
| 未知 TYPE | 忽略载荷，仍回 PONG（`status=0x00`） |
| 载荷不足 | 按缺省值处理（如 VOL 缺参则不调） |
| ASRPRO 忙（播报中） | 仍回 PONG（心跳不断） |
| ESP32 发送失败 | 记录 `uart write failed`，下次重试 |

---

## 6. 示例帧

### 6.1 PING

```
53 54 35 07 01 FF FF ... FF 12 DB 43 21
│  │  │  │  │  └─ payload: 48×FF（无载荷）
│  │  │  │  └─ TYPE = 0x01 (PING)
│  │  │  └─ SEQ = 0x07
│  │  └─ LEN = 0x35
│  └─ HEAD
└─ 共 57 字节
```

### 6.2 PONG

```
53 54 35 07 80 00 00 0A 00 00 00 FF ... FF xx xx 43 21
            │  │  └──┬──┘ └──┬──┘
            │  │     │       └─ power = 0x00 (NORMAL)
            │  │     └─ crc_err = 0
            │  └─ rx_frames = 10
            └─ TYPE = 0x80 (PONG)
```

### 6.3 播报 3 个片段（ID 79, 250, 30）

```
53 54 35 1A 21 00 4F 00 FA 00 1E FF FF ... FF xx xx 43 21
                     │  │  │  │  │  │
                     │  │  │  │  │  └─ id2 = 0x001E = 30
                     │  │  │  └──┴─ id1 = 0x00FA = 250
                     │  └──┴───────── id0 = 0x004F = 79
                     └─ TYPE = 0x21 (ANNOUNCE)
```

---

## 7. 版本兼容

| 版本 | 帧长 | LEN | 状态 |
|---|---|---|---|
| V1.0 | 9 字节 | 0x05 | 已废弃 |
| **V1.1** | **57 字节** | **0x35** | **当前** |

**识别方法**：看第 3 字节。`0x35` = V1.1，`0x05` = V1.0。

**诊断**：如果 ASRPRO 日志显示收到的帧是 `53 54 05 ...`（9 字节），说明 ESP32 烧了旧固件。

---

## 8. 调试要点

| 现象 | 检查 |
|---|---|
| 链路 OFFLINE 且无 `raw` | ASRPRO 没上电 / TX 线断 |
| 有 `raw` 但无 LINK | 收到数据但 CRC/LEN 不对（版本不匹配） |
| `peer[ok=N crc=M]` 中 crc 持续增长 | 波特率漂移 / 接线干扰 |
| RTT 突然变大（>1s） | ASRPRO 正在播报（正常） |

**双串口同时抓**：
- ESP32 COMx（115200）：`link: state=READY`、`peer sets alarm`
- ASRPRO Type-C（115200）：`RX: 53 54 35 ...`、`CHK n valid=1`、`PLAY n`
