// 设备模拟器：LVGL + 固件的 nomi_ui / face 代码编成 WebAssembly，由 sim.html 驱动。
//
// LVGL 刷屏写进 s_lcd（相当于屏幕显存），鼠标当触摸，空格当 BOOT 键。
//
// 整机流程照着固件回放：开机 → 没存 Wi-Fi 就进配网模式 → 手机在配网页填 Wi-Fi → 联网 →
// 启动直连协议、开网页设置页 → 待命 → 对话。每一步往屏幕上发什么（状态、表情、通知条）都按固件里
// Application / WifiBoard / WifiStation / DirectApiProtocol 经 NomiDisplay 调到 nomi_ui 的顺序和时长来，
// 注释里标了对应的固件代码。
//
// 这里只管设备状态；网页和网络那一面由 serve.py 扮演：配网页、设置页是固件里的同一份 HTML，
// 对话时 sim.html 录音交给 serve.py，由它照 DirectApiProtocol 的格式真的去调 API（或给假回答），
// 结果再经 sim_turn_* / sim_tts_* 交回这里，像固件收到 stt / llm / tts 消息一样更新屏幕。
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cbin_font.h"
#include "face.h"
#include "lvgl.h"
#include "nomi_ui.h"

#define LCD_W 480
#define LCD_H 320
#define DRAW_LINES 40

static uint16_t s_lcd[LCD_W * LCD_H];
static uint16_t s_face[FACE_W * FACE_H];
static uint8_t s_draw_buf[LCD_W * DRAW_LINES * 2] __attribute__((aligned(64)));
static int s_dx0 = LCD_W, s_dy0 = LCD_H, s_dx1, s_dy1;
static int s_px, s_py, s_pressed;
static uint32_t s_ms;

/* ---------- 串口日志（sim.html 读出来显示成 idf.py monitor 的样子） ---------- */

static char s_log[16384];
static int s_log_len;

static void dev_log(char level, const char *tag, const char *fmt, ...)
{
    char line[512];
    int n = snprintf(line, sizeof(line), "%c (%u) %s: ", level, (unsigned)s_ms, tag);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    if (s_log_len + n > (int)sizeof(s_log)) s_log_len = 0;  // JS 每帧取走，正常不会满
    memcpy(s_log + s_log_len, line, n);
    s_log_len += n;
}

/* ---------- 网页那一面传进来的文字 ---------- */

enum { TEXT_WIFI_SSID, TEXT_CONFIG_URL, TEXT_IP, TEXT_AP_URL, TEXT_AP_SSID, TEXT_VOICES, TEXT_COUNT };
static char s_str[4096];  // JS 先把 UTF-8 写进这里，再调 sim_set_text / sim_turn_* / sim_face_play 等
static char s_text[TEXT_COUNT][256];

/* ---------- 设备状态 ---------- */

typedef enum {
    STAGE_STARTING,     // kDeviceStateStarting
    STAGE_WIFI_CONFIG,  // kDeviceStateWifiConfiguring：开着热点等手机配网
    STAGE_JOINING,      // 开机连路由器（设备状态仍是 Starting）
    STAGE_ACTIVATING,   // kDeviceStateActivating：启动直连协议
    STAGE_READY,        // kDeviceStateIdle 及之后的对话
} stage_t;

typedef enum { CHAT_IDLE, CHAT_CONNECTING, CHAT_LISTENING, CHAT_SPEAKING } chat_t;

enum { NET_OFF, NET_AP, NET_STA };
enum { REQ_NONE, REQ_FACTORY_RESET, REQ_SELFTEST };

#define FW_VERSION "2.5.2"

static stage_t s_stage;
static uint32_t s_stage_at;
static int s_step;  // 当前阶段里走到第几步
static int s_has_wifi, s_api_ok, s_net, s_request;
static bool s_force_config;  // 设置里点了重新配网：存过 Wi-Fi 也进配网模式
static bool s_error_shown;   // Application 的 last_error_message_ 非空：待命时不覆盖错误提示

static chat_t s_chat = CHAT_IDLE;
static uint32_t s_chat_at;
static bool s_recording;     // DirectApiProtocol::recording_：麦克风在收音，sim.html 跟着开关录音
static bool s_holding;       // BOOT 键按住说话中
static bool s_channel_open;  // Protocol::IsAudioChannelOpened
static int s_turn_id;        // 对话被打断 / 关闭时加一，sim.html 丢掉这一轮还没回来的结果
static int s_rec_started, s_rec_finished;  // 录音开始 / 结束的次数：同一帧里开了又关也不会漏掉

