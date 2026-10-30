// 设备模拟器：LVGL + 固件的 nomi_ui / face 代码编成 WebAssembly，由 sim.html 驱动。
//
// LVGL 刷屏写进 s_lcd（相当于屏幕显存），鼠标当触摸。
// 对话流程是假的：点一下开始聆听，再点一下（或 4 秒后）进入思考，然后念两句示例台词，
// 用来看整套交互；真正的 API 调用只在设备上跑。
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

/* ---------- 假的设备状态 ---------- */

typedef enum { PHASE_IDLE, PHASE_LISTEN, PHASE_THINK, PHASE_SAY1, PHASE_SAY2, PHASE_DONE } phase_t;

static phase_t s_phase = PHASE_IDLE;
static bool s_photo;  // 这一轮带了照片
static uint32_t s_phase_at;
static int s_volume = 70, s_brightness = 80;

// 设置项存在内存里，刷新页面就没了
static int s_chat_mode, s_aec, s_subtitles = 1, s_persona, s_voice = 4, s_dim = 1, s_off = 1;

// 休眠：时间按设置页的选项，默认 1 分钟闭眼调暗、5 分钟息屏
#define DIM_BRIGHTNESS 5
static uint32_t s_last_activity;
static int s_sleep_level;  // 0 醒着，1 调暗，2 息屏

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
    nomi_ui_set_sleep(true);
}

static void enter(phase_t phase)
{
    s_phase = phase;
    s_phase_at = s_ms;
    switch (phase) {
    case PHASE_IDLE:
        nomi_ui_set_status("待机");
        break;
    case PHASE_LISTEN:
        nomi_ui_set_status("聆听中…");
        nomi_ui_set_subtitle(NULL, NULL);
        break;
    case PHASE_THINK:
        nomi_ui_set_status("思考中…");
        nomi_ui_set_subtitle("user", s_photo ? "这是什么植物？" : "今天适合出去走走吗？");
        nomi_ui_set_emotion("thinking");
        break;
    case PHASE_SAY1:
        nomi_ui_set_status("说话中…");
        nomi_ui_set_emotion("happy");
        nomi_ui_set_subtitle("assistant", s_photo ? "这是一盆绿萝，叶子很精神，看得出你照顾得不错。"
                                                  : "当然适合！外面阳光正好，气温也舒服。");
        break;
    case PHASE_SAY2:
        nomi_ui_set_subtitle("assistant", s_photo ? "它喜欢散射光，一周浇一次水就够了。"
                                                  : "记得带瓶水，傍晚可能会起风哦。");
        break;
    case PHASE_DONE:
        nomi_ui_set_status("待机");
        nomi_ui_set_emotion("happy");
        break;
    }
}

static void advance(void)
{
    if (s_phase != PHASE_IDLE) {
        s_last_activity = s_ms;  // 对话中不算空闲
    } else {
        static const int dim_s[] = NOMI_DIM_SECONDS, off_s[] = NOMI_OFF_SECONDS;
        uint32_t idle = s_ms - s_last_activity;
        if (s_sleep_level == 0 && dim_s[s_dim] > 0 && idle > (uint32_t)dim_s[s_dim] * 1000) sleep_now();
        else if (s_sleep_level == 1 && off_s[s_off] > 0 && idle > (uint32_t)off_s[s_off] * 1000) s_sleep_level = 2;
    }
    uint32_t t = s_ms - s_phase_at;
    if (s_phase == PHASE_LISTEN && t > 4000) enter(PHASE_THINK);
    else if (s_phase == PHASE_THINK && t > 1500) enter(PHASE_SAY1);
    else if (s_phase == PHASE_SAY1 && t > 2800) enter(PHASE_SAY2);
    else if (s_phase == PHASE_SAY2 && t > 2800) enter(PHASE_DONE);
    else if (s_phase == PHASE_DONE && t > 3000) {
        nomi_ui_set_subtitle(NULL, NULL);
        s_phase = PHASE_IDLE;
        s_photo = false;
    }
}

static nomi_state_t get_state(void)
{
    switch (s_phase) {
    case PHASE_LISTEN: case PHASE_THINK: return NOMI_STATE_LISTENING;
    case PHASE_SAY1: case PHASE_SAY2: return NOMI_STATE_SPEAKING;
    default: return NOMI_STATE_IDLE;
    }
}

// 和小智的 ToggleChatState 对应：待机→聆听，聆听→结束说话，说话中→打断
static void toggle_chat(void)
{
    wake();
    if (s_phase == PHASE_IDLE || s_phase == PHASE_DONE) enter(PHASE_LISTEN);
    else if (s_phase == PHASE_LISTEN) enter(PHASE_THINK);
    else enter(PHASE_IDLE);
}

