#include "asr.h"
extern "C"{ void * __dso_handle = 0 ;}
#include "setup.h"
#include "HardwareSerial.h"
#include "asr_lu_mp3.h"

uint32_t snid;
ASR_LU_MP3 lxb_mp3_Serial1;
void app();
void ASR_CODE();

/* ===== 闹钟设置:时段 + 日期 + 「几点几分」,凑齐即上报 =====
 * 对话流程(日期可以省略,省略时 ASR 会反问):
 *   你  : 小爱同学
 *   ASR : 我在
 *   你  : 帮我定一个下午的闹钟              -> 只给时段
 *   ASR : 好的，是今天的、明天的、还是每天的？
 *   你  : 每天                              -> 日期补齐(0=今天 1=明天 2=每天)
 *   ASR : 好的，您想设几点几分的闹钟？
 *   你  : 三点五分                          -> 小时+分钟一口气说完,凑齐即上报
 *   ASR : 好的，每天下午三点五分的时候都会叫你
 *
 * 也可以一次性给全(组合词,少一轮问答):
 *   你  : 今天下午的闹钟 / 明天早上的闹钟
 *   ASR : 好的，您想设几点几分的闹钟？
 *
 * 分钟按 5 分钟挡位,共 12 档:整点档直接说「三点」,其余说「三点五分 / 三点十分 /
 * … / 三点五十五分」。所以「三点五分」说得通,「三点三分」识别不到 —— 小时和
 * 分钟合成在 ID 256~399 里一次性匹配,为的就是省掉"先说三点、ASR 再问几分、
 * 再说三分"那一轮。
 *
 * 「每天」= 重复闹钟(每天都会响);「今天/明天」= 只响一次。
 * 确认句由 ESP32 存好之后回 SAY 拼片段播,ASRPRO 不自己播 —— 因为"时间还没校准"
 * "要替换哪一个"这些情况只有 ESP32 知道。
 * 闹钟最多两个,两个都满了再设时 ESP32 会反问,你答"第一个"/"第二个"。
 *
 * 每种触发词都有两个口语别名,想说哪句都行:
 *   帮我定个下午的闹钟 / 下午定个闹钟            <- 等价于"下午的闹钟"
 *   帮我定个今天下午的闹钟 / 今天下午定个闹钟      <- 等价于"今天下午的闹钟"
 *
 * 【进入闹钟流程只有两种方式，其余一律不触发】
 *   A. 一次说全：今天下午的闹钟 / 明天早上的闹钟 …
 *   B. 只说时段：下午的闹钟 → 反问「好的，是今天的、明天的、还是每天的？」→ 你答日期
 *
 * 单独说「今天 / 明天 / 每天」【不】触发任何流程 —— 用户明确要求：
 * 只有“帮我定个 XX 的闹钟”这类明确说法才进入设置。日常闲聊、
 * 问天气时说的“今天”一律忽略，不播提示。
 * 同理，单独说「四点」也不会触发；时间词只在被问过
 * 「您想设几点几分的闹钟」之后的 15 秒窗口内才生效，窗口外静默忽略。
 * 「X点Y分」的 ASRTO 留空:确认句改由 ESP32 存好之后回 SAY 指令再播,
 * 这样"时间还没校准""今天已经过了""闹钟满了要替换哪个"这些情况才播得出对应提示。
 * 注意:播提示音期间麦克风是静音的(半双工),要等它说完再答下一句。
 */
int st_day    = -1;   /* -1=未定 0=今天 1=明天 2=每天(重复) */
int st_period = -1;   /* -1=未说 0=早上 1=上午 2=下午 3=晚上 4=凌晨 */
int st_hour   = -1;   /* 1~12 */
int st_min    = -1;   /* 0=整 5~55 */

/* 反问日期后暂存用户的选择 —— 已经废弃。
 * 现在日期直接写 st_day，不再中转：中转会让上一轮的"明天"残留到下一轮，
 * 用户没说日期也会被安排成明天。保留变量仅为兼容旧引用，值为 -1。 */
int st_pending_day = -1;

/* 确认播报音是 ID 135 那条命令词的回复音,但已经不由本文件播:
 * ESP32 存好闹钟后会回一条 SAY 指令决定说哪句 ——
 *   SAY 135      "好的，已经帮你设定好闹钟了"(正常)
 *   SAY 134      "时间还没校准"(没校时不收,避免假成功)
 *   SAY 162 135  "今天已经过了，帮你改到明天"(顺延了要说明) */

/* ===== 音量控制:0~100,模块默认 30 ===== */
#define TF_HINT_INDEX 0   /* 0=不播提示音。001~012 是整点报时、101 是闹钟铃声,别拿来当提示音 */
int g_volume = 30;

void vol_step(int d){
  g_volume += d;
  if (g_volume < 0)   g_volume = 0;
  if (g_volume > 100) g_volume = 100;
  lxb_mp3_Serial1.volume((uint8_t)g_volume);
  if (TF_HINT_INDEX > 0) {
    lxb_mp3_Serial1.playMp3Folder(0, TF_HINT_INDEX);
    lxb_mp3_Serial1.start();
  }
}

/* 识别完之后要接一句引导语,但不能在 ASR_CODE 里直接播 —— 交给 app 任务播,
 * 与 SAY 片段走同一套播放路径,避免在识别回调里阻塞。
 * 取值:0=无,163=问几点几分,164=请先说是上午还是下午的闹钟,
 *       400=请先说是今天的、明天的、还是每天的,
 *       401=好的，是今天的、明天的、还是每天的。 */
volatile int g_pending_ask = 0;

/* 时间词(256~399,"三点整"这类)的三道闸。
 * 实测:用户说完"帮我定个今天下午的闹钟"后就等着 —— 根本没说"三点",
 * 模块却识别出 SNID 280("三点")并直接上报,直接定了一个他从未同意的闹钟。
 * 这种词 ASRTO 是空的,识别到就发帧,中间没有任何确认环节,所以必须拦:
 *   ① 播报期间、以及播报刚停的一小段时间内,一律当成自己的回放回声丢掉;
 *   ② 时间词必须落在"刚问过几点"的窗口内才认 —— 窗口外冒出来的"三点"
 *      没有出处(环境音/隔壁说话),宁可不设闹钟。 */
volatile uint32_t g_echo_guard_until = 0;   /* 播报刚停,到这个时刻前不认时间词 */
volatile uint32_t g_time_window_until = 0;  /* "刚问过几点"的接受窗口截止时刻 */
/* 当前是否在播报。由 app 任务轮询 prompt_is_playing() 维护 ——
 * ASR_CODE 是识别回调,不在里面碰播放器状态,只读这个标志。 */
volatile bool g_playing = false;

/* 是否正在等用户回答"今天/明天/每天"。
 * 只有"下午的闹钟"这类只给时段的说法才会问这一句(401)。
 * 平时闲聊、问天气时单独说的"今天"一律不接 —— 用户明确要求：
 * 只有"帮我定个XX的闹钟"这种明确说法才进闹钟流程。 */
volatile bool g_asking_date = false;
#define ECHO_GUARD_MS   700
#define TIME_WINDOW_MS 15000

/* 开机问候的音频会被板载麦克风拾回来，可能触发一次假唤醒 ——
 * 这是物理回声，不通过改文案解决（用户要求保留原文）。
 * 注意：不要再加"开机 N 秒内屏蔽所有识别"这种闸，它会吞掉用户刚开机时
 * 真实说出的命令。真正导致取数据期间误播退下的根因在 T_SAY 分支：
 * 残帧凑不出片段（g_say_count=0）却无条件续唤醒态，已于 V1.2t 修掉。 */

/* 清空正在拼装的闹钟(四样都置无效) */
void st_reset(){
  st_day    = -1;
  st_period = -1;
  st_hour   = -1;
  st_min    = -1;
  g_asking_date = false;
}

/* 组合词入口:拿到"日期 + 时段",并清掉上一次残留的小时/分钟,
 * 保证接下来 ASR 问"几点的闹钟"时,用户重说的小时/分钟才是准的。
 * day < 0 表示这条词本身不带日期(如"下午的闹钟"):这时如果用户之前已经
 * 说过"今天/明天/每天",就从 st_pending_day 接上,不再问第二遍。 */
/* 定义在下面,但 st_begin 里可能要用(补齐最后一环时直接上报)。 */
void alarm_report();

void st_begin(int day, int period){
  g_asking_date = false;   /* 新开一轮,上一轮的"等日期"作废 */
  /* 不要再从 st_pending_day 接残值。以前 "下午的闹钟" 会把上一轮
   * 遗留下来的"明天"接上，导致用户明明没说日期，却被告知"明天下午…"。
   * 日期只能来自本轮：组合词自带的 day，或者反问后用户亲口答的 case 45/46/72。 */
  st_day    = day;
  st_period = period;
  st_hour = -1;
  st_min  = -1;
  /* 凑齐了就直接上报。用户分几轮说(今天 -> 三点五分 -> 下午的闹钟),
   * 补齐最后一环的这一刻就该出结果 —— 不能再甩一句反问让他白说一遍。 */
  if (st_day >= 0 && st_hour >= 0 && st_min >= 0) {
    alarm_report();
    return;
  }
  /* 还差小时/分钟，要反问一句。
   * day >= 0 是组合词(如"今天下午的闹钟"),自带日期+时段。这里本该由 ASRTO
   * 播"好的,您想设几点几分的闹钟?" —— 实测 ASRTO 不响(SNID 9 打印了但一点
   * 声音都没有,用户以为它没反应,直接又说了一遍),所以改成和日期词一样,
   * 由 app 任务按 g_pending_ask 播。
   * day < 0 是只说时段的词(40~44 / 2~6 / 34~38),那 15 条的 ASRTO 已留空,
   * 播什么由这里定。 */
  if (day < 0) {
    if (st_day < 0) {
      g_pending_ask = 401;
      g_asking_date = true;   /* 只有这种时候才接受用户回答的"今天/明天/每天" */
    } else {
      g_pending_ask = 163;
    }
  } else if (st_hour < 0 || st_min < 0) {
    g_pending_ask = 163;
  }
  /* 只要问出了"几点几分",就把应答窗口打开。放在这里而不是等 app 播完 ——
   * 用户可能抢在 app 轮到之前就答了,等播完才开窗就会把他的回答当噪音丢掉。 */
  if (g_pending_ask == 163) g_time_window_until = millis() + TIME_WINDOW_MS;
}