static void set_recording(bool on)
{
    if (on == s_recording) return;
    s_recording = on;
    if (on) s_rec_started++;
    else s_rec_finished++;
}

// 设置项存在内存里，刷新页面就没了
static int s_talk_key, s_aec, s_subtitles = 1, s_persona, s_voice = 4, s_dim = 1, s_off = 1;
static int s_volume = 70, s_brightness = 80;

/* ---------- 聊天历史（对应 NomiDisplay::AddChatMessage） ---------- */

#define HIST_MAX 40
#define HIST_TEXT_LEN 1024
static char s_hist_text[HIST_MAX][HIST_TEXT_LEN];
static char s_hist_role[HIST_MAX][12];
static int s_hist_n;

static bool ascii_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static void hist_add(const char *role, const char *text)
{
    if (!text || !*text) return;
    // 同一条回答的下一句接着记（小智逐句下发字幕）
    if (s_hist_n && !strcmp(role, "assistant") && !strcmp(s_hist_role[s_hist_n - 1], "assistant")) {
        char *last = s_hist_text[s_hist_n - 1];
        size_t len = strlen(last);
        if (len && len < HIST_TEXT_LEN - 1 && ascii_alnum(last[len - 1]) && ascii_alnum(text[0])) {
            last[len++] = ' ';
        }
        snprintf(last + len, HIST_TEXT_LEN - len, "%s", text);
        return;
    }
    if (s_hist_n >= HIST_MAX) {
        memmove(s_hist_text, s_hist_text + 1, (HIST_MAX - 1) * HIST_TEXT_LEN);
        memmove(s_hist_role, s_hist_role + 1, (HIST_MAX - 1) * sizeof(s_hist_role[0]));
        s_hist_n--;
    }
    snprintf(s_hist_role[s_hist_n], sizeof(s_hist_role[0]), "%s", role);
    snprintf(s_hist_text[s_hist_n], HIST_TEXT_LEN, "%s", text);
    s_hist_n++;
}

static int hist_count(void) { return s_hist_n; }

static const char *hist_get(int index, const char **role)
{
    if (index < 0 || index >= s_hist_n) {
        *role = "";
        return "";
    }
    *role = s_hist_role[index];
    return s_hist_text[index];
}

static void set_stage(stage_t stage)
{
    s_stage = stage;
    s_stage_at = s_ms;
    s_step = 0;
}

// Application::Alert：状态栏 + 表情 + 系统消息（NomiDisplay 把系统消息显示成 4 秒通知条）
static void alert(const char *status, const char *message, const char *emotion)
{
    dev_log('W', "Application", "Alert [%s] %s: %s", emotion, status, message);
    nomi_ui_set_status(status);
    nomi_ui_set_emotion(emotion);
    nomi_ui_notify(message, 4000);
}

// Application::HandleStateChangedEvent 里 kDeviceStateIdle 那段（NomiDisplay 没接 ClearChatMessages，字幕留着）
static void show_standby(void)
{
    if (s_error_shown) return;
    nomi_ui_set_status("待命");
    nomi_ui_set_emotion("neutral");
}

/* ---------- 对话状态（Application 的 Idle / Connecting / Listening / Speaking） ---------- */

static bool auto_mode(void) { return s_talk_key != 0; }  // 点一下开始 = kListeningModeAutoStop

static void set_chat(chat_t chat)
{
    s_chat = chat;
    s_chat_at = s_ms;
    switch (chat) {
    case CHAT_IDLE:
        set_recording(false);
        dev_log('I', "Application", "STATE: idle");
        show_standby();
        break;
    case CHAT_CONNECTING:
        dev_log('I', "Application", "STATE: connecting");
        nomi_ui_set_status("连接中...");
        nomi_ui_set_emotion("neutral");
        break;
    case CHAT_LISTENING:
        dev_log('I', "Application", "STATE: listening");
        nomi_ui_set_status("聆听中...");
        nomi_ui_set_emotion("neutral");
        set_recording(true);  // StartListeningAudio → SendStartListening
        break;
    case CHAT_SPEAKING:
        set_recording(false);
        dev_log('I', "Application", "STATE: speaking");
        nomi_ui_set_status("说话中...");
        break;
    }
}