static int get_int(const char *key, int def)
{
    if (!strcmp(key, NOMI_KEY_VOLUME)) return s_volume;
    if (!strcmp(key, NOMI_KEY_BRIGHTNESS)) return s_brightness;
    if (!strcmp(key, NOMI_KEY_CHAT_MODE)) return s_chat_mode;
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
    if (!strcmp(key, NOMI_KEY_VOLUME)) s_volume = value;
    else if (!strcmp(key, NOMI_KEY_BRIGHTNESS)) s_brightness = value;
    else if (!strcmp(key, NOMI_KEY_CHAT_MODE)) s_chat_mode = value;
    else if (!strcmp(key, NOMI_KEY_AEC)) s_aec = value;
    else if (!strcmp(key, NOMI_KEY_SUBTITLES)) s_subtitles = value;
    else if (!strcmp(key, NOMI_KEY_PERSONA)) s_persona = value;
    else if (!strcmp(key, NOMI_KEY_VOICE)) s_voice = value;
    else if (!strcmp(key, NOMI_KEY_DIM)) s_dim = value;
    else if (!strcmp(key, NOMI_KEY_OFF)) s_off = value;
}

static const char *get_text(const char *key)
{
    if (!strcmp(key, "personas")) return "桌面小伙伴\n毒舌损友\n学习助手\n英语陪练";
    if (!strcmp(key, "voices")) return "alloy\necho\nfable\nonyx\nnova\nshimmer";
    if (!strcmp(key, "wifi")) return "Home-5G（模拟）";
    if (!strcmp(key, "ip")) return "192.168.1.88";
    if (!strcmp(key, "config_url")) return "http://192.168.1.88/";
    if (!strcmp(key, "version")) return "Pocket Companion 模拟器（LVGL 9.5）";
    return "";
}

static void action(const char *name)
{
    if (!strcmp(name, "take_photo")) {
        // 真机上会先在屏幕上预览拍到的画面，再开始聆听；问题和照片一起发给大模型
        s_photo = true;
        nomi_ui_notify("（模拟）拍好了，说出你的问题", 3000);
        wake();
        enter(PHASE_LISTEN);
    }
    else if (!strcmp(name, "test_api")) nomi_ui_notify("（模拟）大模型正常 · 语音合成支持 opus", 4000);
    else if (!strcmp(name, "reconfigure_wifi")) nomi_ui_notify("（模拟）设备重启进入配网模式", 4000);
    else if (!strcmp(name, "factory_reset")) nomi_ui_notify("（模拟）已恢复出厂设置，设备重启", 4000);
}

// 按住说话：按下开始聆听，松开进入思考
static void start_listening(void) { wake(); enter(PHASE_LISTEN); }
static void stop_listening(void)
{
    if (s_phase == PHASE_LISTEN) enter(PHASE_THINK);
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

/* ---------- 导出给 JS ---------- */

static uint8_t *s_font_bin;

// JS 把小智的字体文件（和设备同一个 font_noto_sans_common_20_4.bin）拷进这块内存
uint8_t *sim_font_buffer(int size)
{
    s_font_bin = malloc(size);
    return s_font_bin;
}

void sim_init(uint32_t ms)
{
    s_ms = ms;
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
        .toggle_chat = toggle_chat,
        .start_listening = start_listening,
        .stop_listening = stop_listening,
        .user_activity = wake,
        .get_int = get_int,
        .set_int = set_int,
        .get_text = get_text,
        .action = action,
    };
    // 没拿到小智字体时退回 LVGL 自带的中文子集（缺字较多）
    const lv_font_t *font = s_font_bin ? cbin_font_create(s_font_bin) : NULL;
    nomi_ui_create(lv_screen_active(), s_face, font ? font : &lv_font_source_han_sans_sc_16_cjk,
                   &callbacks);
    enter(PHASE_IDLE);
    nomi_ui_set_battery(76, false);  // 假电量
    s_last_activity = ms;
}

// 推进到 ms 时刻跑一次 LVGL；返回本次刷到"屏幕"的面积
int sim_frame(uint32_t ms)
{
    s_ms = ms;
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
    return s_sleep_level == 2 ? 0 : s_sleep_level == 1 ? DIM_BRIGHTNESS : s_brightness;
}
void sim_force_sleep(void) { sleep_now(); }
int sim_volume(void) { return s_volume; }
void sim_boot_button(void) { toggle_chat(); }

void sim_pointer(int x, int y, int pressed)
{
    s_px = x;
    s_py = y;
    s_pressed = pressed;
}