/* 日期 + 小时 + 分钟齐了就上报 ESP32(时段可缺省);上报后立刻清空,方便紧接着再设一个。
 * 时段换算:未说时段=12 小时值原样("八点"按 08:00 算);早上/上午=原值;
 * 下午 12 点=12、其余 +12;晚上 12 点=0、其余 +12;凌晨 12 点=0、其余原值。
 * 时段原样一起上报 —— ESP32 拼确认句时要按用户原话读回("早上"不能说成"上午")。
 * 确认句不在这里播 —— 理由见上面的 SAY 说明。 */

/* ===== 片段拼接播报(当前时间 / 今天天气) =====
 * 触发词的 ASRTO 先播一句引导语(时间:"现在时间是";天气:"今天<城市>天气"),
 * 同时通知 ESP32;ESP32 回一行
 *     SAY <片段ID> <片段ID> ...
 * 这里按序播放。片段是一批命令词,它们的 ASRTO 就是片段音频:
 *   时间用:120=点 121=分 122=整 123~132=零~九 133=十 134=时间还没校准
 *   闹钟用:135=好的，已经帮你设定好闹钟了 162=今天已经过了，帮你改到明天
 *          163=好的，您想设几点几分的闹钟？ 164=请先说是上午还是下午的闹钟
 *          75~88 = 确认句的拼接片段(每天/今天/明天/时段/…的时候都会叫你 等)
 *   城市用:165=天气(兜底) 166~195 = "<城市>天气"(共 30 个城市)
 *          ID 与 ESP32 的 k_city_fragments 表对应,由 IP 定位结果选中哪个
 *   天气用:136~155 共 20 种天气现象,按 WMO 代码细分(晴/晴间多云/多云/阴/有雾/
 *          毛毛雨/冻雨/小雨/中雨/大雨/暴雨/阵雨/中阵雨/阵雪/强阵雪/小雪/中雪/
 *          大雪/雷阵雨/雷阵雨伴冰雹)
 *          156=最高 157=最低 158=度 159=零下 160=天气还没更新 161=转(晴转多云)
 * 它们的 ASR 故意写成"播报音XX"这类不会被误触发的说法 —— ASR 只用于让工具
 * 生成对应的回复音,我们只用 reply(ASRTO)那一半。
 * 注意:不能在 play_done 回调里续播 —— 那个回调持有播放器互斥锁,会死锁;
 * 必须用 prompt_is_playing() 轮询,等上一段播完再播下一段。 */
/* 一条 SAY 最多带多少个片段。最长的是"闹钟满了":
 * 已经设了两个闹钟了(1) + 第一个是(1) + 闹钟1(最多9) + 第二个是(1) + 闹钟2(最多9)
 * + 要替换哪一个(1) = 22 段,取 24 留余量。ESP32 侧 asrpro.h 的 ASRPRO_SAY_MAX 要一致。 */
#define SAY_MAX 28
int g_say_ids[SAY_MAX];
int g_say_count = 0;

/* 等当前播报结束;单段最多等 6 s,避免异常情况下卡死 */
void say_wait_idle(){
  uint32_t guard = 0;
  while (prompt_is_playing() && guard < 600) { delay(10); guard++; }
}

void say_sequence(){
  for (int i = 0; i < g_say_count; i++) {
    /* 用"命令词 ID"播放它绑定的回复音(ASRTO 就是片段音频)。
     * 不用 prompt_play_by_voice_id + 自定义播报音编号那条路。
     * 注意:本文件任何位置(含注释)都不能出现播报音标记字样,否则工具会报格式错误。
     *
     * ret=1 有两个完全不同的原因,必须分开看:
     *   CHK valid=0 -> 命令表里查不到这个 ID(词表/校验号问题)
     *   CHK valid=1 -> handle 没问题,那就是 prompt_player.play_queue 为空
     *                   —— 待机态播放器没建起来,得先唤醒才能播。
     * 2026-10-20 整点报时全 ret=1(包括本来有音频的 79/133/120/122),
     * 就是靠这个 CHK 区分到底是哪一类。 */
    const uint16_t id = (uint16_t)g_say_ids[i];
    Serial.print("CHK ");
    Serial.print(id);
    Serial.print(" valid=");
    Serial.println((int)is_valid_cmd_handle(cmd_info_find_command_by_id(id)));

    const uint32_t r = prompt_play_by_cmd_id(id, -1, NULL, true);
    Serial.print("PLAY ");
    Serial.print(id);
    Serial.print(" ret=");
    Serial.println((int)r);
    say_wait_idle();
  }
  g_say_count = 0;
}

/* 解析 "SAY 10110 10120 ..." 并播放 */
void say_parse(const String &cmd){
  int n = 0;
  const int len = (int)cmd.length();
  int i = 0;
  while (i < len && cmd.charAt(i) != ' ') i++;          /* 跳过 "SAY" */
  while (i < len && n < SAY_MAX) {
    while (i < len && cmd.charAt(i) == ' ') i++;
    int v = 0, digits = 0;
    while (i < len && cmd.charAt(i) >= '0' && cmd.charAt(i) <= '9') {
      v = v * 10 + (cmd.charAt(i) - '0');
      i++; digits++;
    }
    if (digits == 0) break;
    if (v > 0 && v <= 65535) g_say_ids[n++] = v;
  }
  g_say_count = n;
  if (n <= 0) return;
  say_wait_idle();   /* 先等触发词的 ASRTO("现在时间是")播完,避免被截断 */
  say_sequence();
}

/* 文案保持原样，不改。
 * 开机问候的回声确实可能触发假唤醒，但解决手段不是改字，
 * 也不是开机窗口屏蔽 —— 见文件上方 T_SAY 残帧那条说明。 */
//{speak:小蝶-清新女声,vol:10,speed:14,platform:haohaodada}
//{playid:10001,voice:欢迎使用语音助手，用小爱同学唤醒我。}
//{playid:10002,voice:我退下了，用小爱同学唤醒我}

/* 串口2(PA5/PA6)接收线程：ESP32 发来命令 -> 控制 MP3 */
/* ============================================================================
 * 链路层（阶段 1）：二进制帧，与 ESP32 侧 docs/protocol 文档一致
 *   请求 12B: 53 54 0C | 时间戳4B | 指令1B | CRC2B | 43 21
 *   回包 21B: 53 54 15 | 时间戳4B | 状态1B | 收帧2B | CRC错2B
 *             | PONG数4B | 功耗状态1B | CRC2B | 43 21
 * ========================================================================== */
extern "C" int power_mode_switch(int mode);

#define PM_NORMAL   0
#define PM_LIGHT    1

/* ===== V1.1 变长帧 =====
 *   53 54 | LEN | SEQ | TYPE | PAYLOAD(N) | CRC_L CRC_H | 43 21
 *   LEN     = 3 + N + 2 = N + 5   (SEQ..CRC 的字节数)
 *   总长度  = N + 9,最小 9 字节
 *   CRC16-Modbus 覆盖 LEN..PAYLOAD 末字节
 * 相比 V1.0 的 12/21 定长帧,SAY 能塞下最多 28 个片段 ID。
 */
#define FRAME_MAX     64
#define PAYLOAD_MAX   48

/* ---- A -> E : ASRPRO 上报 ---- */
#define T_PING         0x01
#define T_STAT         0x02
#define T_ASK_TIME     0x30   /* 用户问时间,ESP32 回 SAY 片段序列 */
#define T_ASK_WEATHER  0x31   /* 用户问天气 */
#define T_ALARM_SET    0x32   /* 载荷: day period hour min */
#define T_ALARM_PICK   0x33   /* 载荷: slot(1/2) */
#define T_ALARM_OFF    0x34

/* ---- E -> A : ESP32 下发 ---- */
#define T_PONG         0x80
#define T_SAY          0x20   /* 载荷: 片段ID(2B 大端) × N，对用户提问的回应 */
/* ESP32 主动播报(整点报时/开机问候)。载荷和 T_SAY 完全相同，
 * 区别只在处理方式：它不调 set_state_enter_wakeup ——
 * 用户没说话，机器自己开口，不能再进 20 秒唤醒态，
 * 否则每次报时完 20 秒都会冒出一句"我退下了"。 */
#define T_ANNOUNCE     0x21
#define T_SLEEP        0x10
#define T_WAKE         0x11
#define T_STOP         0x12
#define T_VOL          0x13
#define T_PLAY         0x14

#define PWR_NORMAL   0x00
#define PWR_LIGHT    0x01
#define PWR_PLAYING  0x02