// SetError → MAIN_EVENT_ERROR：回到待命并弹「错误」；协议标记出错，下次说话重新开通道
static void chat_error(const char *message)
{
    s_turn_id++;
    s_channel_open = false;
    s_holding = false;
    s_error_shown = false;
    set_chat(CHAT_IDLE);
    alert("错误", message, "cancel");
    s_error_shown = true;
}

// CloseAudioChannel → OnAudioChannelClosed → 待命
static void close_channel(const char *why)
{
    if (s_chat == CHAT_IDLE && !s_channel_open) return;
    dev_log('I', "Application", "Closing audio channel (%s)", why);
    s_turn_id++;
    s_channel_open = false;
    s_holding = false;
    set_chat(CHAT_IDLE);
}

// ToggleChatState / StartListening 从待命开始：先开音频通道
static void start_chat(void)
{
    s_error_shown = false;
    if (!s_channel_open) {
        set_chat(CHAT_CONNECTING);  // 接着 ContinueOpenAudioChannel
    } else {
        set_chat(CHAT_LISTENING);
    }
}

/* ---------- Wi-Fi（WifiStation 的扫描 / 重连 和 WifiBoard 的开机超时） ---------- */

typedef enum { STA_OFF, STA_SCANNING, STA_CONNECTING, STA_RECONNECTING, STA_WAIT_SCAN, STA_CONNECTED } sta_t;

#define MAX_RECONNECT_COUNT 5
#define CONNECT_TIMEOUT_MS 60000   // WifiBoard：开机 60 秒连不上就进配网
#define SCAN_MIN_S 10              // 扫不到路由器时下次扫描的间隔，翻倍到 300 秒
#define SCAN_MAX_S 300

static sta_t s_sta;
static uint32_t s_sta_at, s_connect_deadline;
static int s_reconnects, s_scan_interval = SCAN_MIN_S;
static int s_router = 1;  // 网络环境面板：路由器在不在线

static void enter_wifi_config(void);

static void sta_set(sta_t sta)
{
    s_sta = sta;
    s_sta_at = s_ms;
}

static void sta_scan(void)
{
    sta_set(STA_SCANNING);
    dev_log('I', "WifiBoard", "WiFi scanning");
    nomi_ui_notify("扫描 Wi-Fi...", 30000);  // Application：Scanning 事件
}

static void sta_wait_scan(void)
{
    dev_log('I', "WifiStation", "No more AP to connect, next scan in %d seconds", s_scan_interval);
    sta_set(STA_WAIT_SCAN);
}

static void sta_connected(void)
{
    char msg[160];
    sta_set(STA_CONNECTED);
    s_reconnects = 0;
    s_scan_interval = SCAN_MIN_S;
    s_connect_deadline = 0;
    s_net = NET_STA;
    dev_log('I', "WifiStation", "Got IP: %s", s_text[TEXT_IP]);
    dev_log('I', "WifiBoard", "Connected to WiFi: %s", s_text[TEXT_WIFI_SSID]);
    snprintf(msg, sizeof(msg), "已连接 %s", s_text[TEXT_WIFI_SSID]);
    nomi_ui_notify(msg, 30000);
    // HandleNetworkConnectedEvent：只有开机 / 配网时才去激活，平时重连上就只是提示一下
    if (s_stage == STAGE_JOINING) {
        dev_log('I', "Application", "Network connected");
        dev_log('I', "Application", "STATE: activating");
        set_stage(STAGE_ACTIVATING);
    }
}

// WifiManager::StartStation（开机时 WifiBoard 还会开 60 秒的超时）
static void station_start(bool with_timeout)
{
    s_reconnects = 0;
    s_scan_interval = SCAN_MIN_S;
    s_connect_deadline = with_timeout ? s_ms + CONNECT_TIMEOUT_MS : 0;
    sta_scan();
}

