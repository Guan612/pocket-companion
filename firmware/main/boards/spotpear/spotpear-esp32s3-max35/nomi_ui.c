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
#define C_PAGE 0x111318

static nomi_ui_callbacks_t s_cb;
static const lv_font_t *s_font;
static uint16_t *s_buf;

// 页面结构：外层上下翻（设置 / 主界面），主界面里左右翻（Nomi / 视觉），状态栏和字幕浮在主界面上
static lv_obj_t *s_outer, *s_settings_tile, *s_main_tile;
static lv_obj_t *s_modes, *s_chat_tile, *s_vision_tile;
static lv_obj_t *s_canvas, *s_mini_face, *s_preview, *s_preview_hint;
static lv_obj_t *s_status, *s_mode_label, *s_battery, *s_subtitle, *s_toast;
static lv_draw_buf_t *s_preview_buf;
static lv_timer_t *s_toast_timer;
static uint32_t s_last_ms, s_emotion_until;
static float s_acc;
static face_mode_t s_emotion_mode = FACE_IDLE;
static bool s_sleeping, s_subtitles_on = true;
static int s_mode = NOMI_MODE_CHAT;
static bool s_mode_by_code;  // 代码切换模式时不当作用户选择保存

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
static void user_activity(void)
{
    if (s_cb.user_activity) s_cb.user_activity();
}

static void show_main(bool anim);
static void show_mode(int mode, bool anim);
static void on_face_pressed(lv_event_t *e);

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

// 睡着时脸的右上方飘起三个 z：从下往上、由小到大，淡入淡出，错开循环
#define ZZZ_COUNT 3
#define ZZZ_PERIOD_MS 2400
static lv_obj_t *s_zzz[ZZZ_COUNT];