static const uint8_t kHead[2] = { 0x53, 0x54 };
static const uint8_t kTail[2] = { 0x43, 0x21 };

static uint8_t  g_pwr       = PWR_NORMAL;
static uint16_t g_rx_frames = 0;
static uint16_t g_crc_err   = 0;
static uint32_t g_pong_sent = 0;
static uint8_t  g_seq       = 0;

static uint16_t crc16_modbus(const uint8_t *p, int n)
{
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < n; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) {
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
    }
  }
  return crc;
}

/* 通用发帧:type + 载荷(可为 NULL)。返回实际写出的字节数。 */
static int send_frame(uint8_t type, const uint8_t *pl, uint8_t n)
{
  if (n > PAYLOAD_MAX) n = PAYLOAD_MAX;

  uint8_t f[FRAME_MAX];
  f[0] = kHead[0];
  f[1] = kHead[1];
  f[2] = (uint8_t)(n + 5);       /* LEN */
  f[3] = g_seq++;                /* SEQ */
  f[4] = type;
  for (uint8_t i = 0; i < n; i++) f[5 + i] = pl[i];

  const int body = n + 3;        /* LEN..payload 共 body 字节 */
  const uint16_t c = crc16_modbus(f + 2, body);
  f[5 + n] = (uint8_t)(c & 0xFF);
  f[6 + n] = (uint8_t)((c >> 8) & 0xFF);
  f[7 + n] = kTail[0];
  f[8 + n] = kTail[1];

  const int total = n + 9;
  Serial2.write(f, total);
  return total;
}

/* ===== 延迟发送(当前未启用) =====
 * 二进制链路比 9 月的 ASCII 行协议快得多:ASR_CODE 里一 send_frame,ESP32 几毫秒
 * 就回了 T_SAY,这是当初怀疑"ASRTO 被顶掉"的原因。但实测延迟 1.5s 之后
 * "现在时间是"依然没声 —— 说明触发词的回复音本身就没进固件,不是时序问题。
 * 这段机制留着:调成非 0 后可以让某条指令错开 ASRTO 播放。 */
#define DEFER_MS 0
static volatile uint8_t  g_defer_type = 0;
static volatile uint32_t g_defer_at   = 0;

static void defer_request(uint8_t type){
  g_defer_type = type;
  g_defer_at   = millis() + DEFER_MS;
}

void alarm_report(){
  if (st_day < 0 || st_hour < 0 || st_min < 0) return;
  int h = st_hour;
  if (st_period == 2)      h = (st_hour == 12) ? 12 : st_hour + 12;   /* 下午 */
  else if (st_period == 3) h = (st_hour == 12) ? 0  : st_hour + 12;   /* 晚上 */
  else if (st_period == 4) h = (st_hour == 12) ? 0  : st_hour;        /* 凌晨 */

  /* T_ALARM_SET 载荷: day(1) period(1) hour(1) min(1)
   * period 用 0xFF 表示"没说"(ESP32 侧按 24 小时制的 hour 处理即可)。 */
  uint8_t pl[4];
  pl[0] = (uint8_t)st_day;
  pl[1] = (st_period < 0) ? 0xFF : (uint8_t)st_period;
  pl[2] = (uint8_t)h;
  pl[3] = (uint8_t)st_min;
  send_frame(T_ALARM_SET, pl, 4);

  st_reset();
}

/* PONG: status + 收帧数 + CRC 错误数 + 功耗状态 */
static void send_response(uint8_t seq, uint8_t status)
{
  uint8_t pl[6];
  pl[0] = status;
  pl[1] = (uint8_t)(g_rx_frames >> 8); pl[2] = (uint8_t)(g_rx_frames);
  pl[3] = (uint8_t)(g_crc_err   >> 8); pl[4] = (uint8_t)(g_crc_err);
  pl[5] = g_pwr;
  (void)seq;                      /* 回包里回显 SEQ 更有用,放 pl 前不好排,先不回声 */
  send_frame(T_PONG, pl, 6);
  g_pong_sent++;
}

static void handle_request(const uint8_t *f)
{
  /* V1.1 变长帧布局:
   *   [0][1] 帧头 53 54   [2] LEN   [3] SEQ   [4] TYPE
   *   [5 .. 4+n] PAYLOAD   [5+n][6+n] CRC   [7+n][8+n] 帧尾 43 21
   *   n = LEN - 5,总长度 = n + 10 */
  const uint8_t n    = (uint8_t)(f[2] - 5);
  const uint8_t seq  = f[3];
  const uint8_t type = f[4];

  if (f[7 + n] != kTail[0] || f[8 + n] != kTail[1]) { g_crc_err++; return; }
  const uint16_t want = (uint16_t)f[5 + n] | ((uint16_t)f[6 + n] << 8);
  if (want != crc16_modbus(f + 2, n + 3)) { g_crc_err++; return; }

  g_rx_frames++;

  switch (type) {
    case T_PING:
    case T_STAT:
      send_response(seq, 0x00);
      break;

    case T_SLEEP:
      if (g_pwr != PWR_PLAYING) { power_mode_switch(PM_LIGHT); g_pwr = PWR_LIGHT; }
      send_response(seq, 0x00);
      break;

    case T_WAKE:
      power_mode_switch(PM_NORMAL);
      g_pwr = PWR_NORMAL;
      send_response(seq, 0x00);
      break;

    case T_STOP:
      lxb_mp3_Serial1.stop();
      send_response(seq, 0x00);
      break;

    case T_VOL:
      if (n >= 1) {
        int v = f[5];
        if (v > 100) v = 100;
        lxb_mp3_Serial1.volume((uint8_t)v);
      }
      send_response(seq, 0x00);
      break;

    case T_PLAY:
      if (n >= 1 && f[5] > 0) {
        lxb_mp3_Serial1.playMp3Folder(0, f[5]);
        lxb_mp3_Serial1.start();
      }
      send_response(seq, 0x00);
      break;

    case T_SAY:
    case T_ANNOUNCE: {
      /* 两者载荷完全一样: N 个 16 位大端片段 ID,交给 say_sequence() 逐段播。 */
      const uint8_t cnt = (uint8_t)(n / 2);
      g_say_count = 0;
      for (uint8_t i = 0; i < cnt && g_say_count < SAY_MAX; i++) {
        g_say_ids[g_say_count++] = ((int)f[5 + i * 2] << 8) | f[6 + i * 2];
      }
      send_response(seq, 0x00);        /* 先回包,避免播放拖超时 */

      if (type == T_ANNOUNCE) {
        /* 主动播报(整点报时/开机问候)要满足两件事:
         *
         * 1) 切到命令词模型组(0)。
         *    待机时 SDK 停在组 1(里面只有唤醒词),79/133/120/122 全查不到
         *    —— 实测 CHK valid=0、PLAY ret=1。
         *
         * 2) 绝对不碰状态机。
         *    曾经在这里调 set_state_enter_wakeup(3000), 3 秒后
         *    exit_wakeup_deal() 发现在唤醒态就去播 <inactivate>,
         *    结果整点报时变成了「我退下了,用小爱同学唤醒我」。
         *    播放队列是 prompt_play_by_cmd_handle 里懒创建的,不需要唤醒。 */
        cmd_info_change_cur_model_group(0);
        delay(20);
      } else if (g_say_count > 0) {
        /* 只有真的收到可播片段，才续唤醒态。
         * 以前这一支是无条件的 —— 只要收到一帧 CMD=0x20 就续 20 秒，
         * 哪怕 payload 凑不出一个完整片段。
         * 实测抓到过这种帧：
         *   14:24:13  RX: 53 54 05 0A 01 A6 A1 43 21   ← 正常心跳 CMD=01
         *   14:24:14  RX: 53 54 05 0B 20 67 29 43 21   ← CMD 变成 20，payload 只有 1 字节
         *   14:24:15  RX: 53 54 05 0D 01 A4 91 43 21   ← 又回到 01
         * 这条帧没有任何片段可播，却让模块凭空进了 20 秒唤醒态，
         * 20 秒后正是“我退下了”。改成只在有片段时续，就从根上没了源。 */
        set_state_enter_wakeup(20000);
      }

      if (g_say_count > 0) {
        say_wait_idle();
        say_sequence();
      }

      /* 播完把命令表恢复成唤醒词组,保证"小爱同学"随时能识别。 */
      if (type == T_ANNOUNCE) cmd_info_change_cur_model_group(1);
      break;
    }

    default:
      send_response(seq, 0x02);
      break;
  }
}