static void advance_station(void)
{
    uint32_t t = s_ms - s_sta_at;
    char msg[160];
    if (s_connect_deadline && s_ms > s_connect_deadline && s_sta != STA_CONNECTED) {
        dev_log('W', "WifiBoard", "WiFi connection timeout, entering config mode");
        sta_set(STA_OFF);
        s_connect_deadline = 0;
        enter_wifi_config();
        return;
    }
    switch (s_sta) {
    case STA_SCANNING:
        if (t < 1200) break;
        if (s_router) {
            sta_set(STA_CONNECTING);
            dev_log('I', "WifiBoard", "WiFi connecting to %s", s_text[TEXT_WIFI_SSID]);
            snprintf(msg, sizeof(msg), "连接 %s...", s_text[TEXT_WIFI_SSID]);
            nomi_ui_notify(msg, 30000);
        } else {
            sta_wait_scan();
        }
        break;
    case STA_CONNECTING:
        if (t < 1400) break;
        if (s_router) {
            sta_connected();
        } else {
            s_reconnects = 0;
            sta_set(STA_RECONNECTING);
        }
        break;
    case STA_RECONNECTING:
        if (t < 1000) break;
        if (s_router) {
            sta_connected();
        } else if (s_reconnects < MAX_RECONNECT_COUNT) {
            s_reconnects++;
            dev_log('I', "WifiStation", "Reconnecting %s (attempt %d / %d)", s_text[TEXT_WIFI_SSID], s_reconnects,
                    MAX_RECONNECT_COUNT);
            sta_set(STA_RECONNECTING);
        } else {
            sta_wait_scan();
        }
        break;
    case STA_WAIT_SCAN:
        if (t < (uint32_t)s_scan_interval * 1000) break;
        s_scan_interval = s_scan_interval * 2 > SCAN_MAX_S ? SCAN_MAX_S : s_scan_interval * 2;
        sta_scan();
        break;
    default:
        break;
    }
}

// 路由器掉线：WIFI_EVENT_STA_DISCONNECTED → Disconnected 事件 → Application 关掉进行中的对话
static void router_lost(void)
{
    dev_log('I', "WifiStation", "WiFi disconnected, reason: 200");
    dev_log('W', "WifiBoard", "WiFi disconnected");
    s_net = NET_OFF;
    if (s_chat != CHAT_IDLE) close_channel("network disconnected");
    s_reconnects = 0;
    sta_set(STA_RECONNECTING);
}

/* ---------- 开机流程 ---------- */

// WifiBoard::StartWifiConfigMode
static void enter_wifi_config(void)
{
    set_stage(STAGE_WIFI_CONFIG);
    s_force_config = false;
    s_net = NET_AP;
    dev_log('I', "WifiBoard", "WiFi config mode entered");
    dev_log('I', "WifiConfigurationAp", "Access Point started with SSID %s", s_text[TEXT_AP_SSID]);
    char hint[256];
    snprintf(hint, sizeof(hint), "手机连接热点 %s，浏览器访问 %s", s_text[TEXT_AP_SSID], s_text[TEXT_AP_URL]);
    alert("配网模式", hint, "gear");
}

// WifiBoard::TryWifiConnect 有存过的 Wi-Fi 时
static void start_joining(void)
{
    set_stage(STAGE_JOINING);
    s_net = NET_OFF;
    dev_log('I', "WifiBoard", "Starting WiFi connection attempt");
    station_start(true);
}

static void advance_boot(void)
{
    uint32_t t = s_ms - s_stage_at;
    char msg[160];
    switch (s_stage) {
    case STAGE_STARTING:
        // TryWifiConnect：没存 Wi-Fi 时等 1.5 秒（让版本号露个脸）再进配网
        if (s_has_wifi && !s_force_config) {
            if (t > 300) start_joining();
        } else if (t > 1500) {
            if (!s_force_config) dev_log('I', "WifiBoard", "No SSID configured, entering config mode");
            enter_wifi_config();
        }
        break;
    case STAGE_ACTIVATING:
        if (s_step == 0 && t > 400) {
            // InitializeProtocol → DirectApiProtocol::Start
            nomi_ui_set_status("登录服务器...");
            dev_log('I', "DirectApiCfg", "Config page: %s", s_text[TEXT_CONFIG_URL]);
            if (!s_api_ok) {
                snprintf(msg, sizeof(msg), "设置页：%s", s_text[TEXT_CONFIG_URL]);
                nomi_ui_notify(msg, 30000);
            }
            s_step = 1;
        } else if (s_step == 1 && t > 700) {
            // HandleActivationDoneEvent：先切到待命，再弹版本号（默认 3 秒）
            dev_log('I', "Application", "Activation done");
            dev_log('I', "Application", "STATE: idle");
            nomi_ui_notify("版本 " FW_VERSION, 3000);
            show_standby();
            set_stage(STAGE_READY);
        }
        break;
    default:
        break;
    }
}

/* ---------- 休眠 ---------- */