static void update_zzz(uint32_t now)
{
    for (int i = 0; i < ZZZ_COUNT; i++) {
        if (!s_sleeping) {
            lv_obj_add_flag(s_zzz[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        float phase = (float)((now + i * ZZZ_PERIOD_MS / ZZZ_COUNT) % ZZZ_PERIOD_MS) / ZZZ_PERIOD_MS;
        float fade = phase < 0.2f ? phase / 0.2f : (phase > 0.7f ? (1 - phase) / 0.3f : 1);
        lv_obj_set_pos(s_zzz[i], 318 + (int)(phase * 34), 112 - (int)(phase * 64));
        lv_obj_set_style_text_opa(s_zzz[i], (lv_opa_t)(fade * 200), 0);
        lv_obj_set_style_transform_scale(s_zzz[i], (int32_t)(256 + phase * 256), 0);  // 256 为原大小
        lv_obj_remove_flag(s_zzz[i], LV_OBJ_FLAG_HIDDEN);
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

    // 待机和睡着时不显示状态文字，脸本身就说明了
    nomi_state_t state = s_cb.get_state ? s_cb.get_state() : NOMI_STATE_IDLE;
    lv_obj_set_flag(s_status, LV_OBJ_FLAG_HIDDEN, state == NOMI_STATE_IDLE || s_sleeping);
    update_zzz(now);

    // 表情直接画进画布缓冲，只通知 LVGL 重画变化的那块；视觉模式角落的小脸用的是同一块缓冲
    rrect_t d = face_render(s_buf);
    if (!rrect_empty(d)) {
        lv_area_t area = {d.x0, d.y0, d.x1 - 1, d.y1 - 1};
        lv_obj_invalidate_area(s_canvas, &area);
        if (s_mode == NOMI_MODE_VISION) lv_obj_invalidate(s_mini_face);
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

// 字体设在几个顶层对象上，里面的文字跟着继承
void nomi_ui_set_font(const lv_font_t *font)
{
    if (!font) return;
    s_font = font;
    lv_obj_t *roots[] = {s_status, s_mode_label, s_subtitle, s_toast, s_settings_tile, s_vision_tile};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        if (roots[i]) lv_obj_set_style_text_font(roots[i], font, 0);
    }
    for (int i = 0; i < ZZZ_COUNT; i++) lv_obj_set_style_text_font(s_zzz[i], font, 0);
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

// 睡着时回到主界面、显示大脸（取景也停了）；醒来回到原来的模式
void nomi_ui_set_sleep(bool sleeping)
{
    s_sleeping = sleeping;
    face_set_sleep(sleeping);
    if (sleeping) {
        lv_obj_add_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
        show_main(false);
        if (s_mode == NOMI_MODE_VISION) {
            s_mode_by_code = true;
            lv_tileview_set_tile(s_modes, s_chat_tile, LV_ANIM_OFF);
            s_mode_by_code = false;
        }
    } else if (s_mode == NOMI_MODE_VISION) {
        show_mode(NOMI_MODE_VISION, false);
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

/* ---------- 视觉模式：实时取景 ---------- */

void nomi_ui_set_preview(const uint16_t *pixels)
{
    if (!s_preview_buf || !pixels) return;
    memcpy(s_preview_buf->data, pixels, NOMI_PREVIEW_W * NOMI_PREVIEW_H * sizeof(uint16_t));
    lv_obj_add_flag(s_preview_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_preview);
}

static void build_vision(void)
{
    // 取景画面 320×240，放大 1.5 倍铺满 480×320（上下各裁掉一点）
    s_preview_buf = lv_draw_buf_create(NOMI_PREVIEW_W, NOMI_PREVIEW_H, LV_COLOR_FORMAT_RGB565, 0);
    if (s_preview_buf) {
        memset(s_preview_buf->data, 0, s_preview_buf->data_size);
        s_preview = lv_image_create(s_vision_tile);
        lv_image_set_src(s_preview, s_preview_buf);
        lv_image_set_scale(s_preview, 384);  // 256 为原大小
        lv_obj_center(s_preview);
    }

    s_preview_hint = lv_label_create(s_vision_tile);
    lv_label_set_text(s_preview_hint, "正在打开摄像头…");
    set_color(s_preview_hint, C_MUTED);
    lv_obj_center(s_preview_hint);

    // 角落里的小号 Nomi：直接缩放显示大脸那块缓冲，状态和大脸完全同步
    s_mini_face = lv_image_create(s_vision_tile);
    lv_image_set_src(s_mini_face, lv_canvas_get_draw_buf(s_canvas));
    lv_image_set_scale(s_mini_face, 72);  // 约 135×90
    lv_image_set_inner_align(s_mini_face, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_size(s_mini_face, 136, 90);
    lv_obj_set_style_radius(s_mini_face, 14, 0);
    lv_obj_set_style_clip_corner(s_mini_face, true, 0);
    lv_obj_set_style_border_color(s_mini_face, lv_color_hex(C_CONTROL), 0);
    lv_obj_set_style_border_width(s_mini_face, 2, 0);
    lv_obj_align(s_mini_face, LV_ALIGN_BOTTOM_RIGHT, -10, -10);

    lv_obj_add_flag(s_vision_tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_vision_tile, on_face_pressed, LV_EVENT_PRESSED, NULL);
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
    static uint16_t *pixels;           // 画布缓冲常驻，设置页重建时不用重新分配
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
    if (dangerous) show_main(true);
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

/* ---------- 设置页（顶部下拉） ---------- */

static lv_obj_t *add_page(lv_obj_t *tabview, const char *name)
{
    lv_obj_t *page = lv_tabview_add_tab(tabview, name);
    lv_obj_set_style_text_font(page, s_font, 0);  // 默认主题会给页面设自带字体，挡住中文
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(page, 16, 0);
    lv_obj_set_style_pad_right(page, 20, 0);
    lv_obj_set_style_pad_bottom(page, 28, 0);  // 给底部的收起提示留位置
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_ACTIVE);
    return page;
}

static void build_chat_page(lv_obj_t *page)
{
    static const char *const talk[] = {"按住说话", "点一下开始", ""};
    add_choice(page, "BOOT 键", "也可以直接喊「你好小智」", NOMI_KEY_TALK_KEY, talk, 1);
    bool aec_ok = get_int(NOMI_KEY_AEC_SUPPORTED, 0);
    add_switch(page, "随时插话打断",
               aec_ok ? "它说话时直接开口就能打断" : "这块板没有喇叭回采，不支持", NOMI_KEY_AEC,
               aec_ok);
    add_switch(page, "显示字幕", NULL, NOMI_KEY_SUBTITLES, true);
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
    add_info(page, "自定义", "人设提示词、API 地址和 Key、视觉模式用的模型，都在网页设置页里改");
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
    add_action(add_button_row(page), "测试 API", "test_api", false);
    add_action(add_button_row(page), "恢复出厂设置", "factory_reset", true);
}

static void on_grip_clicked(lv_event_t *e)
{
    (void)e;
    show_main(true);
}

// 设置页每次从主界面拉下来时重建，保证显示的是最新的值
static void build_settings(void)
{
    lv_obj_clean(s_settings_tile);

    lv_obj_t *tv = lv_tabview_create(s_settings_tile);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    lv_tabview_set_tab_bar_position(tv, LV_DIR_LEFT);
    lv_tabview_set_tab_bar_size(tv, 92);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_t *bar = lv_tabview_get_tab_bar(tv);
    lv_obj_t *content = lv_tabview_get_content(tv);
    // 默认主题会给标签页各层设自带字体，挡住从外面继承的中文字体
    lv_obj_set_style_text_font(tv, s_font, 0);
    lv_obj_set_style_text_font(bar, s_font, 0);
    lv_obj_set_style_text_font(content, s_font, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(C_PANEL), 0);
    // 分类只靠左边标签切换；内容区不横滑，免得和翻页抢手势
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

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

    // 底部一条把手区：在这里往上推或点一下收起设置。内容区本身能上下滚动，推它是翻内容
    lv_obj_t *grip_zone = lv_obj_create(s_settings_tile);
    lv_obj_remove_style_all(grip_zone);
    lv_obj_set_size(grip_zone, LV_PCT(100), 28);
    lv_obj_align(grip_zone, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_pad_left(grip_zone, 92, 0);  // 横条居中在内容区，避开左边的标签栏
    lv_obj_add_flag(grip_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(grip_zone, on_grip_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *grip = lv_obj_create(grip_zone);
    lv_obj_remove_style_all(grip);
    lv_obj_set_size(grip, 44, 4);
    lv_obj_set_style_radius(grip, 2, 0);
    lv_obj_set_style_bg_color(grip, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_60, 0);
    lv_obj_align(grip, LV_ALIGN_CENTER, 0, 0);
}

/* ---------- 翻页 ---------- */

static void show_main(bool anim)
{
    lv_tileview_set_tile(s_outer, s_main_tile, anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

static void show_mode(int mode, bool anim)
{
    s_mode_by_code = true;
    lv_tileview_set_tile(s_modes, mode == NOMI_MODE_VISION ? s_vision_tile : s_chat_tile,
                         anim ? LV_ANIM_ON : LV_ANIM_OFF);
    s_mode_by_code = false;
}

static void update_mode_label(void)
{
    lv_label_set_text(s_mode_label, s_mode == NOMI_MODE_VISION ? "视觉模式" : "");
}

// 外层开始上下翻：从主界面往下拉时先把设置页刷新成最新的值
static void on_outer_scroll(lv_event_t *e)
{
    (void)e;
    user_activity();
    if (lv_tileview_get_tile_active(s_outer) == s_main_tile) build_settings();
}

// 左右翻到另一个模式：记下来并告诉设备（开关取景、提问时要不要带照片）
static void on_mode_changed(lv_event_t *e)
{
    (void)e;
    if (s_mode_by_code) return;
    int mode = lv_tileview_get_tile_active(s_modes) == s_vision_tile ? NOMI_MODE_VISION : NOMI_MODE_CHAT;
    if (mode == s_mode) return;
    s_mode = mode;
    if (mode == NOMI_MODE_CHAT) lv_obj_remove_flag(s_preview_hint, LV_OBJ_FLAG_HIDDEN);
    update_mode_label();
    set_int(NOMI_KEY_MODE, mode, true);
}

static void on_modes_scroll(lv_event_t *e)
{
    (void)e;
    user_activity();
}

// 触摸只用来叫醒它；说话交给 BOOT 键和唤醒词
static void on_face_pressed(lv_event_t *e)
{
    (void)e;
    user_activity();
}

static lv_obj_t *add_tile(lv_obj_t *tileview, uint8_t col, uint8_t row, lv_dir_t dir, uint32_t bg)
{
    lv_obj_t *tile = lv_tileview_add_tile(tileview, col, row, dir);
    lv_obj_set_style_bg_color(tile, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    set_color(tile, C_TEXT);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    return tile;
}

static lv_obj_t *add_tileview(lv_obj_t *parent)
{
    lv_obj_t *tv = lv_tileview_create(parent);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(tv, lv_color_black(), 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    return tv;
}

void nomi_ui_create(lv_obj_t *screen, uint16_t *face_buf, const lv_font_t *font,
                    const nomi_ui_callbacks_t *callbacks)
{
    s_buf = face_buf;
    s_font = font;
    s_cb = *callbacks;
    s_subtitles_on = get_int(NOMI_KEY_SUBTITLES, 1);
    s_mode = get_int(NOMI_KEY_MODE, NOMI_MODE_CHAT) == NOMI_MODE_VISION ? NOMI_MODE_VISION : NOMI_MODE_CHAT;

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // 外层：设置在上，主界面在下
    s_outer = add_tileview(screen);
    s_settings_tile = add_tile(s_outer, 0, 0, LV_DIR_BOTTOM, C_PAGE);
    s_main_tile = add_tile(s_outer, 0, 1, LV_DIR_TOP, 0x000000);
    lv_obj_add_event_cb(s_outer, on_outer_scroll, LV_EVENT_SCROLL_BEGIN, NULL);

    // 主界面里：Nomi 在左，视觉在右
    s_modes = add_tileview(s_main_tile);
    s_chat_tile = add_tile(s_modes, 0, 0, LV_DIR_RIGHT, 0x000000);
    s_vision_tile = add_tile(s_modes, 1, 0, LV_DIR_LEFT, 0x000000);
    lv_obj_add_event_cb(s_modes, on_mode_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_modes, on_modes_scroll, LV_EVENT_SCROLL_BEGIN, NULL);

    face_init(lv_tick_get() * 2654435761u + 1);
    face_render(s_buf);  // 第一帧整屏画好
    s_canvas = lv_canvas_create(s_chat_tile);
    lv_canvas_set_buffer(s_canvas, s_buf, FACE_W, FACE_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, on_face_pressed, LV_EVENT_PRESSED, NULL);

    for (int i = 0; i < ZZZ_COUNT; i++) {
        s_zzz[i] = lv_label_create(s_chat_tile);
        lv_label_set_text(s_zzz[i], "z");
        set_color(s_zzz[i], C_TEXT);
        lv_obj_add_flag(s_zzz[i], LV_OBJ_FLAG_HIDDEN);
    }

    build_vision();

    // 以下浮在主界面上，左右切模式时不动
    lv_obj_t *grip = lv_obj_create(s_main_tile);  // 顶部小横条：提示往下拉打开设置
    lv_obj_remove_style_all(grip);
    lv_obj_set_size(grip, 44, 4);
    lv_obj_set_style_radius(grip, 2, 0);
    lv_obj_set_style_bg_color(grip, lv_color_hex(C_MUTED), 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_50, 0);
    lv_obj_align(grip, LV_ALIGN_TOP_MID, 0, 6);

    s_status = lv_label_create(s_main_tile);
    set_color(s_status, C_MUTED);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 16);
    lv_label_set_text(s_status, "");

    s_mode_label = lv_label_create(s_main_tile);
    set_color(s_mode_label, C_MUTED);
    lv_obj_align(s_mode_label, LV_ALIGN_TOP_LEFT, 12, 10);

    s_battery = lv_label_create(s_main_tile);
    lv_obj_set_style_text_font(s_battery, &lv_font_montserrat_14, 0);
    lv_obj_align(s_battery, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_obj_add_flag(s_battery, LV_OBJ_FLAG_HIDDEN);

    s_subtitle = lv_label_create(s_main_tile);
    set_color(s_subtitle, C_TEXT);
    lv_obj_set_width(s_subtitle, FACE_W - 40);
    lv_label_set_long_mode(s_subtitle, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_subtitle, lv_color_black(), 0);  // 视觉模式下压在画面上也看得清
    lv_obj_set_style_bg_opa(s_subtitle, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_subtitle, 8, 0);
    lv_obj_align(s_subtitle, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);

    build_settings();
    show_main(false);
    show_mode(s_mode, false);
    update_mode_label();

    // 通知条在最上层，翻到哪一页都看得到
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