void app(){
  uint8_t buf[FRAME_MAX];
  int len = 0;
  bool s_was_playing = false;

  while (1) {
    /* 播报结束的沿:记下来,给时间词留 700 ms 静默期。
     * ASRTO 是 SDK 自动播的,拿不到它的回调,只能轮询 prompt_is_playing()。 */
    const bool playing = prompt_is_playing();
    g_playing = playing;
    if (s_was_playing && !playing) {
      g_echo_guard_until = millis() + ECHO_GUARD_MS;
    }
    s_was_playing = playing;

    /* 引导语:识别到"每天"/"今天"这类还得补条件的词时,ASR_CODE 只置
     * g_pending_ask,真正的播报放在这里 —— 不在识别回调里阻塞。
     * 之前这个标志只赋值、从来没人读,所以说完"每天"一点声音都没有。 */
    if (g_pending_ask != 0) {
      const int ask = g_pending_ask;
      g_pending_ask = 0;
      say_wait_idle();
      prompt_play_by_cmd_id((uint16_t)ask, -1, NULL, true);
      g_playing = true;   /* 立即置位，不等下一轮轮询（否则这段间隙里闸门是开的） */
      /* 刚问完“几点几分”,把应答窗口再续满 —— 静默期的起点算在播完后。 */
      if (ask == 163) g_time_window_until = millis() + TIME_WINDOW_MS;
    }

    /* 延迟发送:等触发词的 ASRTO 播完,再把业务请求发出去。
     * 放在循环开头 —— 下面的帧解析里有 continue 分支,放末尾会被跳过。 */
    if (g_defer_type != 0 && (int32_t)(millis() - g_defer_at) >= 0) {
      const uint8_t t = g_defer_type;
      g_defer_type = 0;
      send_frame(t, NULL, 0);
    }

    if (Serial2.available() > 0) {
      const uint8_t b = (uint8_t)Serial2.read();

      if (len == 0 && b != kHead[0]) continue;
      if (len == 1 && b != kHead[1]) { len = 0; continue; }

      if (len >= FRAME_MAX) len = 0;
      buf[len++] = b;

      /* 收到第 3 字节才有 LEN;总长度 = 2(帧头) + 1(LEN) + LEN + 2(帧尾) = LEN + 4 */
      if (len >= 3) {
        const int total = (int)buf[2] + 4;
        if (total < 9 || total > FRAME_MAX) { len = 0; continue; }
        if (len >= total) {
          Serial.print("RX:");
          for (int i = 0; i < total; i++) {
            Serial.print(' ');
            if (buf[i] < 0x10) Serial.print('0');
            Serial.print(buf[i], HEX);
          }
          Serial.println();
          handle_request(buf);
          len = 0;
        }
      }
    }
    delay(2);
  }
  vTaskDelete(NULL);
}

void ASR_CODE(){
  /* 降频待机时被唤醒词唤醒 -> 本地自主升频，不等 ESP32 下命令。
   * 升频结果在下一次 PONG 的功耗字段回传给 ESP32。 */
  if (snid == 0 && g_pwr == PWR_LIGHT) {
    power_mode_switch(PM_NORMAL);
    g_pwr = PWR_NORMAL;
  }

  /* 注意:这里【不能】再调 Serial.begin()。
   * 9 月能跑的 asr_dialog.cpp 在 ASR_CODE 里没有任何串口操作;我调试时加了
   * Serial.begin(115200) + "HIT" 打印,每次识别都重置一次 UART0(日志口,
   * PB5/PB6),会打断 SDK 的唤醒状态机计时,表现为"我退下了"被播两次。
   * 需要看识别结果时,用 hardware_init 里已经初始化好的 Serial 直接 print 即可。 */
  Serial.print("SNID ");
  Serial.println((int)snid);

  set_state_enter_wakeup(20000);   /* 唤醒后 20s 内可连续说,每次识别会自动续期 */

  /* ---- 小时+分钟一起说:ID 256~399 = 12 小时 × 12 个 5 分钟挡位 ----
   * 用户听到「您想设几点几分的闹钟？」后一口气答「三点五分」,一轮搞定。
   * ID = 256 + 小时下标 × 12 + 分钟下标:
   *   小时下标 0~11 = 一~十二点;分钟下标 0~11 = 整/五分/十分/…/五十五分。
   * 5 分钟挡位下,说"三分/十三分"这类非挡位分钟识别不到;约束解码也有可能
   * 把它匹配到最近的挡位(说"十三分"听成"十五分")—— 兜底是 ESP32 存好后
   * 把完整时间读回一遍,用户听到不对可以说"取消闹钟"重设。 ---- */
  if (snid >= 256 && snid <= 399) {
    /* 三道闸。这类词 ASRTO 是空的，识别到就直接上报，中间没有确认环节：
     * ① 播报期间不认：半双工，用户这时本来就在听，不可能是他在说时间。
     * ② 报完再缓 700 ms：尾音还没消，麦克风会把自己的回放拾回去。
     * ③ 只在“刚问过几点几分”的 15 秒窗口内认。
     *    窗口外一律忽略、不播任何提示 —— 用户明确要求：
     *    单独说个“四点”不该把设备拖进设闹钟流程。
     *    窗口只在问出 163「您想设几点几分」时打开，所以能走到这里的
     *    一定是在回答一个真的在进行的闹钟设置。 */
    if (g_playing) return;
    if ((int32_t)(millis() - g_echo_guard_until) < 0) return;
    if ((int32_t)(millis() - g_time_window_until) >= 0) return;

    const int idx = (int)snid - 256;
    st_hour = idx / 12 + 1;        /* 1~12 */
    st_min  = (idx % 12) * 5;      /* 0,5,10,…,55 */
    if (st_day < 0) return;        /* 理论上到不了：163 只在日期已定时才问 */
    alarm_report();
    return;
  }

  switch (snid) {
   case 1:
    lxb_mp3_Serial1.playMp3Folder(0,1);
    lxb_mp3_Serial1.start();
    break;

   /* ---- 问时间:12 小时制 + 时段(上午/下午/晚上),片段由 ESP32 拼 ---- */
   case 30:                                  // 现在几点
   case 31:                                  // 几点了
    defer_request(T_ASK_TIME);
    break;

   /* ---- 问天气:ASRTO 先播"今天",同样延迟后再取片段 ---- */
   case 32:                                  // 今天天气
   case 33:                                  // 天气怎么样
    defer_request(T_ASK_WEATHER);
    break;

   /* ---- 组合词:一次拿到"日期 + 时段";ASRTO 会自动问"几点的闹钟" ----
    * "早上"为主词、"上午"为别名;两者都映射到同一个时段值。 */
   case 20: st_begin(0, 0); break;   // 今天早上
   case 21: st_begin(0, 1); break;   // 今天上午
   case 22: st_begin(0, 2); break;   // 今天下午
   case 23: st_begin(0, 3); break;   // 今天晚上
   case 24: st_begin(0, 4); break;   // 今天凌晨
   case 25: st_begin(1, 0); break;   // 明天早上
   case 26: st_begin(1, 1); break;   // 明天上午
   case 27: st_begin(1, 2); break;   // 明天下午
   case 28: st_begin(1, 3); break;   // 明天晚上
   case 29: st_begin(1, 4); break;   // 明天凌晨

   /* ---- 时段词(不含日期):只说"下午的闹钟",ASRTO 会自动反问日期 ---- */
   case 40: st_begin(-1, 0); break;   // 早上的闹钟
   case 41: st_begin(-1, 1); break;   // 上午的闹钟
   case 42: st_begin(-1, 2); break;   // 下午的闹钟
   case 43: st_begin(-1, 3); break;   // 晚上的闹钟
   case 44: st_begin(-1, 4); break;   // 凌晨的闹钟

   /* ---- 日期词：只是上面那句反问的回答，不单独开场 ----
    * 用户明确要求：单独说“今天/明天/每天”不该进闹钟流程。
    * 所以只有刚被问过 401「是今天的、明天的、还是每天的」时才接，
    * 其余情况直接忽略、不播任何提示。72 = “每天” -> st_day 用 2 表示。 ---- */
   case 45:
   case 46:
   case 72:
    if (!g_asking_date) break;
    g_asking_date = false;
    st_day = (snid == 46) ? 1 : ((snid == 72) ? 2 : 0);
    if (st_hour >= 0 && st_min >= 0) {
      alarm_report();
      break;
    }
    st_hour = -1;
    st_min  = -1;
    g_pending_ask = 163;     // 好的，您想设几点几分的闹钟？
    g_time_window_until = millis() + TIME_WINDOW_MS;
    break;

   /* ---- 闹钟已满时,回答"要替换哪一个" ---- */
   case 73: { uint8_t s = 1; send_frame(T_ALARM_PICK, &s, 1); break; }   // 第一个
   case 74: { uint8_t s = 2; send_frame(T_ALARM_PICK, &s, 1); break; }   // 第二个

   /* ---- 口语别名 ①:「帮我定个〈时段〉的闹钟」,与 40~44 完全等价 ---- */
   case 2:  st_begin(-1, 0); break;   // 帮我定个早上的闹钟
   case 3:  st_begin(-1, 1); break;   // 帮我定个上午的闹钟
   case 4:  st_begin(-1, 2); break;   // 帮我定个下午的闹钟
   case 5:  st_begin(-1, 3); break;   // 帮我定个晚上的闹钟
   case 6:  st_begin(-1, 4); break;   // 帮我定个凌晨的闹钟
   /* ---- 口语别名 ①:「帮我定个〈日期+时段〉的闹钟」,与 20~29 完全等价 ---- */
   case 7:  st_begin(0, 0); break;    // 帮我定个今天早上的闹钟
   case 8:  st_begin(0, 1); break;    // 帮我定个今天上午的闹钟
   case 9:  st_begin(0, 2); break;    // 帮我定个今天下午的闹钟
   case 10: st_begin(0, 3); break;    // 帮我定个今天晚上的闹钟
   case 11: st_begin(0, 4); break;    // 帮我定个今天凌晨的闹钟
   case 12: st_begin(1, 0); break;    // 帮我定个明天早上的闹钟
   case 13: st_begin(1, 1); break;    // 帮我定个明天上午的闹钟
   case 14: st_begin(1, 2); break;    // 帮我定个明天下午的闹钟
   case 15: st_begin(1, 3); break;    // 帮我定个明天晚上的闹钟
   case 16: st_begin(1, 4); break;    // 帮我定个明天凌晨的闹钟

   /* ---- 口语别名 ②:「〈时段〉定个闹钟」(动词后置),与 40~44 等价 ---- */
   case 34: st_begin(-1, 0); break;   // 早上定个闹钟
   case 35: st_begin(-1, 1); break;   // 上午定个闹钟
   case 36: st_begin(-1, 2); break;   // 下午定个闹钟
   case 37: st_begin(-1, 3); break;   // 晚上定个闹钟
   case 38: st_begin(-1, 4); break;   // 凌晨定个闹钟
   /* ---- 口语别名 ②:「〈日期+时段〉定个闹钟」,与 20~29 等价 ---- */
   case 39: st_begin(0, 0); break;    // 今天早上定个闹钟
   case 47: st_begin(0, 1); break;    // 今天上午定个闹钟
   case 48: st_begin(0, 2); break;    // 今天下午定个闹钟
   case 49: st_begin(0, 3); break;    // 今天晚上定个闹钟
   case 50: st_begin(0, 4); break;    // 今天凌晨定个闹钟
   case 51: st_begin(1, 0); break;    // 明天早上定个闹钟
   case 52: st_begin(1, 1); break;    // 明天上午定个闹钟
   case 53: st_begin(1, 2); break;    // 明天下午定个闹钟
   case 54: st_begin(1, 3); break;    // 明天晚上定个闹钟
   case 55: st_begin(1, 4); break;    // 明天凌晨定个闹钟

   /* ---- 小时:已并进上面的 256~399 范围(小时+分钟一起说),这里不再单独处理 ---- */

   /* ---- 关闭闹钟:"取消闹钟"是别名,映射到同一处理 ---- */
   case 242:
   case 243:
    st_reset();
    /* 以前这里只清本地状态,忘了发帧 —— 用户说"取消闹钟"、ASRTO 也回了
     * "闹钟已关闭",但 ESP32 根本不知道,闹钟照样会响。 */
    send_frame(T_ALARM_OFF, NULL, 0);
    break;

   /* ---- 音量加/减(每次 10):"大声点/小声点"为主词,"音量加/减"为别名 ---- */
   case 250: vol_step(+10); break;   // 大声点
   case 251: vol_step(-10); break;   // 小声点
   case 252: vol_step(+10); break;   // 音量加
   case 253: vol_step(-10); break;   // 音量减
  }
}

