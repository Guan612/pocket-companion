#include "nomi_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "face.h"
#include "face_script.h"

#define FRAME_MS 33
#define STEP_S (1.0f / 60)
#define EMOTION_HOLD_MS 4000  // LLM 给的情绪保持多久
#define THINK_HOLD_MS 30000   // "思考中"一直保持到识别 / 大模型有结果，最长和 HTTP 超时一样
#define SUBTITLE_LINGER_MS 600  // 说完后字幕再留多久就收起
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

// 页面结构：外层上下翻（设置 / 主界面），主界面里左右翻（表情 / 历史对话），状态栏和字幕浮在主界面上
static lv_obj_t *s_outer, *s_settings_tile, *s_main_tile;
static lv_obj_t *s_modes, *s_chat_tile, *s_hist_tile;
static lv_obj_t *s_canvas, *s_hist_list;
static lv_obj_t *s_status, *s_battery, *s_subtitle, *s_toast;
static lv_obj_t *s_listen_sw;  // 连续聆听开关（设置 → 对话，跟着设备状态亮灭）
static lv_timer_t *s_toast_timer;
static uint32_t s_last_ms, s_emotion_until;
static float s_acc;
static face_mode_t s_emotion_mode = FACE_IDLE;
static bool s_sleeping, s_subtitles_on = true;
static bool s_page_by_code;   // 代码切页面时不当作用户操作
static bool s_hist_was_open;  // 睡着前停在历史页的话，醒来回那里
static bool s_hist_dirty;     // 有新消息，历史页可见时要重建列表
static nomi_state_t s_last_state = NOMI_STATE_IDLE;
static uint32_t s_subtitle_hide_at;  // 到这个时刻收起字幕，0 表示不收
static bool s_sw_syncing;            // 状态同步改开关时不算用户操作
static uint32_t s_listen_hold_until; // 用户刚点过连续聆听开关：这段时间状态同步别把开关扳回去

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
static void show_page(int page, bool anim);
static void on_face_pressed(lv_event_t *e);
static void rebuild_history(bool to_bottom);
static void sync_listen_switch(nomi_state_t state);

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

static bool hist_page_active(void)
{
    return lv_tileview_get_tile_active(s_modes) == s_hist_tile;
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

    // 待机、睡着、正在表演时不显示状态文字，脸本身就说明了（表演的道具也可能放在头顶那一格）
    nomi_state_t state = s_cb.get_state ? s_cb.get_state() : NOMI_STATE_IDLE;
    lv_obj_set_flag(s_status, LV_OBJ_FLAG_HIDDEN, state == NOMI_STATE_IDLE || s_sleeping || face_is_playing());

    // 说完了就收起字幕（小智回到待机时会清聊天记录，但 Nomi 的字幕不在那套界面里，得自己收）
    if (s_last_state == NOMI_STATE_SPEAKING && state != NOMI_STATE_SPEAKING) s_subtitle_hide_at = now + SUBTITLE_LINGER_MS;
    if (state == NOMI_STATE_SPEAKING) s_subtitle_hide_at = 0;
    if (s_subtitle_hide_at && (int32_t)(now - s_subtitle_hide_at) >= 0) {
        lv_obj_add_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
        s_subtitle_hide_at = 0;
    }
    s_last_state = state;

    // 连续聆听开关跟着设备状态走
    sync_listen_switch(state);

    // 来了新消息且正停在历史页：把列表重建一遍（说话时逐句更新，一次重建就几毫秒）
    if (s_hist_dirty && hist_page_active()) {
        s_hist_dirty = false;
        rebuild_history(false);
    }

    // 表情直接画进画布缓冲，只通知 LVGL 重画变化的那块
    rrect_t d = face_render(s_buf);
    if (!rrect_empty(d)) {
        lv_area_t area = {d.x0, d.y0, d.x1 - 1, d.y1 - 1};
        lv_obj_invalidate_area(s_canvas, &area);
    }
}

bool nomi_ui_play_face(const char *json, char *err, size_t err_size)
{
    static face_script_t script;  // 一千多字节，别放栈上
    if (!face_script_parse(json, &script, err, err_size)) return false;
    face_play(&script);
    return true;
}