// 休眠和设备一样分三段：先正常亮度睡着，睡着 30 秒后调暗，到息屏时间关屏
#define DIM_BRIGHTNESS 5
#define DIM_AFTER_SLEEP_MS 30000
static uint32_t s_last_activity, s_slept_at;
static int s_sleep_level;  // 0 醒着，1 睡着（正常亮度），2 调暗，3 息屏

static void wake(void)
{
    s_last_activity = s_ms;
    if (s_sleep_level) {
        s_sleep_level = 0;
        nomi_ui_set_sleep(false);
    }
}

static void sleep_now(void)
{
    s_sleep_level = 1;
    s_slept_at = s_ms;
    nomi_ui_set_sleep(true);
}

static void advance(void)
{
    if (s_chat != CHAT_IDLE || s_stage != STAGE_READY) {
        s_last_activity = s_ms;  // 对话中、开机联网中不算空闲
    } else {
        static const int dim_s[] = NOMI_DIM_SECONDS, off_s[] = NOMI_OFF_SECONDS;
        uint32_t idle = s_ms - s_last_activity;
        if (s_sleep_level == 0 && dim_s[s_dim] > 0 && idle > (uint32_t)dim_s[s_dim] * 1000) sleep_now();
        else if (s_sleep_level == 1 && s_ms - s_slept_at > DIM_AFTER_SLEEP_MS) s_sleep_level = 2;
        // 息屏不早于"睡着再调暗"之后
        else if (s_sleep_level == 2 && off_s[s_off] > 0 && idle > (uint32_t)off_s[s_off] * 1000) s_sleep_level = 3;
    }
    // ContinueOpenAudioChannel → DirectApiProtocol::OpenAudioChannel
    if (s_chat == CHAT_CONNECTING && s_ms - s_chat_at > 150) {
        if (!s_api_ok) {
            char msg[200];
            snprintf(msg, sizeof(msg), "API 未配置，请用浏览器打开 %s 填写", s_text[TEXT_CONFIG_URL]);
            chat_error(msg);
            return;
        }
        s_channel_open = true;
        set_chat(CHAT_LISTENING);
    }
}

/* ---------- nomi_ui 回调（对应 NomiDisplay） ---------- */

static nomi_state_t get_state(void)
{
    if (s_stage == STAGE_ACTIVATING || s_chat == CHAT_CONNECTING) return NOMI_STATE_CONNECTING;
    if (s_chat == CHAT_LISTENING) return NOMI_STATE_LISTENING;
    if (s_chat == CHAT_SPEAKING) return NOMI_STATE_SPEAKING;
    return NOMI_STATE_IDLE;
}

static int get_int(const char *key, int def)
{
    if (!strcmp(key, NOMI_KEY_TALK_KEY)) return s_talk_key;
    if (!strcmp(key, NOMI_KEY_VOLUME)) return s_volume;
    if (!strcmp(key, NOMI_KEY_BRIGHTNESS)) return s_brightness;
    if (!strcmp(key, NOMI_KEY_AEC)) return s_aec;
    if (!strcmp(key, NOMI_KEY_AEC_SUPPORTED)) return 1;
    if (!strcmp(key, NOMI_KEY_SUBTITLES)) return s_subtitles;
    if (!strcmp(key, NOMI_KEY_PERSONA)) return s_persona;
    if (!strcmp(key, NOMI_KEY_VOICE)) return s_voice;
    if (!strcmp(key, NOMI_KEY_DIM)) return s_dim;
    if (!strcmp(key, NOMI_KEY_OFF)) return s_off;
    return def;
}

static void set_int(const char *key, int value, bool final)
{
    (void)final;
    if (!strcmp(key, NOMI_KEY_TALK_KEY)) s_talk_key = value;
    else if (!strcmp(key, NOMI_KEY_VOLUME)) s_volume = value;
    else if (!strcmp(key, NOMI_KEY_BRIGHTNESS)) s_brightness = value;
    else if (!strcmp(key, NOMI_KEY_AEC)) s_aec = value;
    else if (!strcmp(key, NOMI_KEY_SUBTITLES)) s_subtitles = value;
    else if (!strcmp(key, NOMI_KEY_PERSONA)) s_persona = value;
    else if (!strcmp(key, NOMI_KEY_VOICE)) s_voice = value;
    else if (!strcmp(key, NOMI_KEY_DIM)) s_dim = value;
    else if (!strcmp(key, NOMI_KEY_OFF)) s_off = value;
}