void hardware_init(){
  /* 这一行必须有。去掉它的那次实验里 SSCOM 全是乱码 —— 说明 UART0 的
   * 115200 就是靠它设的，SDK 没有替我们初始化。
   * （不要去怀疑“它和 SDK 唤醒状态机抢 UART0”：真正有害的是在 ASR_CODE
   *  里反复 begin，而不是在 hardware_init 里设一次。） */
  Serial.begin(115200);
  Serial.println("INIT OK");
  /* 版本戳:每次改完固件把这里改成对应版本,烧录后在 SSCOM 里核对。
   * 看到这行 == 芯片里跑的就是当前这份源码。 */
  Serial.println("BUILD V1.2x 1120L no-day-leak");
  /* ===== 以下严格照抄 tmp/asr_dialog.cpp(9 月 22 日跑通的版本) =====
   * 之前我在这一段加了 THIRD_FUNCTION / scu_set_device_gate / dpmu_set_io_pull
   * / begin 后重复 setPinFun，全都去掉了 —— 那些是我的推测，不是验证过的代码。
   * 9 月能跑的就是这 6 行，先原样恢复。 */
  setPinFun(2,FORTH_FUNCTION);
  setPinFun(3,FORTH_FUNCTION);
  Serial1.begin(115200);
  setPinFun(5,FORTH_FUNCTION);
  setPinFun(6,FORTH_FUNCTION);
  /* 9600 而不是 115200:实测 115200 下前 7 字节正确、第 8 字节开始漂移
   * (ESP32 发 cmd=0x01,ASRPRO 收到 0x08),CRC 必然失败。
   * 9600 对时钟误差容忍度是 115200 的 12 倍,先在这个速率上把链路跑通,
   * 需要提速时再逐级往上试 19200 / 38400 / 57600。 */
  Serial2.begin(9600);
  /* ===== 以下是链路测试用的自检，不影响上面的初始化 ===== */
  {
    const size_t wn = Serial2.write((const uint8_t *)"PA5PA6", 6);
    Serial.print("PA5 TX attempted, bytes=");
    Serial.print((int)wn);
    Serial.println(" (return value is fake, check ESP32 side)");
  }
  delay(10);
  /* ===== 命令表自检 =====
   * prompt_play_by_cmd_id(id) 内部靠 cmd_info_find_command_by_id(id) 反查句柄。
   * 查不到时它直接返回 1、一声不响 —— 这正是"只有九点四十分、没有现在时间是"
   * 最可疑的原因(120~133 能播，30 播不出来)。
   * valid=1 说明命令表里确实有这个大 ID；valid=0 说明天问Block 里没生成。 */
  {
    const uint16_t probe[] = {0, 1, 30, 31, 32, 33, 120, 121, 131, 133, 165};
    for (uint32_t i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
      Serial.print("CMDTBL ");
      Serial.print(probe[i]);
      Serial.print(" valid=");
      Serial.println((int)is_valid_cmd_handle(cmd_info_find_command_by_id(probe[i])));
    }
  }
  lxb_mp3_Serial1.begin(&Serial1,false);
  lxb_mp3_Serial1.reset();
  /* app 任务要跑 SAY 片段播报(内部还会进 prompt_player),128 words 太紧,调大到 512。 */
  xTaskCreate(app,"app",512,NULL,4,NULL);
  vTaskDelete(NULL);
}