void nomi_ui_set_emotion(const char *emotion)
{
    if (!emotion) return;
    s_emotion_mode = mode_for_emotion(emotion);
    // 思考要等到下一个情绪 / 说话 / 出错把它换掉；其他情绪过几秒就回到跟着设备状态走
    s_emotion_until = lv_tick_get() + (s_emotion_mode == FACE_THINK ? THINK_HOLD_MS : EMOTION_HOLD_MS);
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
    lv_obj_t *roots[] = {s_status, s_subtitle, s_toast, s_settings_tile, s_hist_tile};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        if (roots[i]) lv_obj_set_style_text_font(roots[i], font, 0);
    }
    lv_obj_set_height(s_subtitle, lv_font_get_line_height(font) * 2);  // 最多两行
}

void nomi_ui_set_status(const char *status)
{
    lv_label_set_text(s_status, status ? status : "");
}

// 字幕只有两行高，放不下的句子按 UTF-8 从后往前截出能放下的尾巴（正在念的就是最后这句），
// 前面补一个省略号。估宽从宽了算，截多了没关系，截少了还有 DOTS 模式兜底。
static const char *trim_subtitle(const char *text)
{
    static char buf[512];
    if (!s_font) return text;
    const int budget = (FACE_W - 40) * 2;  // 两行的像素预算
    const int lh = lv_font_get_line_height(s_font);
    size_t len = strlen(text);
    int used = 0;
    size_t start = 0, i = len;
    while (i > 0) {
        size_t cs = i - 1;  // 往前找到这个字符的开头
        while (cs > 0 && ((unsigned char)text[cs] & 0xC0) == 0x80) cs--;
        int w = i - cs >= 3 ? lh : lh / 2;  // 中文按整字宽、半角按一半估
        if (used + w > budget - lh) {
            start = i;
            break;
        }
        used += w;
        i = cs;
    }
    if (start == 0) return text;  // 全放得下
    snprintf(buf, sizeof(buf), "…%s", text + start);
    return buf;
}

