#include "nomi_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "face.h"

#define FRAME_MS 33
#define STEP_S (1.0f / 60)
#define EMOTION_HOLD_MS 4000  // LLM 给的情绪保持多久
#define MIN_BRIGHTNESS 10     // 亮度滑条下限，免得调成全黑找不回来

#define C_TEXT 0xe8eaf0
#define C_MUTED 0x8b90a0
#define C_PANEL 0x1b1e26
#define C_CONTROL 0x2c3040
#define C_ACCENT 0x6aa6ff
#define C_DANGER 0xff6b6b

static nomi_ui_callbacks_t s_cb;
static const lv_font_t *s_font;
static uint16_t *s_buf;
static lv_obj_t *s_screen, *s_canvas, *s_status, *s_battery, *s_gear, *s_subtitle, *s_toast;
static lv_obj_t *s_settings;  // 设置页，打开时才创建
static lv_obj_t *s_camera;    // 相机页，打开时才创建
static lv_timer_t *s_toast_timer;
static uint32_t s_last_ms, s_emotion_until;
static float s_acc;
static face_mode_t s_emotion_mode = FACE_IDLE;
static bool s_sleeping, s_subtitles_on = true, s_holding;

static int get_int(const char *key, int def) { return s_cb.get_int ? s_cb.get_int(key, def) : def; }
static void set_int(const char *key, int value, bool final)
{
    if (s_cb.set_int) s_cb.set_int(key, value, final);
}
static const char *get_text(const char *key)
{
    const char *text = s_cb.get_text ? s_cb.get_text(key) : NULL;
    return text ? text : "";
}
static void action(const char *name)
{
    if (s_cb.action) s_cb.action(name);
}

static void close_settings(void);
static void close_camera(void);

// 二维码用 Nayuki 的 qrcodegen：设备上由小智依赖的 esp_emote_gfx 带进来，模拟器里由 LVGL 带进来。
// 两份同名，不能同时链接，所以不打开 LVGL 的 qrcode 控件，只声明这里用到的三个函数。
// 枚举参数按 int 传：ecl 0 为 LOW，mask -1 为 AUTO
bool qrcodegen_encodeText(const char *text, uint8_t tempBuffer[], uint8_t qrcode[], int ecl,
                          int minVersion, int maxVersion, int mask, bool boostEcl);
int qrcodegen_getSize(const uint8_t qrcode[]);
bool qrcodegen_getModule(const uint8_t qrcode[], int x, int y);

/* ---------- 表情 ---------- */

static face_mode_t mode_for_emotion(const char *emotion)
{
    static const char *const happy[] = {"happy", "laughing", "funny",   "loving", "winking", "cool",
                                        "relaxed", "delicious", "kissy", "confident", "silly"};
    static const char *const confused[] = {"confused", "embarrassed", "surprised", "shocked"};
    for (size_t i = 0; i < sizeof(happy) / sizeof(happy[0]); i++) {
        if (strcmp(emotion, happy[i]) == 0) return FACE_HAPPY;
    }
    for (size_t i = 0; i < sizeof(confused) / sizeof(confused[0]); i++) {
        if (strcmp(emotion, confused[i]) == 0) return FACE_CONFUSED;
    }
    if (strcmp(emotion, "thinking") == 0) return FACE_THINK;
    return FACE_IDLE;
}

// 说话最优先，其次是 LLM 刚给的情绪，再看设备状态
static face_mode_t pick_mode(uint32_t now)
{
    nomi_state_t state = s_cb.get_state ? s_cb.get_state() : NOMI_STATE_IDLE;
    if (state == NOMI_STATE_SPEAKING) return FACE_SPEAK;
    if (s_emotion_mode != FACE_IDLE && (int32_t)(s_emotion_until - now) > 0) return s_emotion_mode;
    switch (state) {
    case NOMI_STATE_LISTENING: return FACE_LISTEN;
    case NOMI_STATE_CONNECTING: return FACE_THINK;
    case NOMI_STATE_ERROR: return FACE_CONFUSED;
    default: return FACE_IDLE;
    }
}