// GetText：没连上路由器时 Wi-Fi、IP 都是空的；设置页地址协议启动后才有
static const char *get_text(const char *key)
{
    static char buf[160];
    bool online = s_net == NET_STA;
    if (!strcmp(key, "personas")) return "桌面小伙伴\n毒舌损友\n学习助手\n英语陪练";
    if (!strcmp(key, "voices")) return s_text[TEXT_VOICES];  // 合成供应商的音色短名，serve.py 给的
    if (!strcmp(key, "wifi")) {
        if (!online) return "";
        snprintf(buf, sizeof(buf), "%s（信号 -42 dBm）", s_text[TEXT_WIFI_SSID]);
        return buf;
    }
    if (!strcmp(key, "ip")) return online ? s_text[TEXT_IP] : "";
    if (!strcmp(key, "config_url")) return online && s_stage == STAGE_READY ? s_text[TEXT_CONFIG_URL] : "";
    if (!strcmp(key, "version")) return "Pocket Companion " FW_VERSION "（模拟器）";
    return "";
}

static void action(const char *name)
{
    if (!strcmp(name, "test_api")) {
        // NomiDisplay：先提示，测完用通知条显示 DirectApiProtocol::SelfTest 的结果（serve.py 代测）
        nomi_ui_notify("正在测试 API…", 15000);
        s_request = REQ_SELFTEST;
    } else if (!strcmp(name, "reconfigure_wifi")) {
        // WifiBoard::EnterWifiConfigMode：提示一下，1 秒后断开路由器、开热点
        dev_log('I', "WifiBoard", "EnterWifiConfigMode called");
        nomi_ui_notify("进入配网模式...", 3000);
        close_channel("enter wifi config");
        sta_set(STA_OFF);
        s_connect_deadline = 0;
        s_net = NET_OFF;
        set_stage(STAGE_STARTING);
        s_force_config = true;
        s_stage_at = s_ms - 500;  // STARTING 阶段等 1.5 秒，这里只等 1 秒
    } else if (!strcmp(name, "factory_reset")) {
        dev_log('W', "NomiDisplay", "Factory reset");
        s_request = REQ_FACTORY_RESET;  // 擦 NVS 和重启交给 sim.html
    }
}

/* ---------- LVGL 驱动 ---------- */

static uint32_t tick_cb(void) { return s_ms; }