void nomi_ui_set_subtitle(const char *role, const char *text)
{
    s_subtitle_hide_at = 0;  // 有新字幕就别收了
    s_hist_dirty = true;     // 历史页记下这条消息，可见时重建（字幕关了也要记）
    if (!s_subtitles_on || !text || !*text) {
        lv_obj_add_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // 用户说的话偏灰，助手的回答用亮白
    bool from_user = role && strcmp(role, "user") == 0;
    set_color(s_subtitle, from_user ? C_MUTED : C_TEXT);
    lv_label_set_text(s_subtitle, trim_subtitle(text));
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

// 睡着时回到表情页（睡着动画在大脸上）；醒来回到原来停的页面
void nomi_ui_set_sleep(bool sleeping)
{
    s_sleeping = sleeping;
    face_set_sleep(sleeping);
    if (sleeping) {
        lv_obj_add_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
        show_main(false);
        s_hist_was_open = hist_page_active();
        if (s_hist_was_open) show_page(0, false);
    } else if (s_hist_was_open) {
        s_hist_was_open = false;
        s_hist_dirty = true;
        show_page(1, false);
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

/* ---------- 第二页：历史对话 ---------- */

// 按回调里的记录重建整个列表；to_bottom 为 true 时滚到最下面
static void rebuild_history(bool to_bottom)
{
    if (!s_cb.history_count || !s_cb.history_get) return;
    bool was_at_bottom = lv_obj_get_scroll_bottom(s_hist_list) <= 2;
    lv_obj_clean(s_hist_list);
    int n = s_cb.history_count();
    if (n <= 0) {
        lv_obj_t *hint = lv_label_create(s_hist_list);
        lv_label_set_text(hint, "还没有对话\n\n聊过的话会记在这里\n往右滑回去叫它说话吧");
        lv_obj_set_width(hint, LV_PCT(100));
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_opa(hint, LV_OPA_60, 0);
    }
    for (int i = 0; i < n; i++) {
        const char *role = "";
        const char *text = s_cb.history_get(i, &role);
        if (!text || !*text) continue;
        lv_obj_t *label = lv_label_create(s_hist_list);
        // 自己的话带个称呼、用灰色；它的回答用亮白
        bool from_user = strcmp(role, "user") == 0;
        lv_label_set_text_fmt(label, "%s%s", from_user ? "我：" : "", text);
        lv_obj_set_width(label, LV_PCT(100));
        set_color(label, from_user ? C_MUTED : C_TEXT);
    }
    if (to_bottom || was_at_bottom) {
        lv_obj_update_layout(s_hist_list);  // 先算好布局才知道能滚多远
        lv_obj_scroll_to_y(s_hist_list, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

// 左右翻到另一页：翻到历史页时把列表刷成最新的
static void on_page_changed(lv_event_t *e)
{
    (void)e;
    if (s_page_by_code) return;
    if (hist_page_active()) {
        s_hist_dirty = false;
        rebuild_history(true);
    }
}

static void on_modes_scroll(lv_event_t *e)
{
    (void)e;
    user_activity();
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

/* ---------- 连续聆听开关（设置 → 对话）：不是存的设置，就是"现在在不在连续对话" ---------- */

// 开关亮灭跟着设备状态走：聆听 / 说话中就是开。设置页开着时 face_tick 每帧对一次，
// 对话自动结束（没人说话回待机）开关也会自己灭。
// 开 / 关是异步生效的（要排进主任务开通道），用户刚点过的几秒里别把开关扳回去，等状态落定再说
static void sync_listen_switch(nomi_state_t state)
{
    if (!s_listen_sw) return;
    if ((int32_t)(s_listen_hold_until - lv_tick_get()) > 0) return;  // 用户刚点过，先别抢
    bool on = state == NOMI_STATE_LISTENING || state == NOMI_STATE_SPEAKING;
    if (lv_obj_has_state(s_listen_sw, LV_STATE_CHECKED) == on) return;
    s_sw_syncing = true;  // 程序改状态也可能触发事件，别当成用户点了一下
    if (on) lv_obj_add_state(s_listen_sw, LV_STATE_CHECKED);
    else lv_obj_remove_state(s_listen_sw, LV_STATE_CHECKED);
    s_sw_syncing = false;
}

static void on_listen_switch(lv_event_t *e)
{
    (void)e;
    if (s_sw_syncing) return;
    user_activity();
    s_listen_hold_until = lv_tick_get() + 3000;
    action("toggle_listen");  // 开：待机进连续聆听；关：退出对话回待机
}

static void add_listen_switch(lv_obj_t *page)
{
    lv_obj_t *item = add_item(page, "连续聆听",
                              "打开就开始连续对话，关闭退出回待机；BOOT 键和唤醒词都不用");
    s_listen_sw = lv_switch_create(item);
    lv_obj_add_flag(s_listen_sw, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(s_listen_sw, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_set_style_bg_color(s_listen_sw, lv_color_hex(C_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_listen_sw, lv_color_hex(C_ACCENT), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_listen_sw, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_add_event_cb(s_listen_sw, on_listen_switch, LV_EVENT_VALUE_CHANGED, NULL);
    sync_listen_switch(s_cb.get_state ? s_cb.get_state() : NOMI_STATE_IDLE);  // 建出来就是当前状态
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
    add_listen_switch(page);
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
    add_info(page, "自定义", "人设提示词、API 地址和 Key、看图识物的模型，都在网页设置页里改");
}

static void build_display_page(lv_obj_t *page)
{
    static const char *const dim[] = {"30 秒", "1 分", "2 分", "5 分", "从不", ""};
    static const char *const off[] = {"2 分", "5 分", "10 分", "30 分", "从不", ""};
    add_slider(page, "亮度", NOMI_KEY_BRIGHTNESS, MIN_BRIGHTNESS, 80);
    add_choice(page, "多久后睡着", "睡着后过 30 秒屏幕再调暗", NOMI_KEY_DIM, dim, 1);
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

// 设置页里外层翻页是锁住的，只有按在底部小白条上时才放开，往上推才能回主界面
static void lock_outer_if_settings(void)
{
    if (lv_tileview_get_tile_active(s_outer) == s_settings_tile) {
        lv_obj_set_scroll_dir(s_outer, LV_DIR_NONE);
    }
}

static void on_grip(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        lv_obj_set_scroll_dir(s_outer, LV_DIR_VER);
    } else {
        lock_outer_if_settings();
    }
}

// tileview 在滚动结束时会按 tile 方向重新放开滚动（在发 VALUE_CHANGED 之后），
// 所以挂在 SCROLL_END 上、排在它自己的处理之后再锁住
static void on_outer_scroll_end(lv_event_t *e)
{
    (void)e;
    lock_outer_if_settings();
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
    // 静音模式：关了语音播报，回复只显示文字，不念出来也不调合成接口
    add_switch(sound, "语音播报", "关掉就是静音模式：回复只显示文字，不念出来", NOMI_KEY_TTS_SPEAK,
               true);
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

    // 底部一条把手区：只有按住这里往上推才收起设置，其他地方上下推都是翻内容
    lv_obj_t *grip_zone = lv_obj_create(s_settings_tile);
    lv_obj_remove_style_all(grip_zone);
    lv_obj_set_size(grip_zone, LV_PCT(100), 28);
    lv_obj_align(grip_zone, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_pad_left(grip_zone, 92, 0);  // 横条居中在内容区，避开左边的标签栏
    lv_obj_add_flag(grip_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(grip_zone, on_grip, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(grip_zone, on_grip, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(grip_zone, on_grip, LV_EVENT_PRESS_LOST, NULL);
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

// 代码切页面：0 表情，1 历史
static void show_page(int page, bool anim)
{
    s_page_by_code = true;
    lv_tileview_set_tile(s_modes, page ? s_hist_tile : s_chat_tile, anim ? LV_ANIM_ON : LV_ANIM_OFF);
    s_page_by_code = false;
}

// 外层开始上下翻：从主界面往下拉时先把设置页刷新成最新的值
static void on_outer_scroll(lv_event_t *e)
{
    (void)e;
    user_activity();
    if (lv_tileview_get_tile_active(s_outer) == s_main_tile) build_settings();
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

    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    // 外层：设置在上，主界面在下
    s_outer = add_tileview(screen);
    s_settings_tile = add_tile(s_outer, 0, 0, LV_DIR_BOTTOM, C_PAGE);
    s_main_tile = add_tile(s_outer, 0, 1, LV_DIR_TOP, 0x000000);
    lv_obj_add_event_cb(s_outer, on_outer_scroll, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(s_outer, on_outer_scroll_end, LV_EVENT_SCROLL_END, NULL);

    // 主界面里：表情在左，历史对话在右
    s_modes = add_tileview(s_main_tile);
    s_chat_tile = add_tile(s_modes, 0, 0, LV_DIR_RIGHT, 0x000000);
    s_hist_tile = add_tile(s_modes, 1, 0, LV_DIR_LEFT, C_PAGE);
    lv_obj_add_event_cb(s_modes, on_page_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_modes, on_modes_scroll, LV_EVENT_SCROLL_BEGIN, NULL);

    face_init(lv_tick_get() * 2654435761u + 1);
    face_render(s_buf);  // 第一帧整屏画好
    s_canvas = lv_canvas_create(s_chat_tile);
    lv_canvas_set_buffer(s_canvas, s_buf, FACE_W, FACE_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_canvas, on_face_pressed, LV_EVENT_PRESSED, NULL);

    // 历史对话页：一列消息，上下滚动；进来时按回调里的记录重建
    s_hist_list = lv_obj_create(s_hist_tile);
    lv_obj_remove_style_all(s_hist_list);
    lv_obj_set_size(s_hist_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_hist_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_hist_list, 16, 0);
    lv_obj_set_style_pad_bottom(s_hist_list, 40, 0);  // 最后一行别被字幕挡住
    lv_obj_set_style_pad_row(s_hist_list, 10, 0);
    lv_obj_set_scrollbar_mode(s_hist_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_hist_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_hist_list, on_face_pressed, LV_EVENT_PRESSED, NULL);

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