static void face_tick(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now = lv_tick_get();
    float dt = (now - s_last_ms) / 1000.0f;
    s_last_ms = now;
    s_acc += dt > 0.1f ? 0.1f : dt;  // 卡顿后别一次追太多步

    face_mode_t mode = pick_mode(now);
    if (mode != face_get_mode()) face_set_mode(mode);
    for (; s_acc >= STEP_S; s_acc -= STEP_S) face_step(STEP_S);

    // 设置页、相机页盖住整屏时不用重画脸
    if (s_settings || s_camera) return;
    // 表情直接画进画布缓冲，只通知 LVGL 重画变化的那块
    rrect_t d = face_render(s_buf);
    if (!rrect_empty(d)) {
        lv_area_t area = {d.x0, d.y0, d.x1 - 1, d.y1 - 1};
        lv_obj_invalidate_area(s_canvas, &area);
    }
}

void nomi_ui_set_emotion(const char *emotion)
{
    if (!emotion) return;
    s_emotion_mode = mode_for_emotion(emotion);
    s_emotion_until = lv_tick_get() + EMOTION_HOLD_MS;
}

/* ---------- 状态栏、字幕、通知 ---------- */

static void set_color(lv_obj_t *obj, uint32_t color)
{
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

// 字体设在几个顶层对象上，设置页里的文字跟着继承
void nomi_ui_set_font(const lv_font_t *font)
{
    if (!font) return;
    s_font = font;
    lv_obj_t *roots[] = {s_status, s_subtitle, s_toast, s_settings};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        if (roots[i]) lv_obj_set_style_text_font(roots[i], font, 0);
    }
    lv_obj_set_height(s_subtitle, lv_font_get_line_height(font) * 2);  // 最多两行
}

void nomi_ui_set_status(const char *status)
{
    lv_label_set_text(s_status, status ? status : "");
}