static void flush_cb(lv_display_t *disp, const lv_area_t *a, uint8_t *px)
{
    int w = a->x2 - a->x1 + 1;
    const uint16_t *src = (const uint16_t *)px;
    for (int y = a->y1; y <= a->y2; y++, src += w) {
        uint16_t *dst = s_lcd + y * LCD_W + a->x1;
        for (int x = 0; x < w; x++) dst[x] = src[x];
    }
    if (a->x1 < s_dx0) s_dx0 = a->x1;
    if (a->y1 < s_dy0) s_dy0 = a->y1;
    if (a->x2 + 1 > s_dx1) s_dx1 = a->x2 + 1;
    if (a->y2 + 1 > s_dy1) s_dy1 = a->y2 + 1;
    lv_display_flush_ready(disp);
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point.x = s_px;
    data->point.y = s_py;
    data->state = s_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ---------- 导出给 JS：启动和每帧 ---------- */

static uint8_t *s_font_bin;

// JS 把小智的字体文件（和设备同一个 font_noto_sans_common_20_4.bin）拷进这块内存
uint8_t *sim_font_buffer(int size)
{
    s_font_bin = malloc(size);
    return s_font_bin;
}

char *sim_str_buf(void) { return s_str; }
int sim_str_size(void) { return sizeof(s_str); }

void sim_set_text(int id)
{
    if (id < 0 || id >= TEXT_COUNT) return;
    snprintf(s_text[id], sizeof(s_text[id]), "%s", s_str);
}

// 上电：has_wifi / api_ok 是"NVS"里有没有存 Wi-Fi、API 设置是否齐全（由 serve.py 保管）；router 是路由器在不在线
void sim_init(uint32_t ms, int has_wifi, int api_ok, int router)
{
    s_ms = ms;
    s_has_wifi = has_wifi;
    s_api_ok = api_ok;
    s_router = router;
    lv_init();
    lv_tick_set_cb(tick_cb);
    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, s_draw_buf, NULL, sizeof(s_draw_buf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_cb);

    static const nomi_ui_callbacks_t callbacks = {
        .get_state = get_state,
        .user_activity = wake,
        .get_int = get_int,
        .set_int = set_int,
        .get_text = get_text,
        .action = action,
        .history_count = hist_count,
        .history_get = hist_get,
    };
    // 没拿到小智字体时退回 LVGL 自带的中文子集（缺字较多）
    const lv_font_t *font = s_font_bin ? cbin_font_create(s_font_bin) : NULL;
    nomi_ui_create(lv_screen_active(), s_face, font ? font : &lv_font_source_han_sans_sc_16_cjk,
                   &callbacks);
    nomi_ui_set_battery(76, false);  // 假电量
    s_last_activity = ms;

    // Application::Initialize：先把板子名 / 版本作为系统消息打出来，再启动网络
    dev_log('I', "main", "Pocket Companion %s (spotpear-esp32s3-max35)", FW_VERSION);
    dev_log('I', "Application", "STATE: starting");
    nomi_ui_notify("spotpear-esp32s3-max35/" FW_VERSION, 4000);
    set_stage(STAGE_STARTING);
    if (s_has_wifi) dev_log('I', "WifiBoard", "Saved SSID: %s", s_text[TEXT_WIFI_SSID]);
}

// 推进到 ms 时刻跑一次 LVGL；返回本次刷到"屏幕"的面积
int sim_frame(uint32_t ms)
{
    s_ms = ms;
    advance_boot();
    advance_station();
    advance();
    lv_timer_handler();
    return s_dx1 > s_dx0 ? (s_dx1 - s_dx0) * (s_dy1 - s_dy0) : 0;
}

void sim_clear_dirty(void)
{
    s_dx0 = LCD_W;
    s_dy0 = LCD_H;
    s_dx1 = s_dy1 = 0;
}

uint16_t *sim_lcd(void) { return s_lcd; }
int sim_dirty_x0(void) { return s_dx0; }
int sim_dirty_y0(void) { return s_dy0; }
int sim_dirty_x1(void) { return s_dx1; }
int sim_dirty_y1(void) { return s_dy1; }
int sim_brightness(void)
{
    return s_sleep_level == 3 ? 0 : s_sleep_level == 2 ? DIM_BRIGHTNESS : s_brightness;
}
void sim_force_sleep(void)
{
    if (s_stage == STAGE_READY && s_chat == CHAT_IDLE) sleep_now();
}
int sim_volume(void) { return s_volume; }
int sim_net(void) { return s_net; }
int sim_stage(void) { return s_stage; }
int sim_chat(void) { return s_chat; }
int sim_talk_key(void) { return s_talk_key; }

int sim_request(void)
{
    int r = s_request;
    s_request = REQ_NONE;
    return r;
}

char *sim_log_buf(void) { return s_log; }
int sim_log_len(void) { return s_log_len; }
void sim_log_clear(void) { s_log_len = 0; }

void sim_notify(int duration_ms) { nomi_ui_notify(s_str, duration_ms); }  // Display::ShowNotification

/* ---------- 导出给 JS：网页 / 网络那一面发生的事 ---------- */

// 配网页 /submit 成功：设备连上了这个 Wi-Fi 并存进 NVS（SSID 先经 sim_set_text 传进来）
void sim_wifi_saved(void)
{
    dev_log('I', "WifiConfigurationAp", "Connected to %s, saved", s_text[TEXT_WIFI_SSID]);
    s_has_wifi = 1;
}

// 配网完成页发来 /exit：关热点，用新 Wi-Fi 联网
void sim_ap_exit(void)
{
    if (s_stage != STAGE_WIFI_CONFIG || !s_has_wifi) return;
    dev_log('I', "WifiConfigurationAp", "Exiting config mode...");
    dev_log('I', "WifiBoard", "WiFi config mode exited");
    start_joining();
}

// 网页设置页保存了：下一轮对话按新设置走
void sim_settings_saved(int api_ok)
{
    dev_log('I', "DirectApiCfg", "Settings saved");
    s_api_ok = api_ok;
}

// 网络环境面板：路由器上线 / 掉线（上线后由扫描 / 重连自己连回去）
void sim_router(int on)
{
    if (on == s_router) return;
    s_router = on;
    if (!on && s_sta == STA_CONNECTED) router_lost();
}

/* ---------- 导出给 JS：一轮对话（对应 DirectApiProtocol::RunTurn 发出的消息） ---------- */

int sim_recording(void) { return s_recording; }
int sim_rec_started(void) { return s_rec_started; }
int sim_rec_finished(void) { return s_rec_finished; }
int sim_turn_id(void) { return s_turn_id; }

// 自动模式下设备端 VAD 判断说完了（或录满 15 秒）：FinishRecording，状态仍是聆听
void sim_vad_end(void) { set_recording(false); }

// RunTurn 一开始：EmitJson("llm", "emotion", "thinking")
void sim_turn_thinking(void) { nomi_ui_set_emotion("thinking"); }

// "stt"：识别出的文字作为用户字幕，记进聊天历史
void sim_turn_stt(void)
{
    dev_log('I', "Application", ">> %s", s_str);
    hist_add("user", s_str);
    nomi_ui_set_subtitle("user", s_str);
}

// 没识别出内容：自动模式回到聆听，手动模式已经在待命
void sim_turn_empty(void)
{
    nomi_ui_set_emotion("neutral");  // RunTurn：没听清时结束"思考中"
    if (auto_mode() && s_channel_open && s_chat == CHAT_LISTENING) set_recording(true);
}

// "llm"：情绪
void sim_turn_emotion(void) { nomi_ui_set_emotion(s_str); }

// "tts" start / sentence_start / stop
void sim_tts_start(void) { set_chat(CHAT_SPEAKING); }

void sim_tts_sentence(void)
{
    dev_log('I', "Application", "<< %s", s_str);
    hist_add("assistant", s_str);
    nomi_ui_set_subtitle("assistant", s_str);
}

void sim_tts_stop(void)
{
    if (s_chat != CHAT_SPEAKING) return;
    set_chat(auto_mode() ? CHAT_LISTENING : CHAT_IDLE);
}

void sim_turn_error(void) { chat_error(s_str); }

// 大模型调用了设备工具：0 音量，1 亮度
void sim_apply(int kind, int value)
{
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    if (kind == 0) {
        s_volume = value;
        dev_log('I', "Board", "Set volume to %d", value);
    } else {
        s_brightness = value < 10 ? 10 : value;
        dev_log('I', "Backlight", "Set brightness to %d", s_brightness);
    }
}

/* ---------- 表情脚本 ---------- */

// 播一段表情脚本（JSON 先写进 s_str）：试播框、大模型回复里的 <face>、self.face.play 工具都走这里。
// 失败返回 0，原因写回 s_str
int sim_face_play(void)
{
    char err[160];
    wake();
    if (nomi_ui_play_face(s_str, err, sizeof(err))) {
        dev_log('I', "Face", "play script (%u bytes)", (unsigned)strlen(s_str));
        return 1;
    }
    dev_log('W', "Face", "bad script: %s", err);
    snprintf(s_str, sizeof(s_str), "%s", err);
    return 0;
}

/* ---------- 输入 ---------- */

// BOOT 键（spotpear_esp32s3_max35.cc）：开机阶段点一下进配网；联网后按住说话 / 点一下切换
void sim_boot_down(void)
{
    wake();
    if (s_stage == STAGE_STARTING || s_stage == STAGE_JOINING) {
        action("reconfigure_wifi");
        return;
    }
    if (s_stage != STAGE_READY) return;
    if (!auto_mode()) {
        // StartListening：说话中按下会打断它，直接开始听
        s_holding = true;
        if (s_chat == CHAT_IDLE) {
            start_chat();
        } else if (s_chat == CHAT_SPEAKING) {
            s_turn_id++;
            set_chat(CHAT_LISTENING);
        }
        return;
    }
    // ToggleChatState
    if (s_chat == CHAT_IDLE) start_chat();
    else if (s_chat == CHAT_LISTENING) close_channel("toggle chat");
    else if (s_chat == CHAT_SPEAKING) {  // AbortSpeaking，自动模式接着听
        s_turn_id++;
        set_chat(CHAT_LISTENING);
    }
}

void sim_boot_up(void)
{
    if (!s_holding) return;
    s_holding = false;
    // HandleStopListeningEvent：只处理聆听中（连接中松手不管，录满 15 秒才结束，和固件一样）
    if (s_chat == CHAT_LISTENING) set_chat(CHAT_IDLE);
}

void sim_pointer(int x, int y, int pressed)
{
    s_px = x;
    s_py = y;
    s_pressed = pressed;
}