void setup()
{
  //{ID:0,keyword:"唤醒词",ASR:"小爱同学",ASRTO:"我在"}
  //{ID:1,keyword:"命令词",ASR:"播放音乐",ASRTO:"好的"}
  //{ID:30,keyword:"命令词",ASR:"现在几点",ASRTO:"现在时间是"}
  //{ID:31,keyword:"命令词",ASR:"几点了",ASRTO:"现在时间是"}
  /* ---- 天气触发词:只播"今天",具体地名由 ESP32 按 IP 定位结果选城市片段拼上来 ---- */
  //{ID:32,keyword:"命令词",ASR:"今天天气",ASRTO:"今天"}
  //{ID:33,keyword:"命令词",ASR:"天气怎么样",ASRTO:"今天"}
  /* ---- 时段词:不含日期,回复句会自动反问日期 ---- */
  //{ID:40,keyword:"命令词",ASR:"早上的闹钟",ASRTO:""}
  //{ID:41,keyword:"命令词",ASR:"上午的闹钟",ASRTO:""}
  //{ID:42,keyword:"命令词",ASR:"下午的闹钟",ASRTO:""}
  //{ID:43,keyword:"命令词",ASR:"晚上的闹钟",ASRTO:""}
  //{ID:44,keyword:"命令词",ASR:"凌晨的闹钟",ASRTO:""}
  /* ---- 日期词:只用来回答上面那句反问,不能单独开场。
   * 回复句留空 —— 引导语改由 app 任务按状态播(没说时段时要拒绝,见 g_pending_ask)。 ---- */
  //{ID:45,keyword:"命令词",ASR:"今天",ASRTO:""}
  //{ID:46,keyword:"命令词",ASR:"明天",ASRTO:""}
  /* ---- 重复模式 + 替换选择(闹钟两个槽位用满时反问"要替换哪一个") ---- */
  //{ID:72,keyword:"命令词",ASR:"每天",ASRTO:""}
  //{ID:73,keyword:"命令词",ASR:"第一个",ASRTO:""}
  //{ID:74,keyword:"命令词",ASR:"第二个",ASRTO:""}
  /* ---- 以下 257~270 是"设好闹钟后读回时间"的确认句片段,不是给用户说的 ----
   * 确认句拼法:重复模式 + 时段 + 时 + 点 + 分 + 分 + 结尾
   *   每天  : 每天 + 下午 + 三 + 点 + 三 + 分 + 的时候都会叫你
   *   一次性: 今天 + 下午 + 三 + 点 + 三 + 分 + 的时候叫你一次
   * 时段片段顺序必须与 st_period 编码一致:0=早上 1=上午 2=下午 3=晚上 4=凌晨 */
  //{ID:75,keyword:"命令词",ASR:"播报音每天",ASRTO:"每天"}
  //{ID:76,keyword:"命令词",ASR:"播报音今天",ASRTO:"今天"}
  //{ID:77,keyword:"命令词",ASR:"播报音明天",ASRTO:"明天"}
  //{ID:78,keyword:"命令词",ASR:"播报音早上",ASRTO:"早上"}
  //{ID:79,keyword:"命令词",ASR:"播报音上午",ASRTO:"上午"}
  //{ID:80,keyword:"命令词",ASR:"播报音下午",ASRTO:"下午"}
  //{ID:81,keyword:"命令词",ASR:"播报音晚上",ASRTO:"晚上"}
  //{ID:82,keyword:"命令词",ASR:"播报音凌晨",ASRTO:"凌晨"}
  //{ID:83,keyword:"命令词",ASR:"播报音的时候都会叫你",ASRTO:"的时候都会叫你"}
  //{ID:84,keyword:"命令词",ASR:"播报音的时候叫你一次",ASRTO:"的时候叫你一次"}
  //{ID:85,keyword:"命令词",ASR:"播报音已经设了两个闹钟了",ASRTO:"已经设了两个闹钟了"}
  //{ID:86,keyword:"命令词",ASR:"播报音第一个是",ASRTO:"第一个是"}
  //{ID:87,keyword:"命令词",ASR:"播报音第二个是",ASRTO:"第二个是"}
  //{ID:88,keyword:"命令词",ASR:"播报音要替换哪一个",ASRTO:"要替换哪一个？"}
  /* ---- 口语别名①:「帮我定个X的闹钟」(与 40~44 / 20~29 等价) ---- */
  //{ID:2,keyword:"命令词",ASR:"帮我定个早上的闹钟",ASRTO:""}
  //{ID:3,keyword:"命令词",ASR:"帮我定个上午的闹钟",ASRTO:""}
  //{ID:4,keyword:"命令词",ASR:"帮我定个下午的闹钟",ASRTO:""}
  //{ID:5,keyword:"命令词",ASR:"帮我定个晚上的闹钟",ASRTO:""}
  //{ID:6,keyword:"命令词",ASR:"帮我定个凌晨的闹钟",ASRTO:""}
  //{ID:7,keyword:"命令词",ASR:"帮我定个今天早上的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:8,keyword:"命令词",ASR:"帮我定个今天上午的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:9,keyword:"命令词",ASR:"帮我定个今天下午的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:10,keyword:"命令词",ASR:"帮我定个今天晚上的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:11,keyword:"命令词",ASR:"帮我定个今天凌晨的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:12,keyword:"命令词",ASR:"帮我定个明天早上的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:13,keyword:"命令词",ASR:"帮我定个明天上午的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:14,keyword:"命令词",ASR:"帮我定个明天下午的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:15,keyword:"命令词",ASR:"帮我定个明天晚上的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:16,keyword:"命令词",ASR:"帮我定个明天凌晨的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  /* ---- 口语别名②:「X定个闹钟」(动词后置,与 40~44 / 20~29 等价) ---- */
  //{ID:34,keyword:"命令词",ASR:"早上定个闹钟",ASRTO:""}
  //{ID:35,keyword:"命令词",ASR:"上午定个闹钟",ASRTO:""}
  //{ID:36,keyword:"命令词",ASR:"下午定个闹钟",ASRTO:""}
  //{ID:37,keyword:"命令词",ASR:"晚上定个闹钟",ASRTO:""}
  //{ID:38,keyword:"命令词",ASR:"凌晨定个闹钟",ASRTO:""}
  //{ID:39,keyword:"命令词",ASR:"今天早上定个闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:47,keyword:"命令词",ASR:"今天上午定个闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:48,keyword:"命令词",ASR:"今天下午定个闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:49,keyword:"命令词",ASR:"今天晚上定个闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:50,keyword:"命令词",ASR:"今天凌晨定个闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:51,keyword:"命令词",ASR:"明天早上定个闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:52,keyword:"命令词",ASR:"明天上午定个闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:53,keyword:"命令词",ASR:"明天下午定个闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:54,keyword:"命令词",ASR:"明天晚上定个闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:55,keyword:"命令词",ASR:"明天凌晨定个闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  /* ---- 以下 120~134 是"当前时间"播报用的音频片段,不是给用户说的 ----
   * ASR 故意用"播报音XX"避免误触发;真正用到的只有 ASRTO 那半边的音频。 */
  //{ID:120,keyword:"命令词",ASR:"播报音点",ASRTO:"点"}
  //{ID:121,keyword:"命令词",ASR:"播报音分",ASRTO:"分"}
  //{ID:122,keyword:"命令词",ASR:"播报音整",ASRTO:"整"}
  //{ID:123,keyword:"命令词",ASR:"播报音零",ASRTO:"零"}
  //{ID:124,keyword:"命令词",ASR:"播报音一",ASRTO:"一"}
  //{ID:125,keyword:"命令词",ASR:"播报音二",ASRTO:"二"}
  //{ID:126,keyword:"命令词",ASR:"播报音三",ASRTO:"三"}
  //{ID:127,keyword:"命令词",ASR:"播报音四",ASRTO:"四"}
  //{ID:128,keyword:"命令词",ASR:"播报音五",ASRTO:"五"}
  //{ID:129,keyword:"命令词",ASR:"播报音六",ASRTO:"六"}
  //{ID:130,keyword:"命令词",ASR:"播报音七",ASRTO:"七"}
  //{ID:131,keyword:"命令词",ASR:"播报音八",ASRTO:"八"}
  //{ID:132,keyword:"命令词",ASR:"播报音九",ASRTO:"九"}
  //{ID:133,keyword:"命令词",ASR:"播报音十",ASRTO:"十"}
  //{ID:134,keyword:"命令词",ASR:"播报音未校准",ASRTO:"时间还没校准"}
  //{ID:135,keyword:"命令词",ASR:"播报音闹钟完成",ASRTO:"好的，已经帮你设定好闹钟了"}
  /* ---- 以下 136~160 是"今天天气"播报用的音频片段,同样只取 ASRTO 那半边的音频 ----
   * 天气现象按 WMO 代码细分到 20 种,避免"晴间多云/多云"混成一句。 */
  //{ID:136,keyword:"命令词",ASR:"播报音晴",ASRTO:"晴"}
  //{ID:137,keyword:"命令词",ASR:"播报音晴间多云",ASRTO:"晴间多云"}
  //{ID:138,keyword:"命令词",ASR:"播报音多云",ASRTO:"多云"}
  //{ID:139,keyword:"命令词",ASR:"播报音阴",ASRTO:"阴"}
  //{ID:140,keyword:"命令词",ASR:"播报音有雾",ASRTO:"有雾"}
  //{ID:141,keyword:"命令词",ASR:"播报音毛毛雨",ASRTO:"毛毛雨"}
  //{ID:142,keyword:"命令词",ASR:"播报音冻雨",ASRTO:"冻雨"}
  //{ID:143,keyword:"命令词",ASR:"播报音小雨",ASRTO:"小雨"}
  //{ID:144,keyword:"命令词",ASR:"播报音中雨",ASRTO:"中雨"}
  //{ID:145,keyword:"命令词",ASR:"播报音大雨",ASRTO:"大雨"}
  //{ID:146,keyword:"命令词",ASR:"播报音暴雨",ASRTO:"暴雨"}
  //{ID:147,keyword:"命令词",ASR:"播报音阵雨",ASRTO:"阵雨"}
  //{ID:148,keyword:"命令词",ASR:"播报音中阵雨",ASRTO:"中阵雨"}
  //{ID:149,keyword:"命令词",ASR:"播报音阵雪",ASRTO:"阵雪"}
  //{ID:150,keyword:"命令词",ASR:"播报音强阵雪",ASRTO:"强阵雪"}
  //{ID:151,keyword:"命令词",ASR:"播报音小雪",ASRTO:"小雪"}
  //{ID:152,keyword:"命令词",ASR:"播报音中雪",ASRTO:"中雪"}
  //{ID:153,keyword:"命令词",ASR:"播报音大雪",ASRTO:"大雪"}
  //{ID:154,keyword:"命令词",ASR:"播报音雷阵雨",ASRTO:"雷阵雨"}
  //{ID:155,keyword:"命令词",ASR:"播报音雷阵雨带冰雹",ASRTO:"雷阵雨伴冰雹"}
  //{ID:156,keyword:"命令词",ASR:"播报音最高",ASRTO:"最高"}
  //{ID:157,keyword:"命令词",ASR:"播报音最低",ASRTO:"最低"}
  //{ID:158,keyword:"命令词",ASR:"播报音度",ASRTO:"度"}
  //{ID:159,keyword:"命令词",ASR:"播报音零下",ASRTO:"零下"}
  //{ID:160,keyword:"命令词",ASR:"播报音没天气",ASRTO:"天气还没更新"}
  //{ID:161,keyword:"命令词",ASR:"播报音转",ASRTO:"转"}
  /* ---- 日期词问答用的引导语,由 app 任务按状态播 ---- */
  //{ID:163,keyword:"命令词",ASR:"播报音问几点",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:164,keyword:"命令词",ASR:"播报音先说时段",ASRTO:"请先说是上午还是下午的闹钟"}
  /* ---- 城市名片段(165 + 166~195):"今天天气"要报地名时用,内容 = "<城市>天气" ----
   * ID 必须与 ESP32 的 k_city_fragments 表一一对应,改名/加城市要两边一起改。
   * 165 是 IP 匹配不到城市时的兜底,只说"天气"。
   * 城市从 70 个精简到 30 个 —— 腾出词数给「X点Y分」的 144 条。 */
  //{ID:165,keyword:"命令词",ASR:"播报音天气",ASRTO:"天气"}
  //{ID:166,keyword:"命令词",ASR:"播报音北京",ASRTO:"北京天气"}
  //{ID:167,keyword:"命令词",ASR:"播报音上海",ASRTO:"上海天气"}
  //{ID:168,keyword:"命令词",ASR:"播报音天津",ASRTO:"天津天气"}
  //{ID:169,keyword:"命令词",ASR:"播报音重庆",ASRTO:"重庆天气"}
  //{ID:170,keyword:"命令词",ASR:"播报音太原",ASRTO:"太原天气"}
  //{ID:171,keyword:"命令词",ASR:"播报音沈阳",ASRTO:"沈阳天气"}
  //{ID:172,keyword:"命令词",ASR:"播报音长春",ASRTO:"长春天气"}
  //{ID:173,keyword:"命令词",ASR:"播报音哈尔滨",ASRTO:"哈尔滨天气"}
  //{ID:174,keyword:"命令词",ASR:"播报音南京",ASRTO:"南京天气"}
  //{ID:175,keyword:"命令词",ASR:"播报音杭州",ASRTO:"杭州天气"}
  //{ID:176,keyword:"命令词",ASR:"播报音合肥",ASRTO:"合肥天气"}
  //{ID:177,keyword:"命令词",ASR:"播报音福州",ASRTO:"福州天气"}
  //{ID:178,keyword:"命令词",ASR:"播报音南昌",ASRTO:"南昌天气"}
  //{ID:179,keyword:"命令词",ASR:"播报音济南",ASRTO:"济南天气"}
  //{ID:180,keyword:"命令词",ASR:"播报音郑州",ASRTO:"郑州天气"}
  //{ID:181,keyword:"命令词",ASR:"播报音武汉",ASRTO:"武汉天气"}
  //{ID:182,keyword:"命令词",ASR:"播报音长沙",ASRTO:"长沙天气"}
  //{ID:183,keyword:"命令词",ASR:"播报音广州",ASRTO:"广州天气"}
  //{ID:184,keyword:"命令词",ASR:"播报音南宁",ASRTO:"南宁天气"}
  //{ID:185,keyword:"命令词",ASR:"播报音成都",ASRTO:"成都天气"}
  //{ID:186,keyword:"命令词",ASR:"播报音贵阳",ASRTO:"贵阳天气"}
  //{ID:187,keyword:"命令词",ASR:"播报音昆明",ASRTO:"昆明天气"}
  //{ID:188,keyword:"命令词",ASR:"播报音西安",ASRTO:"西安天气"}
  //{ID:189,keyword:"命令词",ASR:"播报音乌鲁木齐",ASRTO:"乌鲁木齐天气"}
  //{ID:190,keyword:"命令词",ASR:"播报音深圳",ASRTO:"深圳天气"}
  //{ID:191,keyword:"命令词",ASR:"播报音厦门",ASRTO:"厦门天气"}
  //{ID:192,keyword:"命令词",ASR:"播报音宁波",ASRTO:"宁波天气"}
  //{ID:193,keyword:"命令词",ASR:"播报音青岛",ASRTO:"青岛天气"}
  //{ID:194,keyword:"命令词",ASR:"播报音大连",ASRTO:"大连天气"}
  //{ID:195,keyword:"命令词",ASR:"播报音苏州",ASRTO:"苏州天气"}
  //{ID:20,keyword:"命令词",ASR:"今天早上的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:21,keyword:"命令词",ASR:"今天上午的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:22,keyword:"命令词",ASR:"今天下午的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:23,keyword:"命令词",ASR:"今天晚上的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:24,keyword:"命令词",ASR:"今天凌晨的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:25,keyword:"命令词",ASR:"明天早上的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:26,keyword:"命令词",ASR:"明天上午的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:27,keyword:"命令词",ASR:"明天下午的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:28,keyword:"命令词",ASR:"明天晚上的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  //{ID:29,keyword:"命令词",ASR:"明天凌晨的闹钟",ASRTO:"好的，您想设几点几分的闹钟？"}
  /* ---- 分钟:5 分钟挡位(ID 60~71,分钟值 = (ID-60) × 5) ----
   * 按领导要求改成 5 分钟挡位:1点5分 / 1点10分 / 1点15分 … 1点55分。
   * 60=整点(0分) 61=五分 62=十分 63=十五分 … 71=五十五分,共 12 条。
   * 相比原来的 60 条(一分~五十九分):省 48 个 ID,识别区分度也更高(不会把
   * "四十分/四十四分/三十分"听混)。代价是设不了 3:07 这种非挡位分钟。 */
  /* ---- 小时+分钟一起说：ID 256~399 = 12 小时 × 12 个 5 分钟挡位 ----
   * ASR 问「您想设几点几分的闹钟？」,用户一口气答「四点五分」,一轮搞定。
   * ID = 256 + 小时下标×12 + 分钟下标：
   *   小时下标 0~11 = 一~十二点；分钟下标 0~11 = 整/五分/…/五十五分。
   *
   * 【这 144 条必须全写出来，一条都不能省】
   * 之前这里只留了 280(三点) 和 281(三点五分) 两条，但代码按 256~399 算下标。
   * 结果词表里根本没有“四点/五点/…”，约束解码把它们全硬匹配到最近的热词上，
   * 用户不管说几点都只能得到“三点”——怎么设都是三点就是这个原因。
   * 编号改了要同步改 ASR_CODE 里的 st_hour/st_min 换算。 */
  //{ID:256,keyword:"命令词",ASR:"一点",ASRTO:""}
  //{ID:257,keyword:"命令词",ASR:"一点五分",ASRTO:""}
  //{ID:258,keyword:"命令词",ASR:"一点十分",ASRTO:""}
  //{ID:259,keyword:"命令词",ASR:"一点十五分",ASRTO:""}
  //{ID:260,keyword:"命令词",ASR:"一点二十分",ASRTO:""}
  //{ID:261,keyword:"命令词",ASR:"一点二十五分",ASRTO:""}
  //{ID:262,keyword:"命令词",ASR:"一点三十分",ASRTO:""}
  //{ID:263,keyword:"命令词",ASR:"一点三十五分",ASRTO:""}
  //{ID:264,keyword:"命令词",ASR:"一点四十分",ASRTO:""}
  //{ID:265,keyword:"命令词",ASR:"一点四十五分",ASRTO:""}
  //{ID:266,keyword:"命令词",ASR:"一点五十分",ASRTO:""}
  //{ID:267,keyword:"命令词",ASR:"一点五十五分",ASRTO:""}
  //{ID:268,keyword:"命令词",ASR:"二点",ASRTO:""}
  //{ID:269,keyword:"命令词",ASR:"二点五分",ASRTO:""}
  //{ID:270,keyword:"命令词",ASR:"二点十分",ASRTO:""}
  //{ID:271,keyword:"命令词",ASR:"二点十五分",ASRTO:""}
  //{ID:272,keyword:"命令词",ASR:"二点二十分",ASRTO:""}
  //{ID:273,keyword:"命令词",ASR:"二点二十五分",ASRTO:""}
  //{ID:274,keyword:"命令词",ASR:"二点三十分",ASRTO:""}
  //{ID:275,keyword:"命令词",ASR:"二点三十五分",ASRTO:""}
  //{ID:276,keyword:"命令词",ASR:"二点四十分",ASRTO:""}
  //{ID:277,keyword:"命令词",ASR:"二点四十五分",ASRTO:""}
  //{ID:278,keyword:"命令词",ASR:"二点五十分",ASRTO:""}
  //{ID:279,keyword:"命令词",ASR:"二点五十五分",ASRTO:""}
  //{ID:280,keyword:"命令词",ASR:"三点",ASRTO:""}
  //{ID:281,keyword:"命令词",ASR:"三点五分",ASRTO:""}
  //{ID:282,keyword:"命令词",ASR:"三点十分",ASRTO:""}
  //{ID:283,keyword:"命令词",ASR:"三点十五分",ASRTO:""}
  //{ID:284,keyword:"命令词",ASR:"三点二十分",ASRTO:""}
  //{ID:285,keyword:"命令词",ASR:"三点二十五分",ASRTO:""}
  //{ID:286,keyword:"命令词",ASR:"三点三十分",ASRTO:""}
  //{ID:287,keyword:"命令词",ASR:"三点三十五分",ASRTO:""}
  //{ID:288,keyword:"命令词",ASR:"三点四十分",ASRTO:""}
  //{ID:289,keyword:"命令词",ASR:"三点四十五分",ASRTO:""}
  //{ID:290,keyword:"命令词",ASR:"三点五十分",ASRTO:""}
  //{ID:291,keyword:"命令词",ASR:"三点五十五分",ASRTO:""}
  //{ID:292,keyword:"命令词",ASR:"四点",ASRTO:""}
  //{ID:293,keyword:"命令词",ASR:"四点五分",ASRTO:""}
  //{ID:294,keyword:"命令词",ASR:"四点十分",ASRTO:""}
  //{ID:295,keyword:"命令词",ASR:"四点十五分",ASRTO:""}
  //{ID:296,keyword:"命令词",ASR:"四点二十分",ASRTO:""}
  //{ID:297,keyword:"命令词",ASR:"四点二十五分",ASRTO:""}
  //{ID:298,keyword:"命令词",ASR:"四点三十分",ASRTO:""}
  //{ID:299,keyword:"命令词",ASR:"四点三十五分",ASRTO:""}
  //{ID:300,keyword:"命令词",ASR:"四点四十分",ASRTO:""}
  //{ID:301,keyword:"命令词",ASR:"四点四十五分",ASRTO:""}
  //{ID:302,keyword:"命令词",ASR:"四点五十分",ASRTO:""}
  //{ID:303,keyword:"命令词",ASR:"四点五十五分",ASRTO:""}
  //{ID:304,keyword:"命令词",ASR:"五点",ASRTO:""}
  //{ID:305,keyword:"命令词",ASR:"五点五分",ASRTO:""}
  //{ID:306,keyword:"命令词",ASR:"五点十分",ASRTO:""}
  //{ID:307,keyword:"命令词",ASR:"五点十五分",ASRTO:""}
  //{ID:308,keyword:"命令词",ASR:"五点二十分",ASRTO:""}
  //{ID:309,keyword:"命令词",ASR:"五点二十五分",ASRTO:""}
  //{ID:310,keyword:"命令词",ASR:"五点三十分",ASRTO:""}
  //{ID:311,keyword:"命令词",ASR:"五点三十五分",ASRTO:""}
  //{ID:312,keyword:"命令词",ASR:"五点四十分",ASRTO:""}
  //{ID:313,keyword:"命令词",ASR:"五点四十五分",ASRTO:""}
  //{ID:314,keyword:"命令词",ASR:"五点五十分",ASRTO:""}
  //{ID:315,keyword:"命令词",ASR:"五点五十五分",ASRTO:""}
  //{ID:316,keyword:"命令词",ASR:"六点",ASRTO:""}
  //{ID:317,keyword:"命令词",ASR:"六点五分",ASRTO:""}
  //{ID:318,keyword:"命令词",ASR:"六点十分",ASRTO:""}
  //{ID:319,keyword:"命令词",ASR:"六点十五分",ASRTO:""}
  //{ID:320,keyword:"命令词",ASR:"六点二十分",ASRTO:""}
  //{ID:321,keyword:"命令词",ASR:"六点二十五分",ASRTO:""}
  //{ID:322,keyword:"命令词",ASR:"六点三十分",ASRTO:""}
  //{ID:323,keyword:"命令词",ASR:"六点三十五分",ASRTO:""}
  //{ID:324,keyword:"命令词",ASR:"六点四十分",ASRTO:""}
  //{ID:325,keyword:"命令词",ASR:"六点四十五分",ASRTO:""}
  //{ID:326,keyword:"命令词",ASR:"六点五十分",ASRTO:""}
  //{ID:327,keyword:"命令词",ASR:"六点五十五分",ASRTO:""}
  //{ID:328,keyword:"命令词",ASR:"七点",ASRTO:""}
  //{ID:329,keyword:"命令词",ASR:"七点五分",ASRTO:""}
  //{ID:330,keyword:"命令词",ASR:"七点十分",ASRTO:""}
  //{ID:331,keyword:"命令词",ASR:"七点十五分",ASRTO:""}
  //{ID:332,keyword:"命令词",ASR:"七点二十分",ASRTO:""}
  //{ID:333,keyword:"命令词",ASR:"七点二十五分",ASRTO:""}
  //{ID:334,keyword:"命令词",ASR:"七点三十分",ASRTO:""}
  //{ID:335,keyword:"命令词",ASR:"七点三十五分",ASRTO:""}
  //{ID:336,keyword:"命令词",ASR:"七点四十分",ASRTO:""}
  //{ID:337,keyword:"命令词",ASR:"七点四十五分",ASRTO:""}
  //{ID:338,keyword:"命令词",ASR:"七点五十分",ASRTO:""}
  //{ID:339,keyword:"命令词",ASR:"七点五十五分",ASRTO:""}
  //{ID:340,keyword:"命令词",ASR:"八点",ASRTO:""}
  //{ID:341,keyword:"命令词",ASR:"八点五分",ASRTO:""}
  //{ID:342,keyword:"命令词",ASR:"八点十分",ASRTO:""}
  //{ID:343,keyword:"命令词",ASR:"八点十五分",ASRTO:""}
  //{ID:344,keyword:"命令词",ASR:"八点二十分",ASRTO:""}
  //{ID:345,keyword:"命令词",ASR:"八点二十五分",ASRTO:""}
  //{ID:346,keyword:"命令词",ASR:"八点三十分",ASRTO:""}
  //{ID:347,keyword:"命令词",ASR:"八点三十五分",ASRTO:""}
  //{ID:348,keyword:"命令词",ASR:"八点四十分",ASRTO:""}
  //{ID:349,keyword:"命令词",ASR:"八点四十五分",ASRTO:""}
  //{ID:350,keyword:"命令词",ASR:"八点五十分",ASRTO:""}
  //{ID:351,keyword:"命令词",ASR:"八点五十五分",ASRTO:""}
  //{ID:352,keyword:"命令词",ASR:"九点",ASRTO:""}
  //{ID:353,keyword:"命令词",ASR:"九点五分",ASRTO:""}
  //{ID:354,keyword:"命令词",ASR:"九点十分",ASRTO:""}
  //{ID:355,keyword:"命令词",ASR:"九点十五分",ASRTO:""}
  //{ID:356,keyword:"命令词",ASR:"九点二十分",ASRTO:""}
  //{ID:357,keyword:"命令词",ASR:"九点二十五分",ASRTO:""}
  //{ID:358,keyword:"命令词",ASR:"九点三十分",ASRTO:""}
  //{ID:359,keyword:"命令词",ASR:"九点三十五分",ASRTO:""}
  //{ID:360,keyword:"命令词",ASR:"九点四十分",ASRTO:""}
  //{ID:361,keyword:"命令词",ASR:"九点四十五分",ASRTO:""}
  //{ID:362,keyword:"命令词",ASR:"九点五十分",ASRTO:""}
  //{ID:363,keyword:"命令词",ASR:"九点五十五分",ASRTO:""}
  //{ID:364,keyword:"命令词",ASR:"十点",ASRTO:""}
  //{ID:365,keyword:"命令词",ASR:"十点五分",ASRTO:""}
  //{ID:366,keyword:"命令词",ASR:"十点十分",ASRTO:""}
  //{ID:367,keyword:"命令词",ASR:"十点十五分",ASRTO:""}
  //{ID:368,keyword:"命令词",ASR:"十点二十分",ASRTO:""}
  //{ID:369,keyword:"命令词",ASR:"十点二十五分",ASRTO:""}
  //{ID:370,keyword:"命令词",ASR:"十点三十分",ASRTO:""}
  //{ID:371,keyword:"命令词",ASR:"十点三十五分",ASRTO:""}
  //{ID:372,keyword:"命令词",ASR:"十点四十分",ASRTO:""}
  //{ID:373,keyword:"命令词",ASR:"十点四十五分",ASRTO:""}
  //{ID:374,keyword:"命令词",ASR:"十点五十分",ASRTO:""}
  //{ID:375,keyword:"命令词",ASR:"十点五十五分",ASRTO:""}
  //{ID:376,keyword:"命令词",ASR:"十一点",ASRTO:""}
  //{ID:377,keyword:"命令词",ASR:"十一点五分",ASRTO:""}
  //{ID:378,keyword:"命令词",ASR:"十一点十分",ASRTO:""}
  //{ID:379,keyword:"命令词",ASR:"十一点十五分",ASRTO:""}
  //{ID:380,keyword:"命令词",ASR:"十一点二十分",ASRTO:""}
  //{ID:381,keyword:"命令词",ASR:"十一点二十五分",ASRTO:""}
  //{ID:382,keyword:"命令词",ASR:"十一点三十分",ASRTO:""}
  //{ID:383,keyword:"命令词",ASR:"十一点三十五分",ASRTO:""}
  //{ID:384,keyword:"命令词",ASR:"十一点四十分",ASRTO:""}
  //{ID:385,keyword:"命令词",ASR:"十一点四十五分",ASRTO:""}
  //{ID:386,keyword:"命令词",ASR:"十一点五十分",ASRTO:""}
  //{ID:387,keyword:"命令词",ASR:"十一点五十五分",ASRTO:""}
  //{ID:388,keyword:"命令词",ASR:"十二点",ASRTO:""}
  //{ID:389,keyword:"命令词",ASR:"十二点五分",ASRTO:""}
  //{ID:390,keyword:"命令词",ASR:"十二点十分",ASRTO:""}
  //{ID:391,keyword:"命令词",ASR:"十二点十五分",ASRTO:""}
  //{ID:392,keyword:"命令词",ASR:"十二点二十分",ASRTO:""}
  //{ID:393,keyword:"命令词",ASR:"十二点二十五分",ASRTO:""}
  //{ID:394,keyword:"命令词",ASR:"十二点三十分",ASRTO:""}
  //{ID:395,keyword:"命令词",ASR:"十二点三十五分",ASRTO:""}
  //{ID:396,keyword:"命令词",ASR:"十二点四十分",ASRTO:""}
  //{ID:397,keyword:"命令词",ASR:"十二点四十五分",ASRTO:""}
  //{ID:398,keyword:"命令词",ASR:"十二点五十分",ASRTO:""}
  //{ID:399,keyword:"命令词",ASR:"十二点五十五分",ASRTO:""}
  /* ---- 小时:把小时念回去让用户确认,再问分钟 ---- */
  //{ID:242,keyword:"命令词",ASR:"关闭闹钟",ASRTO:"闹钟已关闭"}
  //{ID:243,keyword:"命令词",ASR:"取消闹钟",ASRTO:"闹钟已关闭"}
  //{ID:250,keyword:"命令词",ASR:"大声点",ASRTO:"好"}
  //{ID:251,keyword:"命令词",ASR:"小声点",ASRTO:"好"}
  //{ID:252,keyword:"命令词",ASR:"音量加",ASRTO:"好"}
  //{ID:253,keyword:"命令词",ASR:"音量减",ASRTO:"好"}

  /* ---- 用户跳过日期直接说时间时的提示(由 app 任务按 g_pending_ask = 400 播) ---- */
  //{ID:400,keyword:"命令词",ASR:"播报音先说日期",ASRTO:"请先说是今天的、明天的、还是每天的"}
  /* ---- 只说时段、且日期还不知道时问的一句;用户已先说日期就直接问时间 ---- */
  //{ID:401,keyword:"命令词",ASR:"播报音问日期",ASRTO:"好的，是今天的、明天的、还是每天的？"}
  /* ---- 闹钟响铃播报的最后一句(前面依次是问候/时间/天气,由 ESP32 拼) ---- */
  //{ID:402,keyword:"命令词",ASR:"播报音记得起床",ASRTO:"记得按时起床哦"}
}