void nomi_ui_set_subtitle(const char *role, const char *text)
{
    if (!s_subtitles_on || !text || !*text) {
        lv_obj_add_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // 用户说的话偏灰，助手的回答用亮白
    bool from_user = role && strcmp(role, "user") == 0;
    set_color(s_subtitle, from_user ? C_MUTED : C_TEXT);
    lv_label_set_text(s_subtitle, text);
    lv_obj_remove_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
}

void nomi_ui_set_battery(int level, bool charging)
{
    if (level < 0) {
        lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const char *icon = level >= 90 ? LV_SYMBOL_BATTERY_FULL
                     : level >= 65 ? LV_SYMBOL_BATTERY_3
                     : level >= 40 ? LV_SYMBOL_BATTERY_2
                     : level >= 15 ? LV_SYMBOL_BATTERY_1
                                   : LV_SYMBOL_BATTERY_EMPTY;
    lv_label_set_text_fmt(s_battery, "%s%s %d%%", charging ? LV_SYMBOL_CHARGE " " : "", icon, level);
    // 低电量且没在充电时标红
    set_color(s_battery, level < 15 && !charging ? C_DANGER : C_MUTED);
    lv_obj_remove_flag(s_battery, LV_OBJ_FLAG_HIDDEN);
}

void nomi_ui_set_sleep(bool sleeping)
{
    s_sleeping = sleeping;
    face_set_sleep(sleeping);
    if (sleeping) {
        lv_obj_add_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
        close_settings();
        close_camera();
    }
}

static void hide_toast(lv_timer_t *timer)
{
    (void)timer;
    s_toast_timer = NULL;  // 只跑一次的定时器跑完会自己删掉
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

void nomi_ui_notify(const char *text, uint32_t duration_ms)
{
    if (!text || !*text) return;
    lv_label_set_text(s_toast, text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);
    if (s_toast_timer) lv_timer_delete(s_toast_timer);
    s_toast_timer = lv_timer_create(hide_toast, duration_ms, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

/* ---------- 设置页的小部件 ---------- */

// 一个设置项：标题 + 可选说明，控件放在返回的容器里
static lv_obj_t *add_item(lv_obj_t *page, const char *title, const char *hint)
{
    lv_obj_t *item = lv_obj_create(page);
    lv_obj_remove_style_all(item);
    lv_obj_set_size(item, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(item, 6, 0);
    lv_obj_set_style_pad_bottom(item, 12, 0);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(item);
    lv_label_set_text(label, title);
    set_color(label, C_TEXT);
    if (hint) {
        lv_obj_t *h = lv_label_create(item);
        lv_label_set_text(h, hint);
        lv_obj_set_width(h, LV_PCT(100));
        set_color(h, C_MUTED);
    }
    return item;
}

static void on_slider(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    const char *key = lv_event_get_user_data(e);
    set_int(key, lv_slider_get_value(slider), lv_event_get_code(e) == LV_EVENT_RELEASED);
}

static void add_slider(lv_obj_t *page, const char *title, const char *key, int min, int def)
{
    lv_obj_t *item = add_item(page, title, NULL);
    lv_obj_t *slider = lv_slider_create(item);
    lv_obj_set_width(slider, LV_PCT(92));
    lv_obj_set_style_margin_hor(slider, 10, 0);
    lv_slider_set_range(slider, min, 100);
    int value = get_int(key, def);
    lv_slider_set_value(slider, value < min ? min : value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(C_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_add_event_cb(slider, on_slider, LV_EVENT_VALUE_CHANGED, (void *)key);
    lv_obj_add_event_cb(slider, on_slider, LV_EVENT_RELEASED, (void *)key);
}

static void on_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    const char *key = lv_event_get_user_data(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (strcmp(key, NOMI_KEY_SUBTITLES) == 0) {
        s_subtitles_on = on;
        if (!on) lv_obj_add_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
    }
    set_int(key, on, true);
}

// 标题在左、开关在右的一行
static lv_obj_t *add_switch(lv_obj_t *page, const char *title, const char *hint, const char *key,
                            bool enabled)
{
    lv_obj_t *item = add_item(page, title, hint);
    lv_obj_t *sw = lv_switch_create(item);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(sw, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_set_style_bg_color(sw, lv_color_hex(C_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (get_int(key, 0)) lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (!enabled) lv_obj_add_state(sw, LV_STATE_DISABLED);
    lv_obj_add_event_cb(sw, on_switch, LV_EVENT_VALUE_CHANGED, (void *)key);
    return sw;
}

static void on_choice(lv_event_t *e)
{
    lv_obj_t *btnm = lv_event_get_target(e);
    const char *key = lv_event_get_user_data(e);
    uint32_t id = lv_buttonmatrix_get_selected_button(btnm);
    if (id != LV_BUTTONMATRIX_BUTTON_NONE) set_int(key, (int)id, true);
}

// 单选按钮组。map 按 LVGL 格式（"\n" 换行，"" 结尾），要在设置页存在期间一直有效
static void add_choice(lv_obj_t *page, const char *title, const char *hint, const char *key,
                       const char *const *map, int rows)
{
    lv_obj_t *item = add_item(page, title, hint);
    lv_obj_t *btnm = lv_buttonmatrix_create(item);
    lv_obj_set_style_text_font(btnm, s_font, 0);
    lv_buttonmatrix_set_map(btnm, map);
    lv_buttonmatrix_set_button_ctrl_all(btnm, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(btnm, true);
    int selected = get_int(key, -1);
    if (selected >= 0) lv_buttonmatrix_set_button_ctrl(btnm, selected, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_set_size(btnm, LV_PCT(100), rows * 44);
    lv_obj_set_style_bg_opa(btnm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnm, 0, 0);
    lv_obj_set_style_pad_all(btnm, 0, 0);
    lv_obj_set_style_pad_gap(btnm, 6, 0);
    lv_obj_set_style_bg_color(btnm, lv_color_hex(C_CONTROL), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(btnm, lv_color_hex(C_ACCENT), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(btnm, lv_color_hex(C_TEXT), LV_PART_ITEMS);
    lv_obj_set_style_text_color(btnm, lv_color_hex(0x0b1220), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_radius(btnm, 10, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(btnm, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(btnm, on_choice, LV_EVENT_VALUE_CHANGED, (void *)key);
}

// 把 '\n' 分隔的列表拆成按钮组的 map，每行 per_row 个；返回行数
static int build_map(const char *list, char *buf, size_t buf_size, const char **map, int max_items,
                     int per_row)
{
    snprintf(buf, buf_size, "%s", list);
    int n = 0, slots = 0, rows = buf[0] ? 1 : 0;
    char *p = buf;
    while (*p && n < max_items && slots < max_items * 2 - 2) {
        if (n > 0 && n % per_row == 0) {
            map[slots++] = "\n";
            rows++;
        }
        map[slots++] = p;
        n++;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = '\0';
        p = nl + 1;
    }
    map[slots] = "";
    return rows;
}

// 二维码画进一张画布，四周留两格白边方便扫
#define QR_MAX_PX 132
static lv_obj_t *add_qrcode(lv_obj_t *parent, const char *text)
{
    static uint8_t qr[512], tmp[512];  // 版本 10 以内够用，网址很短
    static uint16_t *pixels;           // 画布缓冲常驻，不随设置页释放（删除是异步的）
    if (!qrcodegen_encodeText(text, tmp, qr, 0, 1, 10, -1, true)) return NULL;
    if (!pixels) pixels = malloc(QR_MAX_PX * QR_MAX_PX * sizeof(uint16_t));
    if (!pixels) return NULL;

    const int border = 2, cells = qrcodegen_getSize(qr) + border * 2;
    const int scale = QR_MAX_PX / cells, size = cells * scale;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            bool dark = qrcodegen_getModule(qr, x / scale - border, y / scale - border);
            pixels[y * size + x] = dark ? 0x0000 : 0xFFFF;
        }
    }
    lv_obj_t *canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, pixels, size, size, LV_COLOR_FORMAT_RGB565);
    return canvas;
}

static void add_info(lv_obj_t *page, const char *title, const char *value)
{
    lv_obj_t *item = add_item(page, title, NULL);
    lv_obj_t *label = lv_label_create(item);
    lv_label_set_text(label, value[0] ? value : "—");
    lv_obj_set_width(label, LV_PCT(100));
    set_color(label, C_MUTED);
}

// 危险操作要点两次：第一次只把按钮文字改成"再点一次确认"
static void on_action_button(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *name = lv_event_get_user_data(e);
    bool dangerous = strcmp(name, "factory_reset") == 0 || strcmp(name, "reconfigure_wifi") == 0;
    if (dangerous && !lv_obj_has_state(btn, LV_STATE_USER_1)) {
        lv_obj_add_state(btn, LV_STATE_USER_1);
        lv_label_set_text(lv_obj_get_child(btn, 0), "再点一次确认");
        return;
    }
    if (dangerous) close_settings();
    action(name);
}

static void add_action(lv_obj_t *parent, const char *text, const char *name, bool danger)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(danger ? 0x4a2a30 : C_CONTROL), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 14, 0);
    lv_obj_set_style_pad_ver(btn, 10, 0);
    lv_obj_add_event_cb(btn, on_action_button, LV_EVENT_CLICKED, (void *)name);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    set_color(label, danger ? C_DANGER : C_TEXT);
}

static lv_obj_t *add_button_row(lv_obj_t *page)
{
    lv_obj_t *row = lv_obj_create(page);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(row, 8, 0);
    lv_obj_set_style_pad_bottom(row, 12, 0);
    return row;
}

/* ---------- 设置页 ---------- */

static lv_obj_t *add_page(lv_obj_t *tabview, const char *name)
{
    lv_obj_t *page = lv_tabview_add_tab(tabview, name);
    lv_obj_set_style_text_font(page, s_font, 0);  // 默认主题会给页面设自带字体，挡住中文
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 16, 0);
    lv_obj_set_style_pad_right(page, 20, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_ACTIVE);
    return page;
}

static void build_chat_page(lv_obj_t *page)
{
    static const char *const modes[] = {"点一下开始", "按住说话", ""};
    add_choice(page, "对话方式", "主界面右滑或点左上角齿轮进设置，左滑进相机", NOMI_KEY_CHAT_MODE,
               modes, 1);
    bool aec_ok = get_int(NOMI_KEY_AEC_SUPPORTED, 0);
    add_switch(page, "随时插话打断",
               aec_ok ? "它说话时直接开口就能打断" : "这块板没有喇叭回采，不支持", NOMI_KEY_AEC,
               aec_ok);
    add_switch(page, "显示字幕", NULL, NOMI_KEY_SUBTITLES, true);
    add_info(page, "唤醒词", "你好小智");
}

static void build_role_page(lv_obj_t *page)
{
    static char persona_buf[512], voice_buf[256];
    static const char *persona_map[24], *voice_map[24];
    int rows = build_map(get_text("personas"), persona_buf, sizeof(persona_buf), persona_map, 12, 2);
    if (rows) add_choice(page, "人设", NULL, NOMI_KEY_PERSONA, persona_map, rows);
    rows = build_map(get_text("voices"), voice_buf, sizeof(voice_buf), voice_map, 12, 3);
    if (rows) {
        add_choice(page, "音色", "OpenAI 的音色名，别家的音色请在网页设置页里填", NOMI_KEY_VOICE,
                   voice_map, rows);
    }
    add_info(page, "自定义", "人设提示词、API 地址和 Key 在网页设置页里改，地址见「网络」");
}

static void build_display_page(lv_obj_t *page)
{
    static const char *const dim[] = {"30 秒", "1 分", "2 分", "5 分", "从不", ""};
    static const char *const off[] = {"2 分", "5 分", "10 分", "30 分", "从不", ""};
    add_slider(page, "亮度", NOMI_KEY_BRIGHTNESS, MIN_BRIGHTNESS, 80);
    add_choice(page, "多久后闭眼调暗", NULL, NOMI_KEY_DIM, dim, 1);
    add_choice(page, "多久后息屏", NULL, NOMI_KEY_OFF, off, 1);
}

static void build_network_page(lv_obj_t *page)
{
    add_info(page, "Wi-Fi", get_text("wifi"));
    add_info(page, "IP 地址", get_text("ip"));
    const char *url = get_text("config_url");
    lv_obj_t *item = add_item(page, "网页设置页", url[0] ? "手机扫码打开，填写 API 地址和 Key" : "还没连上网络");
    if (url[0]) {
        add_qrcode(item, url);
        lv_obj_t *label = lv_label_create(item);
        lv_label_set_text(label, url);
        set_color(label, C_MUTED);
    }
    add_action(add_button_row(page), "重新配网", "reconfigure_wifi", true);
}

static void build_system_page(lv_obj_t *page)
{
    add_info(page, "版本", get_text("version"));
    lv_obj_t *row = add_button_row(page);
    add_action(row, "测试 API", "test_api", false);
    add_action(add_button_row(page), "恢复出厂设置", "factory_reset", true);
}

static void on_close(lv_event_t *e)
{
    (void)e;
    close_settings();
}

static void close_settings(void)
{
    if (!s_settings) return;
    lv_obj_delete_async(s_settings);  // 可能正处在设置页自己的事件回调里
    s_settings = NULL;
    lv_obj_invalidate(s_canvas);
}

// 每次打开都重建，保证显示的是最新的值
static void open_settings(void)
{
    if (s_settings) return;
    close_camera();
    s_settings = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_settings);
    lv_obj_set_size(s_settings, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_settings, lv_color_hex(0x111318), 0);
    lv_obj_set_style_bg_opa(s_settings, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_settings, s_font, 0);
    set_color(s_settings, C_TEXT);
    lv_obj_add_flag(s_settings, LV_OBJ_FLAG_CLICKABLE);  // 挡住下面的脸

    lv_obj_t *tv = lv_tabview_create(s_settings);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    lv_tabview_set_tab_bar_position(tv, LV_DIR_LEFT);
    lv_tabview_set_tab_bar_size(tv, 92);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    // 默认主题会给标签页各层设自带字体，挡住从外面继承的中文字体
    lv_obj_set_style_text_font(tv, s_font, 0);
    lv_obj_set_style_text_font(bar, s_font, 0);
    lv_obj_set_style_text_font(lv_tabview_get_content(tv), s_font, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_pad_top(bar, 52, 0);  // 给左上角的关闭按钮留位置

    build_chat_page(add_page(tv, "对话"));
    build_role_page(add_page(tv, "角色"));
    build_display_page(add_page(tv, "显示"));
    lv_obj_t *sound = add_page(tv, "声音");
    add_slider(sound, "音量", NOMI_KEY_VOLUME, 0, 70);
    build_network_page(add_page(tv, "网络"));
    build_system_page(add_page(tv, "系统"));

    // 标签按钮：未选中灰字，选中亮字加左侧色条
    for (uint32_t i = 0; i < lv_obj_get_child_count(bar); i++) {
        lv_obj_t *btn = lv_obj_get_child(bar, i);
        lv_obj_set_style_text_font(btn, s_font, 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(C_MUTED), 0);
        lv_obj_set_style_text_color(btn, lv_color_hex(C_TEXT), LV_STATE_CHECKED);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(C_ACCENT), LV_STATE_CHECKED);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_LEFT, LV_STATE_CHECKED);
        lv_obj_set_style_border_color(btn, lv_color_hex(C_ACCENT), LV_STATE_CHECKED);
        lv_obj_set_style_border_width(btn, 3, LV_STATE_CHECKED);
    }

    lv_obj_t *close = lv_button_create(s_settings);
    lv_obj_set_size(close, 76, 40);
    lv_obj_align(close, LV_ALIGN_TOP_LEFT, 8, 6);
    lv_obj_set_style_bg_color(close, lv_color_hex(C_CONTROL), 0);
    lv_obj_set_style_radius(close, 10, 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_add_event_cb(close, on_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(close);
    lv_label_set_text(label, "返回");
    lv_obj_center(label);
}

/* ---------- 相机页（主界面左滑）：拍照给大模型看 ---------- */

static void close_camera(void)
{
    if (!s_camera) return;
    lv_obj_delete_async(s_camera);
    s_camera = NULL;
    lv_obj_invalidate(s_canvas);
}

static void on_camera_close(lv_event_t *e)
{
    (void)e;
    close_camera();
}

// 拍完回到主界面：屏幕先预览拍到的画面，接着开始聆听，问题和照片一起发给大模型
static void on_shutter(lv_event_t *e)
{
    (void)e;
    close_camera();
    action("take_photo");
}

static void open_camera(void)
{
    if (s_camera || s_settings) return;
    s_camera = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_camera);
    lv_obj_set_size(s_camera, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_camera, lv_color_hex(0x111318), 0);
    lv_obj_set_style_bg_opa(s_camera, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_camera, s_font, 0);
    set_color(s_camera, C_TEXT);
    lv_obj_add_flag(s_camera, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hint = lv_label_create(s_camera);
    lv_label_set_text(hint, "拍一张，然后说出你的问题\n它会看着照片回答");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    set_color(hint, C_MUTED);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 64);

    // 快门：白色实心圆，外面一圈白环
    lv_obj_t *shutter = lv_button_create(s_camera);
    lv_obj_set_size(shutter, 96, 96);
    lv_obj_align(shutter, LV_ALIGN_CENTER, 0, 24);
    lv_obj_set_style_radius(shutter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(shutter, lv_color_white(), 0);
    lv_obj_set_style_bg_color(shutter, lv_color_hex(0xc8ccd6), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(shutter, lv_color_hex(0x111318), 0);
    lv_obj_set_style_border_width(shutter, 6, 0);
    lv_obj_set_style_outline_color(shutter, lv_color_white(), 0);
    lv_obj_set_style_outline_width(shutter, 4, 0);
    lv_obj_set_style_shadow_width(shutter, 0, 0);
    lv_obj_add_event_cb(shutter, on_shutter, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close = lv_button_create(s_camera);
    lv_obj_set_size(close, 76, 40);
    lv_obj_align(close, LV_ALIGN_TOP_LEFT, 8, 6);
    lv_obj_set_style_bg_color(close, lv_color_hex(C_CONTROL), 0);
    lv_obj_set_style_radius(close, 10, 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_add_event_cb(close, on_camera_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(close);
    lv_label_set_text(label, "返回");
    lv_obj_center(label);
}

/* ---------- 入口 ---------- */

static void on_face(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    static bool press_woke;  // 这一下是不是用来叫醒它的
    bool hold_mode = get_int(NOMI_KEY_CHAT_MODE, 0) == 1;

    if (code == LV_EVENT_PRESSED) {
        press_woke = s_sleeping;
        if (s_cb.user_activity) s_cb.user_activity();  // 设备上会同步退出休眠
        if (!press_woke && hold_mode && s_cb.start_listening) {
            s_holding = true;
            s_cb.start_listening();
        }
        return;
    }
    if (code == LV_EVENT_RELEASED) {
        if (s_holding && s_cb.stop_listening) s_cb.stop_listening();
        s_holding = false;
        return;
    }
    if (press_woke) return;
    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
        lv_indev_wait_release(lv_indev_active());  // 这次抬手不再算点击
        if (s_holding && s_cb.stop_listening) s_cb.stop_listening();  // 按住说话时滑走就算了
        s_holding = false;
        if (dir == LV_DIR_LEFT) open_camera();
        else if (dir == LV_DIR_RIGHT) open_settings();
        return;
    }
    if (hold_mode) return;
    if (code == LV_EVENT_SHORT_CLICKED) {
        if (s_cb.toggle_chat) s_cb.toggle_chat();
    } else if (code == LV_EVENT_LONG_PRESSED) {
        open_settings();
    }
}

static void on_gear(lv_event_t *e)
{
    (void)e;
    if (s_cb.user_activity) s_cb.user_activity();
    if (!s_sleeping) open_settings();
}

void nomi_ui_create(lv_obj_t *screen, uint16_t *face_buf, const lv_font_t *font,
                    const nomi_ui_callbacks_t *callbacks)
{
    s_screen = screen;
    s_buf = face_buf;
    s_font = font;
    s_cb = *callbacks;
    s_subtitles_on = get_int(NOMI_KEY_SUBTITLES, 1);

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    face_init(lv_tick_get() * 2654435761u + 1);
    face_render(s_buf);  // 第一帧整屏画好
    s_canvas = lv_canvas_create(screen);
    lv_canvas_set_buffer(s_canvas, s_buf, FACE_W, FACE_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, on_face, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_canvas, on_face, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_canvas, on_face, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_canvas, on_face, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_canvas, on_face, LV_EVENT_GESTURE, NULL);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_GESTURE_BUBBLE);  // 默认手势交给父对象，这里自己接

    s_status = lv_label_create(screen);
    set_color(s_status, C_MUTED);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 8);
    lv_label_set_text(s_status, "");

    // 左上角齿轮：任何模式下都能进设置
    s_gear = lv_label_create(screen);
    lv_obj_set_style_text_font(s_gear, &lv_font_montserrat_14, 0);
    set_color(s_gear, C_MUTED);
    lv_label_set_text(s_gear, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_pad_all(s_gear, 12, 0);
    lv_obj_align(s_gear, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_flag(s_gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_gear, on_gear, LV_EVENT_CLICKED, NULL);

    s_battery = lv_label_create(screen);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_14, 0);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);

    s_subtitle = lv_label_create(screen);
    set_color(s_subtitle, C_TEXT);
    lv_obj_set_width(s_subtitle, FACE_W - 40);
    lv_label_set_long_mode(s_subtitle, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_subtitle, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);

    s_toast = lv_label_create(screen);
    set_color(s_toast, C_TEXT);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 12, 0);
    lv_obj_set_style_pad_hor(s_toast, 14, 0);
    lv_obj_set_style_pad_ver(s_toast, 8, 0);
    lv_obj_set_style_max_width(s_toast, FACE_W - 40, 0);
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    nomi_ui_set_font(font);

    s_last_ms = lv_tick_get();
    lv_timer_create(face_tick, FRAME_MS, NULL);
}
