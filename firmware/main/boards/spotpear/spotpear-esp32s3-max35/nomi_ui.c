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
#define C_WARM 0xffc857  // 太阳、闪电、闹钟铃铛

#define CLOCK_REFRESH_MS 1000  // 时钟页多久对一次时间、天气、闹钟（文字没变就不重画）
#define RADIO_REFRESH_MS 400   // 电台页、左上角电台小条多久对一次状态
#define DOTS_SHOW_MS 2500      // 翻页后页码小圆点留多久

// 时钟页的大数字、天气和播放控制图标（lv_font_conv 从 LVGL 自带的 Montserrat / FontAwesome 生成，见文件头）
LV_FONT_DECLARE(nomi_font_clock_120);
LV_FONT_DECLARE(nomi_font_icon_36);
#define ICON_PLAY "\xEF\x81\x8B"
#define ICON_PAUSE "\xEF\x81\x8C"
#define ICON_PREV "\xEF\x81\x88"
#define ICON_NEXT "\xEF\x81\x91"

static nomi_ui_callbacks_t s_cb;
static const lv_font_t *s_font;
static uint16_t *s_buf;

// 页面结构：外层上下翻（设置 / 主界面），主界面里左右翻（历史对话 / 表情 / 电台 / 时钟），
// 状态栏、字幕、页码小圆点和电台小条浮在主界面上
enum { PAGE_HIST, PAGE_FACE, PAGE_MUSIC, PAGE_CLOCK, PAGE_COUNT };
static lv_obj_t *s_outer, *s_settings_tile, *s_main_tile;
static lv_obj_t *s_modes, *s_chat_tile, *s_hist_tile, *s_music_tile, *s_clock_tile;
// 收听页（电台 / 播客）
static lv_obj_t *s_tab_btns[2], *s_now_cover, *s_now_title, *s_now_sub;
static lv_obj_t *s_radio_prev, *s_radio_play, *s_radio_next, *s_radio_list, *s_radio_volume;
static lv_obj_t *s_volume_row, *s_progress_row, *s_progress, *s_time_pos, *s_time_dur;
static lv_obj_t *s_radio_mini, *s_radio_mini_label;  // 表情页左上角"♪ 名字"小条
static int s_listen_tab;        // 右边列表：0 电台台单，1 播客
static int s_now_episode = -1;  // 左边显示的是不是播客节目（换了就跟着切标签、换控件）
static lv_image_dsc_t s_cover_dsc;
static int s_cover_version = -1;
static uint32_t s_radio_refresh_at;  // 下次对收听页的时刻
static lv_obj_t *s_canvas, *s_hist_list;
static lv_obj_t *s_status, *s_battery, *s_subtitle, *s_toast, *s_dots;
static lv_timer_t *s_toast_timer;
static uint32_t s_last_ms, s_emotion_until;
static float s_acc;
static face_mode_t s_emotion_mode = FACE_IDLE;
static bool s_sleeping, s_subtitles_on = true;
static bool s_page_by_code;          // 代码切页面时不当作用户操作
static int s_page_before_sleep = -1;  // 睡着前停在哪页，醒来回那里
static bool s_hist_dirty;     // 有新消息，历史页可见时要重建列表
static nomi_state_t s_last_state = NOMI_STATE_IDLE;
static uint32_t s_subtitle_hide_at;  // 到这个时刻收起字幕，0 表示不收
static uint32_t s_dots_until;        // 页码小圆点显示到这个时刻
// 时钟页
static lv_obj_t *s_clock_digit[5], *s_clock_date, *s_wx_row, *s_wx_icon, *s_wx_temp, *s_wx_text, *s_alarm_list;
static uint32_t s_clock_next;    // 下次对时钟页的时刻
static char s_alarms_shown[512];  // 时钟页上闹钟列表对应的文字，变了才重建
static lv_obj_t *s_armed_delete;  // 点过一次、等确认的删除按钮
// 响铃
static lv_obj_t *s_alarm_layer, *s_alarm_time, *s_alarm_label, *s_alarm_bell;
// 历史页增量更新：列表里的标签和记录一一对应，只追加新消息、改最后一条、删掉记录头上丢掉的
static int s_hist_shown;        // 列表里已有几条消息
static int s_hist_base;         // 上次同步时记录已经从头上丢掉了几条
static size_t s_hist_last_len;  // 最后一条显示时的长度（回答是逐句接上去的）
static lv_obj_t *s_hist_hint;   // "还没有对话"提示
static char s_settings_sig[768];  // 设置页建好时各项的值；下拉时没变就不重建
static lv_obj_t *s_wifi_label;  // 设置 → 网络里"当前 Wi-Fi"那行（信号强度每次打开都刷新）
static char s_status_text[64];  // 小智设的状态文字（思考中时显示"思考中…"，结束后换回它）
static int s_battery_level = -2;
static bool s_battery_charging;

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
static int current_page(void);
static void on_face_pressed(lv_event_t *e);
static void sync_history(bool to_bottom);
static void clock_refresh(bool force);
static void radio_refresh(bool force);
static void set_text_if_changed(lv_obj_t *label, const char *text);
static void update_dots(void);

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
    if (state == NOMI_STATE_THINKING) return FACE_THINK;  // 录完到开口：一直是思考的样子
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

// 表情只在看得见、而且没在翻页时才画。屏幕是 40MHz SPI，整屏刷一次要 60ms，表情每帧还要重画
// 一大块；停在设置 / 历史 / 时钟页、或者手指正在拖页面时还照画，翻页和滚动就会被它拖慢
// 注意 tileview 的"当前页"在第一次切页 / 滑动结束前是 NULL，所以只按"停在别的页"判断看不见
static bool face_visible(void)
{
    if (lv_tileview_get_tile_active(s_outer) == s_settings_tile) return false;
    lv_obj_t *tile = lv_tileview_get_tile_active(s_modes);
    if (tile == s_hist_tile || tile == s_music_tile || tile == s_clock_tile) return false;
    if (s_alarm_layer && !lv_obj_has_flag(s_alarm_layer, LV_OBJ_FLAG_HIDDEN)) return false;
    return !lv_obj_is_scrolling(s_outer) && !lv_obj_is_scrolling(s_modes);
}

static bool in_conversation(nomi_state_t state)
{
    return state == NOMI_STATE_LISTENING || state == NOMI_STATE_THINKING || state == NOMI_STATE_SPEAKING ||
           state == NOMI_STATE_CONNECTING;
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

    // 待机、睡着、正在表演时不显示状态文字，脸本身就说明了（表演的道具也可能放在头顶那一格）。
    // 配网模式例外：一直写着"配网模式"，不然换个地方连不上网时屏幕上什么提示都没有
    nomi_state_t state = s_cb.get_state ? s_cb.get_state() : NOMI_STATE_IDLE;
    bool hide_status = state == NOMI_STATE_IDLE || ((s_sleeping || face_is_playing()) && state != NOMI_STATE_WIFI_SETUP);
    if (lv_obj_has_flag(s_status, LV_OBJ_FLAG_HIDDEN) != hide_status) lv_obj_set_flag(s_status, LV_OBJ_FLAG_HIDDEN, hide_status);
    // 思考中：设备状态还是"聆听中"（小智只在开口时才切到说话），状态文字这里自己换掉，结束时换回来
    if ((state == NOMI_STATE_THINKING) != (s_last_state == NOMI_STATE_THINKING)) {
        lv_label_set_text(s_status, state == NOMI_STATE_THINKING ? "思考中…" : s_status_text);
    }

    // 说完了就收起字幕（小智回到待机时会清聊天记录，但 Nomi 的字幕不在那套界面里，得自己收）
    if (s_last_state == NOMI_STATE_SPEAKING && state != NOMI_STATE_SPEAKING) s_subtitle_hide_at = now + SUBTITLE_LINGER_MS;
    if (state == NOMI_STATE_SPEAKING) s_subtitle_hide_at = 0;
    if (s_subtitle_hide_at && (int32_t)(now - s_subtitle_hide_at) >= 0) {
        lv_obj_add_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
        s_subtitle_hide_at = 0;
    }
    // 停在时钟 / 电台页时叫它说话：翻回表情页（听、想、说的样子都在脸上）
    int page = current_page();
    if (in_conversation(state) && !in_conversation(s_last_state) && (page == PAGE_CLOCK || page == PAGE_MUSIC)) {
        show_page(PAGE_FACE, !s_sleeping);
    }
    s_last_state = state;

    // 时钟页：每秒对一次时间、天气、闹钟（看不见时也对，文字没变就不重画，翻过去时已经是对的）
    if ((int32_t)(now - s_clock_next) >= 0) {
        s_clock_next = now + CLOCK_REFRESH_MS;
        clock_refresh(false);
    }
    if ((int32_t)(now - s_radio_refresh_at) >= 0) {
        s_radio_refresh_at = now + RADIO_REFRESH_MS;
        radio_refresh(false);
    }

    // 页码小圆点：翻页时和翻完后一小会儿显示；有字幕时让开
    bool dots = (lv_obj_is_scrolling(s_modes) || (int32_t)(s_dots_until - now) > 0) &&
                lv_obj_has_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
    if (lv_obj_has_flag(s_dots, LV_OBJ_FLAG_HIDDEN) == dots) lv_obj_set_flag(s_dots, LV_OBJ_FLAG_HIDDEN, !dots);

    // 来了新消息且正停在历史页：只把新句子补上去（不在拖动列表时做，免得滚动中跳一下）
    if (s_hist_dirty && hist_page_active() && !lv_obj_is_scrolling(s_hist_list)) {
        s_hist_dirty = false;
        sync_history(false);
    }

    // 表情直接画进画布缓冲，只通知 LVGL 重画变化的那块；看不见或正在翻页时先停着（动画状态照走）
    if (!face_visible()) return;
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
    if (font != s_font) s_settings_sig[0] = '\0';  // 设置页里有几层显式设了字体，下次下拉时重建
    s_font = font;
    lv_obj_t *roots[] = {s_status,     s_subtitle,   s_toast,       s_settings_tile, s_hist_tile,
                         s_music_tile, s_clock_tile, s_alarm_layer, s_radio_mini};
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        if (roots[i]) lv_obj_set_style_text_font(roots[i], font, 0);
    }
    lv_obj_set_height(s_subtitle, lv_font_get_line_height(font) * 2);  // 最多两行
}

void nomi_ui_set_status(const char *status)
{
    snprintf(s_status_text, sizeof(s_status_text), "%s", status ? status : "");
    if (s_last_state != NOMI_STATE_THINKING) lv_label_set_text(s_status, s_status_text);  // 思考中先不盖掉
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
    // 每秒都会调一次，没变就别动标签（改文字会让那块区域重画、多刷一次屏）
    if (level == s_battery_level && charging == s_battery_charging) return;
    s_battery_level = level;
    s_battery_charging = charging;
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

// 睡着时翻到时钟页（设置里选了"表情"、或者还没对上时间时，回到表情页看睡着动画）；
// 醒来回到原来停的页面。是被叫醒来说话的就停在表情页
void nomi_ui_set_sleep(bool sleeping)
{
    s_sleeping = sleeping;
    face_set_sleep(sleeping);
    if (sleeping) {
        lv_obj_add_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
        show_main(false);
        s_page_before_sleep = current_page();
        bool clock = get_int(NOMI_KEY_SLEEP_SHOW, 0) == 0 && get_text("clock_time")[0];
        show_page(clock ? PAGE_CLOCK : PAGE_FACE, false);
        if (clock) clock_refresh(true);
    } else if (s_page_before_sleep >= 0) {
        int page = s_page_before_sleep;
        s_page_before_sleep = -1;
        nomi_state_t state = s_cb.get_state ? s_cb.get_state() : NOMI_STATE_IDLE;
        if (page == PAGE_CLOCK && in_conversation(state)) page = PAGE_FACE;
        if (page == PAGE_HIST) s_hist_dirty = true;
        if (page != current_page()) show_page(page, false);
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

// 一条消息的文字：自己的话带个称呼、用灰色；它的回答用亮白
static void set_history_label(lv_obj_t *label, const char *role, const char *text)
{
    bool from_user = strcmp(role, "user") == 0;
    lv_label_set_text_fmt(label, "%s%s", from_user ? "我：" : "", text ? text : "");
    set_color(label, from_user ? C_MUTED : C_TEXT);
}

// 让列表和回调里的记录一致。不再每次整个重建：记录最多 100 条、每条能有上千字，
// 全部重建要排一遍版，翻到历史页和说话时逐句更新都会卡一下。to_bottom 为 true 时滚到最下面
static void sync_history(bool to_bottom)
{
    if (!s_cb.history_count || !s_cb.history_get) return;
    bool was_at_bottom = lv_obj_get_scroll_bottom(s_hist_list) <= 2;
    int n = s_cb.history_count();
    int dropped = s_cb.history_dropped ? s_cb.history_dropped() : s_hist_base;

    // 记录满了会从头上丢旧消息，列表也删掉对应的前几条；对不上（比如记录被清空）就整个重建
    int gone = dropped - s_hist_base;
    if (gone < 0 || n < s_hist_shown - gone) {
        lv_obj_clean(s_hist_list);
        s_hist_hint = NULL;
        s_hist_shown = 0;
    } else if (gone > 0) {
        int k = gone < s_hist_shown ? gone : s_hist_shown;
        for (int i = 0; i < k; i++) lv_obj_delete(lv_obj_get_child(s_hist_list, 0));
        s_hist_shown -= k;
    }
    s_hist_base = dropped;

    const char *role = "";
    const char *text;
    // 最后一条回答可能又接上了新句子
    if (s_hist_shown > 0 && s_hist_shown <= n) {
        text = s_cb.history_get(s_hist_shown - 1, &role);
        if (strlen(text ? text : "") != s_hist_last_len) {
            set_history_label(lv_obj_get_child(s_hist_list, s_hist_shown - 1), role, text);
        }
    }
    for (int i = s_hist_shown; i < n; i++) {
        text = s_cb.history_get(i, &role);
        lv_obj_t *label = lv_label_create(s_hist_list);
        lv_obj_set_width(label, LV_PCT(100));
        set_history_label(label, role, text);
    }
    s_hist_shown = n;
    if (n > 0) {
        text = s_cb.history_get(n - 1, &role);
        s_hist_last_len = strlen(text ? text : "");
    }

    if (n > 0 && s_hist_hint) {
        lv_obj_delete(s_hist_hint);
        s_hist_hint = NULL;
    } else if (n == 0 && !s_hist_hint) {
        s_hist_hint = lv_label_create(s_hist_list);
        lv_label_set_text(s_hist_hint, "还没有对话\n\n聊过的话会记在这里\n往左滑回去叫它说话吧");
        lv_obj_set_width(s_hist_hint, LV_PCT(100));
        lv_obj_set_style_text_align(s_hist_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_opa(s_hist_hint, LV_OPA_60, 0);
    }
    if (to_bottom || was_at_bottom) {
        lv_obj_update_layout(s_hist_list);  // 先算好布局才知道能滚多远
        lv_obj_scroll_to_y(s_hist_list, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

// 左右翻到另一页：翻到历史页时把新消息补上，并停在最新那条
static void on_page_changed(lv_event_t *e)
{
    (void)e;
    update_dots();
    if (s_page_by_code) return;
    s_dots_until = lv_tick_get() + DOTS_SHOW_MS;
    if (hist_page_active()) {
        s_hist_dirty = false;
        sync_history(true);
    }
}

static void on_modes_scroll(lv_event_t *e)
{
    (void)e;
    user_activity();
    clock_refresh(false);  // 往时钟 / 电台页翻时上面已经是对的
    radio_refresh(false);
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

// 危险操作要点两次：第一次只把按钮文字改成"再点一次确认"。
// 设置页不再每次打开都重建，所以离开设置页时要把点过一次的按钮复原，免得下次一点就执行
static lv_obj_t *s_armed_btn;

static void disarm_action_button(void)
{
    if (!s_armed_btn) return;
    lv_obj_remove_state(s_armed_btn, LV_STATE_USER_1);
    lv_label_set_text(lv_obj_get_child(s_armed_btn, 0), lv_obj_get_user_data(s_armed_btn));
    s_armed_btn = NULL;
}

static void on_action_button(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *name = lv_event_get_user_data(e);
    bool dangerous = strcmp(name, "factory_reset") == 0 || strcmp(name, "reconfigure_wifi") == 0;
    if (dangerous && !lv_obj_has_state(btn, LV_STATE_USER_1)) {
        disarm_action_button();
        lv_obj_add_state(btn, LV_STATE_USER_1);
        lv_label_set_text(lv_obj_get_child(btn, 0), "再点一次确认");
        s_armed_btn = btn;
        return;
    }
    if (dangerous) {
        disarm_action_button();
        show_main(true);
    }
    action(name);
}

static void on_action_button_deleted(lv_event_t *e)
{
    if (lv_event_get_target(e) == s_armed_btn) s_armed_btn = NULL;
}

static void add_action(lv_obj_t *parent, const char *text, const char *name, bool danger)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_style_bg_color(btn, lv_color_hex(danger ? 0x4a2a30 : C_CONTROL), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 14, 0);
    lv_obj_set_style_pad_ver(btn, 10, 0);
    lv_obj_set_user_data(btn, (void *)text);  // 原来的文字，复原"再点一次确认"时用
    lv_obj_add_event_cb(btn, on_action_button, LV_EVENT_CLICKED, (void *)name);
    lv_obj_add_event_cb(btn, on_action_button_deleted, LV_EVENT_DELETE, NULL);
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
    add_choice(page, "BOOT 键", "也可以喊「你好小智」", NOMI_KEY_TALK_KEY, talk, 1);
    // 不开时是连续对话：回答完接着听，5 秒没人开口自己回待机，说"退下吧"马上回
    add_switch(page, "一问一答", "答完一句就回待机", NOMI_KEY_ONE_SHOT, true);
    bool aec_ok = get_int(NOMI_KEY_AEC_SUPPORTED, 0);
    add_switch(page, "随时插话打断", aec_ok ? "它说话时开口就能打断" : "这块板不支持", NOMI_KEY_AEC, aec_ok);
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
        add_choice(page, "音色", NULL, NOMI_KEY_VOICE, voice_map, rows);
    }
    add_info(page, "更多", "提示词、音色、API 在网页设置页改");
}

static void build_display_page(lv_obj_t *page)
{
    static const char *const dim[] = {"30 秒", "1 分", "2 分", "5 分", "从不", ""};
    static const char *const off[] = {"2 分", "5 分", "10 分", "30 分", "从不", ""};
    static const char *const sleep_show[] = {"时钟", "表情", ""};
    add_slider(page, "亮度", NOMI_KEY_BRIGHTNESS, MIN_BRIGHTNESS, 80);
    add_choice(page, "多久后睡着", "睡着 30 秒后调暗", NOMI_KEY_DIM, dim, 1);
    add_choice(page, "睡着时显示", NULL, NOMI_KEY_SLEEP_SHOW, sleep_show, 1);
    add_choice(page, "多久后息屏", NULL, NOMI_KEY_OFF, off, 1);
    const char *city = get_text("weather_city");
    lv_obj_t *item = add_item(page, "天气城市", "跟它说「我在深圳」也能改");
    lv_obj_t *label = lv_label_create(item);
    lv_label_set_text(label, city[0] ? city : "自动（按网络位置）");
    set_color(label, C_MUTED);
}

// 已存的 Wi-Fi，一行一个，正连着的标出来
static void add_saved_wifi(lv_obj_t *page)
{
    static char buf[400];
    snprintf(buf, sizeof(buf), "%s", get_text("saved_wifi"));
    char current[40];
    snprintf(current, sizeof(current), "%s", get_text("wifi_ssid"));
    int count = 0;
    for (char *p = buf; *p; p++) count += *p == '\n';
    if (buf[0]) count++;
    char title[48];
    snprintf(title, sizeof(title), "已存的 Wi-Fi（%d / 10）", count);
    lv_obj_t *item = add_item(page, title, "自动连附近的，网页设置页可增删");
    if (!count) {
        lv_obj_t *label = lv_label_create(item);
        lv_label_set_text(label, "还没有");
        set_color(label, C_MUTED);
        return;
    }
    for (char *p = buf; p && *p;) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        lv_obj_t *label = lv_label_create(item);
        lv_obj_set_width(label, LV_PCT(100));
        bool on = current[0] && strcmp(p, current) == 0;
        lv_label_set_text_fmt(label, "%s%s", p, on ? "  · 已连接" : "");
        set_color(label, on ? C_TEXT : C_MUTED);
        p = nl ? nl + 1 : NULL;
    }
}

static void build_network_page(lv_obj_t *page)
{
    // 配网模式：先告诉人怎么加网络——扫码连上热点（手机一般会自动弹出配网页）
    // get_text 的返回值只到下一次调用前有效（设备上是同一个 std::string），热点名先拷出来，
    // 不然下面再取网址时会被覆盖，提示里热点名的位置也显示成网址
    char hotspot[40];
    snprintf(hotspot, sizeof(hotspot), "%s", get_text("hotspot"));
    if (hotspot[0]) {
        char wifi_qr[64], hint[160];
        snprintf(wifi_qr, sizeof(wifi_qr), "WIFI:T:nopass;S:%s;;", hotspot);
        snprintf(hint, sizeof(hint), "手机扫码连热点 %s，打开 %s 添加 Wi-Fi", hotspot, get_text("hotspot_url"));
        lv_obj_t *item = add_item(page, "配网中", hint);
        add_qrcode(item, wifi_qr);
        add_saved_wifi(page);
        return;  // 配网时没有局域网地址，网页设置页的二维码也不显示（二维码画布只有一块，也放不下两张）
    } else {
        lv_obj_t *item = add_item(page, "当前 Wi-Fi", NULL);
        s_wifi_label = lv_label_create(item);
        lv_obj_set_width(s_wifi_label, LV_PCT(100));
        set_color(s_wifi_label, C_MUTED);
        const char *wifi = get_text("wifi");
        lv_label_set_text(s_wifi_label, wifi[0] ? wifi : "没连上");
        add_info(page, "IP 地址", get_text("ip"));
    }
    const char *url = get_text("config_url");
    if (url[0]) {
        lv_obj_t *item = add_item(page, "网页设置页", "手机扫码打开");
        add_qrcode(item, url);
        lv_obj_t *label = lv_label_create(item);
        lv_label_set_text(label, url);
        set_color(label, C_MUTED);
    }
    add_saved_wifi(page);
    add_item(page, "添加 Wi-Fi", "会先断开当前网络，已存的都保留");
    add_action(add_button_row(page), "开配网热点", "reconfigure_wifi", true);
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
    if (lv_tileview_get_tile_active(s_outer) == s_main_tile) disarm_action_button();
}

#define TAB_BAR_W 92  // 设置页左边标签栏的宽度

// 设置页上显示的所有值拼成一串。下拉时和建页面时的那串一样就不重建：
// 重建要删掉再建一百来个控件，正赶在手指开始拖的那一下，拖动会顿一下
static void settings_signature(char *out, size_t size)
{
    static const char *const keys[] = {NOMI_KEY_TALK_KEY,  NOMI_KEY_AEC,       NOMI_KEY_AEC_SUPPORTED,
                                       NOMI_KEY_SUBTITLES, NOMI_KEY_PERSONA,   NOMI_KEY_VOICE,
                                       NOMI_KEY_BRIGHTNESS, NOMI_KEY_DIM,      NOMI_KEY_OFF,
                                       NOMI_KEY_VOLUME,    NOMI_KEY_TTS_SPEAK, NOMI_KEY_ONE_SHOT,
                                       NOMI_KEY_SLEEP_SHOW};
    static const char *const texts[] = {"voices", "wifi_ssid", "ip", "config_url", "hotspot", "saved_wifi",
                                        "weather_city"};
    size_t len = 0;
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]) && len < size; i++) {
        len += snprintf(out + len, size - len, "%d,", get_int(keys[i], -9));
    }
    for (size_t i = 0; i < sizeof(texts) / sizeof(texts[0]) && len < size; i++) {
        len += snprintf(out + len, size - len, "%s|", get_text(texts[i]));
    }
}

// 建设置页（开机建一次；之后下拉时值变了才重建）
static void build_settings(void)
{
    settings_signature(s_settings_sig, sizeof(s_settings_sig));
    lv_obj_clean(s_settings_tile);
    s_wifi_label = NULL;

    lv_obj_t *tv = lv_tabview_create(s_settings_tile);
    lv_obj_set_size(tv, LV_PCT(100), LV_PCT(100));
    lv_tabview_set_tab_bar_position(tv, LV_DIR_LEFT);
    lv_tabview_set_tab_bar_size(tv, TAB_BAR_W);
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
    add_switch(sound, "语音播报", "关掉只显示文字", NOMI_KEY_TTS_SPEAK, true);
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

    // 底部一条把手区：只有按住这里往上推才收起设置，其他地方上下推都是翻内容。
    // 只盖内容区，别伸到左边标签栏下面：原来整屏宽，把最下面"系统"标签的下半截挡住了，点着不灵
    lv_obj_t *grip_zone = lv_obj_create(s_settings_tile);
    lv_obj_remove_style_all(grip_zone);
    lv_obj_set_size(grip_zone, FACE_W - TAB_BAR_W, 28);
    lv_obj_align(grip_zone, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
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
    disarm_action_button();
    lv_tileview_set_tile(s_outer, s_main_tile, anim ? LV_ANIM_ON : LV_ANIM_OFF);
}

static lv_obj_t *page_tile(int page)
{
    switch (page) {
    case PAGE_HIST: return s_hist_tile;
    case PAGE_MUSIC: return s_music_tile;
    case PAGE_CLOCK: return s_clock_tile;
    default: return s_chat_tile;
    }
}

static int current_page(void)
{
    lv_obj_t *tile = lv_tileview_get_tile_active(s_modes);
    for (int page = 0; page < PAGE_COUNT; page++) {
        if (tile == page_tile(page)) return page;
    }
    return PAGE_FACE;
}

// 代码切页面（PAGE_*）
static void show_page(int page, bool anim)
{
    s_page_by_code = true;
    lv_tileview_set_tile(s_modes, page_tile(page), anim ? LV_ANIM_ON : LV_ANIM_OFF);
    s_page_by_code = false;
    update_dots();
    if (page == PAGE_CLOCK) clock_refresh(true);
    radio_refresh(page == PAGE_MUSIC);
}

/* ---------- 页码小圆点 ---------- */

static void update_dots(void)
{
    if (!s_dots) return;
    int page = current_page();
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *dot = lv_obj_get_child(s_dots, i);
        lv_obj_set_style_bg_opa(dot, i == page ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

static void create_dots(void)
{
    s_dots = lv_obj_create(s_main_tile);
    lv_obj_remove_style_all(s_dots);
    lv_obj_set_size(s_dots, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_dots, 8, 0);
    lv_obj_remove_flag(s_dots, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < PAGE_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(s_dots);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_style_radius(dot, 3, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(C_TEXT), 0);
    }
    lv_obj_align(s_dots, LV_ALIGN_BOTTOM_MID, 0, -6);
}

/* ---------- 第三页：收听（电台 / 播客） ---------- */

// 左边是"正在放"：不管右边切到哪个标签，都显示现在放的东西（电台或一期播客）；
// 右边列表跟着标签变：电台台单，或者播客订阅 → 点进去是最新几期

enum { RADIO_OFF, RADIO_CONNECTING, RADIO_PLAYING, RADIO_HELD, RADIO_ERROR, RADIO_LOADING };  // 同 NomiRadio::State
enum { ITEM_STATION, ITEM_PODCAST, ITEM_BACK, ITEM_EPISODE, ITEM_RETRY };
#define COVER_SIZE 88  // 同 NomiPodcast::kCoverSize

static void fmt_clock(char *buf, size_t size, int seconds)
{
    if (seconds < 0) seconds = 0;
    if (seconds >= 3600) snprintf(buf, size, "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60);
    else snprintf(buf, size, "%d:%02d", seconds / 60, seconds % 60);
}

static void on_listen_tab(lv_event_t *e)
{
    user_activity();
    s_listen_tab = (int)(intptr_t)lv_event_get_user_data(e);
    radio_refresh(true);
}

static void on_radio_button(lv_event_t *e)
{
    const char *name = lv_event_get_user_data(e);
    user_activity();
    // 左右两个键：电台是换台，播客是倒回 15 秒 / 快进 30 秒
    if (s_now_episode == 1 && !strcmp(name, "radio_prev")) name = "pod_seek:-15";
    else if (s_now_episode == 1 && !strcmp(name, "radio_next")) name = "pod_seek:30";
    action(name);
    radio_refresh(false);
}

static void on_list_item(lv_event_t *e)
{
    int data = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    int kind = data >> 16, index = data & 0xFFFF;
    char name[32];
    switch (kind) {
    case ITEM_STATION: snprintf(name, sizeof(name), "radio_play:%d", index); break;
    case ITEM_PODCAST:
    case ITEM_RETRY: snprintf(name, sizeof(name), "pod_open:%d", index); break;
    case ITEM_BACK: snprintf(name, sizeof(name), "pod_back"); break;
    default: snprintf(name, sizeof(name), "pod_play:%d", index); break;
    }
    user_activity();
    action(name);
    radio_refresh(true);
}

static void on_progress(lv_event_t *e)
{
    char name[32];
    snprintf(name, sizeof(name), "pod_seek_to:%d", (int)lv_slider_get_value(lv_event_get_target(e)));
    user_activity();
    action(name);
}

static lv_obj_t *add_round_button(lv_obj_t *parent, int size, const char *icon, const char *name, bool primary)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, size / 2, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(primary ? C_ACCENT : C_CONTROL), 0);
    lv_obj_add_event_cb(btn, on_radio_button, LV_EVENT_CLICKED, (void *)name);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &nomi_font_icon_36, 0);
    lv_label_set_text(label, icon);
    set_color(label, primary ? 0x0b1220 : C_TEXT);
    lv_obj_center(label);
    return btn;
}

// 右边列表的一项；meta 不空时下面再写一行小字（节目的日期、时长）
static void add_list_item(const char *text, const char *meta, int kind, int index, bool on)
{
    lv_obj_t *btn = lv_button_create(s_radio_list);
    lv_obj_set_size(btn, LV_PCT(100), meta ? 58 : 42);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 10, 0);
    lv_obj_set_style_pad_ver(btn, 4, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(on ? C_ACCENT : kind == ITEM_BACK ? C_PANEL : C_CONTROL), 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_user_data(btn, (void *)(intptr_t)((kind << 16) | (index & 0xFFFF)));
    lv_obj_add_event_cb(btn, on_list_item, LV_EVENT_CLICKED, NULL);
    if (kind == ITEM_BACK) {
        // 返回：箭头用图标字体画（中文字库里没有这个符号）
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(btn, 8, 0);
        lv_obj_t *arrow = lv_label_create(btn);
        lv_obj_set_style_text_font(arrow, &lv_font_montserrat_14, 0);
        lv_label_set_text(arrow, LV_SYMBOL_LEFT);
        set_color(arrow, C_MUTED);
    }
    // 一行，放不下就省略号（省略号只在高度固定时起作用，不定高度会折成好几行）
    const int line = lv_font_get_line_height(s_font);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_size(label, LV_PCT(100), line);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(label, text);
    set_color(label, on ? 0x0b1220 : C_TEXT);
    if (meta) {
        lv_obj_t *m = lv_label_create(btn);
        lv_obj_set_size(m, LV_PCT(100), line);
        lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(m, meta);
        set_color(m, on ? 0x0b1220 : C_MUTED);
    }
}

static void add_list_hint(const char *text)
{
    lv_obj_t *hint = lv_label_create(s_radio_list);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_text(hint, text);
    set_color(hint, C_MUTED);
}

// 一行一项（'\n' 分隔）挨个交给 fn；each 返回 false 就停
static void for_each_line(const char *text, void (*fn)(char *line, int index, void *ctx), void *ctx)
{
    enum { SIZE = 2048 };
    static char *buf;  // 放堆上（设备上大块走 PSRAM），别占内部 RAM
    if (!buf && !(buf = malloc(SIZE))) return;
    snprintf(buf, SIZE, "%s", text);
    int index = 0;
    for (char *p = buf; p && *p; index++) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = '\0';
        fn(p, index, ctx);
        p = nl ? nl + 1 : NULL;
    }
}

static void add_station_line(char *line, int index, void *ctx)
{
    add_list_item(line, NULL, ITEM_STATION, index, index == *(int *)ctx);
}

static void add_podcast_line(char *line, int index, void *ctx)
{
    (void)ctx;
    add_list_item(line, NULL, ITEM_PODCAST, index, false);
}

static void add_episode_line(char *line, int index, void *ctx)
{
    char *tab = strchr(line, '\t');
    if (tab) *tab++ = '\0';
    add_list_item(line, tab && *tab ? tab : NULL, ITEM_EPISODE, index, !strcmp(line, (const char *)ctx));
}

// 右边列表：内容（标签、台单 / 订阅 / 节目单、正在放的是哪个）变了才重建
static void sync_listen_list(bool force)
{
    enum { SIZE = 2600 };
    static char *sig, *now;  // 放堆上（设备上大块走 PSRAM）：静态数组占内部 RAM，放栈上会撑爆界面任务的栈
    if (!sig && !(sig = calloc(1, SIZE))) return;
    if (!now && !(now = malloc(SIZE))) return;
    int len;
    if (s_listen_tab == 0) {
        len = snprintf(now, SIZE, "R%d|%s", get_int("radio_index", -1), get_text("radio_list"));
    } else {
        int opened = get_int("pod_opened", -1), load = get_int("pod_load", 0);
        len = snprintf(now, SIZE, "P%d|%d|", opened, load);
        if (len < SIZE) len += snprintf(now + len, SIZE - len, "%s|", get_text(opened < 0 ? "pod_list" : "pod_episodes"));
        if (len < SIZE) snprintf(now + len, SIZE - len, "%s", get_text("pod_title"));
    }
    if (!force && !strcmp(now, sig)) return;
    if (lv_obj_is_scrolling(s_radio_list)) return;  // 正在滑：等停了再换
    snprintf(sig, SIZE, "%s", now);
    lv_obj_clean(s_radio_list);
    lv_obj_scroll_to_y(s_radio_list, 0, LV_ANIM_OFF);

    if (s_listen_tab == 0) {
        int current = get_int("radio_index", -1);
        const char *list = get_text("radio_list");
        if (!list[0]) add_list_hint("台单是空的，在网页设置页添加");
        else for_each_line(list, add_station_line, &current);
        return;
    }
    int opened = get_int("pod_opened", -1);
    if (opened < 0) {
        const char *list = get_text("pod_list");
        if (!list[0]) add_list_hint("还没订阅。跟它说「订阅忽左忽右」，或者在网页设置页搜索订阅");
        else for_each_line(list, add_podcast_line, NULL);
        return;
    }
    // 打开了一个订阅：第一行返回，下面是最新几期
    add_list_item("订阅", NULL, ITEM_BACK, 0, false);
    int load = get_int("pod_load", 0);
    if (load == 1) {
        add_list_hint("加载中…");
    } else if (load == 2) {
        add_list_item("加载失败，点这里重试", NULL, ITEM_RETRY, opened, false);
    } else {
        static char current[160];
        snprintf(current, sizeof(current), "%s", get_text("pod_title"));
        for_each_line(get_text("pod_episodes"), add_episode_line, current);
    }
}

// 对一次收听页和左上角小条。force 时列表也重建、音量条也对一下
static void radio_refresh(bool force)
{
    if (!s_music_tile) return;
    int state = get_int("radio_state", RADIO_OFF);
    int episode = get_int("pod_playing", 0) ? 1 : 0;
    char title[160], sub[160];

    // 换了放的东西（电台 ↔ 播客）：右边标签跟过去，左右两个键换成换台 / 快进快退，底下换成音量 / 进度
    if (episode != s_now_episode) {
        s_now_episode = episode;
        if (state != RADIO_OFF) s_listen_tab = episode;
        lv_obj_t *prev = lv_obj_get_child(s_radio_prev, 0), *next = lv_obj_get_child(s_radio_next, 0);
        lv_obj_set_style_text_font(prev, episode ? s_font : &nomi_font_icon_36, 0);
        lv_obj_set_style_text_font(next, episode ? s_font : &nomi_font_icon_36, 0);
        lv_label_set_text(prev, episode ? "-15" : ICON_PREV);
        lv_label_set_text(next, episode ? "+30" : ICON_NEXT);
        lv_obj_set_flag(s_volume_row, LV_OBJ_FLAG_HIDDEN, episode);
        lv_obj_set_flag(s_progress_row, LV_OBJ_FLAG_HIDDEN, !episode);
        force = true;
    }

    // 标签按钮
    for (int i = 0; i < 2; i++) {
        bool on = i == s_listen_tab;
        lv_obj_set_style_bg_color(s_tab_btns[i], lv_color_hex(on ? C_ACCENT : C_CONTROL), 0);
        set_color(lv_obj_get_child(s_tab_btns[i], 0), on ? 0x0b1220 : C_MUTED);
    }

    // 封面（播客才有）：换了图就把新缓冲交给图片控件
    const uint16_t *cover = NULL;
    int version = 0, size = 0;
    if (episode && s_cb.get_image) cover = s_cb.get_image("pod_cover", &size, &version);
    bool show_cover = cover && size == COVER_SIZE;
    if (show_cover && version != s_cover_version) {
        s_cover_version = version;
        memset(&s_cover_dsc, 0, sizeof(s_cover_dsc));
        s_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_cover_dsc.header.w = COVER_SIZE;
        s_cover_dsc.header.h = COVER_SIZE;
        s_cover_dsc.header.stride = COVER_SIZE * 2;
        s_cover_dsc.data = (const uint8_t *)cover;
        s_cover_dsc.data_size = COVER_SIZE * COVER_SIZE * 2;
        lv_image_set_src(s_now_cover, &s_cover_dsc);
        lv_obj_invalidate(s_now_cover);
    }
    if (lv_obj_has_flag(s_now_cover, LV_OBJ_FLAG_HIDDEN) == show_cover) {
        lv_obj_set_flag(s_now_cover, LV_OBJ_FLAG_HIDDEN, !show_cover);
        int x = show_cover ? 16 + COVER_SIZE + 12 : 18, w = show_cover ? 244 - x : 226;
        lv_obj_set_pos(s_now_title, x, 50);
        lv_obj_set_width(s_now_title, w);
        lv_obj_set_pos(s_now_sub, x, 82);
        lv_obj_set_width(s_now_sub, w);
    }

    // 标题（一行，太长就滚动）和下面一行
    const char *status = state == RADIO_CONNECTING ? "连接中…"
                       : state == RADIO_LOADING    ? "加载中…"
                       : state == RADIO_HELD       ? "说完话接着放"
                       : state == RADIO_ERROR      ? "连不上，稍后再试"
                       : state == RADIO_OFF        ? (episode ? "已暂停" : "没在放")
                                                   : NULL;
    if (episode) {
        snprintf(title, sizeof(title), "%s", get_text("pod_title"));
        snprintf(sub, sizeof(sub), "%s%s%s", get_text("pod_name"), status ? " · " : "", status ? status : "");
    } else {
        snprintf(title, sizeof(title), "%s", get_text("radio_name"));
        const char *song = get_text("radio_title");
        snprintf(sub, sizeof(sub), "%s", status ? status : song[0] ? song : "直播中");
    }
    set_text_if_changed(s_now_title, title[0] ? title : "收听");
    set_text_if_changed(s_now_sub, sub);
    set_text_if_changed(lv_obj_get_child(s_radio_play, 0), state == RADIO_OFF || state == RADIO_ERROR ? ICON_PLAY : ICON_PAUSE);

    // 进度（播客）：拖着的时候别动它
    if (episode) {
        int pos = get_int("pod_pos", 0), dur = get_int("pod_dur", 0);
        char a[16], b[16];
        fmt_clock(a, sizeof(a), pos);
        fmt_clock(b, sizeof(b), dur);
        set_text_if_changed(s_time_pos, a);
        set_text_if_changed(s_time_dur, dur > 0 ? b : "");
        if (!lv_obj_has_state(s_progress, LV_STATE_PRESSED)) {
            lv_slider_set_range(s_progress, 0, dur > 0 ? dur : 1);
            lv_slider_set_value(s_progress, dur > 0 ? pos : 0, LV_ANIM_OFF);
        }
    }

    // 在放、待机、停在表情页：Nomi 手放耳边、眯着眼跟着晃，头上飘音符（循环播一小段表演）；
    // 一开口说话就停下，换回听人说话的样子
    static bool music_face;
    nomi_state_t dev = s_cb.get_state ? s_cb.get_state() : NOMI_STATE_IDLE;
    bool music = state == RADIO_PLAYING && dev == NOMI_STATE_IDLE && !s_sleeping && current_page() == PAGE_FACE;
    if (music && !face_is_playing()) {
        static const char *const kMusicFace =
            "[{\"eyes\":\"arc\",\"hands\":\"ear\",\"motion\":\"sway\",\"prop\":{\"icon\":\"note\",\"anim\":\"float\",\"at\":\"top\"},\"dur\":4},"
            "{\"eyes\":\"arc\",\"hands\":\"ear\",\"motion\":\"sway\",\"prop\":{\"icon\":\"music\",\"anim\":\"float\",\"at\":\"top\"},\"dur\":4}]";
        char err[80];
        music_face = nomi_ui_play_face(kMusicFace, err, sizeof(err));
    } else if (!music && music_face) {
        music_face = false;
        if (face_is_playing()) face_stop();
    }

    // 表情页左上角小条：在放（或对话时暂停着）、而且停在表情页时显示，点一下来收听页
    bool mini = state != RADIO_OFF && current_page() == PAGE_FACE && !s_sleeping;
    if (lv_obj_has_flag(s_radio_mini, LV_OBJ_FLAG_HIDDEN) == mini) lv_obj_set_flag(s_radio_mini, LV_OBJ_FLAG_HIDDEN, !mini);
    if (mini) set_text_if_changed(s_radio_mini_label, episode ? get_text("pod_name") : get_text("radio_name"));

    sync_listen_list(force);
    if (force && !lv_obj_has_state(s_radio_volume, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_radio_volume, get_int(NOMI_KEY_VOLUME, 70), LV_ANIM_OFF);  // 设置页 / 语音也能改音量
    }
}

static void on_radio_mini(lv_event_t *e)
{
    (void)e;
    user_activity();
    show_page(PAGE_MUSIC, true);
}

static lv_obj_t *add_tab_button(const char *text, int index)
{
    lv_obj_t *btn = lv_button_create(s_music_tile);
    lv_obj_set_size(btn, 64, 30);
    lv_obj_set_style_radius(btn, 15, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_pos(btn, 16 + index * 70, 6);
    lv_obj_add_event_cb(btn, on_listen_tab, LV_EVENT_CLICKED, (void *)(intptr_t)index);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return btn;
}

static void build_music_page(void)
{
    s_tab_btns[0] = add_tab_button("电台", 0);
    s_tab_btns[1] = add_tab_button("播客", 1);

    // 左半边：封面、标题（一行滚动）、下面一行、播放控制、音量或进度
    s_now_cover = lv_image_create(s_music_tile);
    lv_obj_set_size(s_now_cover, COVER_SIZE, COVER_SIZE);
    lv_obj_set_pos(s_now_cover, 16, 44);
    lv_obj_set_style_radius(s_now_cover, 10, 0);
    lv_obj_set_style_clip_corner(s_now_cover, true, 0);
    lv_obj_add_flag(s_now_cover, LV_OBJ_FLAG_HIDDEN);
    s_now_title = lv_label_create(s_music_tile);
    lv_obj_set_width(s_now_title, 226);
    lv_label_set_long_mode(s_now_title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    set_color(s_now_title, C_TEXT);
    lv_obj_set_pos(s_now_title, 18, 50);
    s_now_sub = lv_label_create(s_music_tile);
    lv_obj_set_size(s_now_sub, 226, lv_font_get_line_height(s_font));
    lv_label_set_long_mode(s_now_sub, LV_LABEL_LONG_MODE_DOTS);
    set_color(s_now_sub, C_MUTED);
    lv_obj_set_pos(s_now_sub, 18, 82);

    lv_obj_t *row = lv_obj_create(s_music_tile);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 228, 80);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(row, 16, 136);
    s_radio_prev = add_round_button(row, 56, ICON_PREV, "radio_prev", false);
    s_radio_play = add_round_button(row, 76, ICON_PLAY, "radio_toggle", true);
    s_radio_next = add_round_button(row, 56, ICON_NEXT, "radio_next", false);

    // 电台：音量
    s_volume_row = lv_obj_create(s_music_tile);
    lv_obj_remove_style_all(s_volume_row);
    lv_obj_set_size(s_volume_row, 228, 30);
    lv_obj_set_pos(s_volume_row, 16, 258);
    lv_obj_remove_flag(s_volume_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *vol = lv_label_create(s_volume_row);
    lv_obj_set_style_text_font(vol, &lv_font_montserrat_14, 0);
    lv_label_set_text(vol, LV_SYMBOL_VOLUME_MID);
    set_color(vol, C_MUTED);
    lv_obj_align(vol, LV_ALIGN_LEFT_MID, 6, 0);
    s_radio_volume = lv_slider_create(s_volume_row);
    lv_obj_set_size(s_radio_volume, 180, 8);
    lv_obj_align(s_radio_volume, LV_ALIGN_LEFT_MID, 38, 0);
    lv_slider_set_range(s_radio_volume, 0, 100);
    lv_slider_set_value(s_radio_volume, get_int(NOMI_KEY_VOLUME, 70), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_radio_volume, lv_color_hex(C_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_radio_volume, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_radio_volume, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_add_event_cb(s_radio_volume, on_slider, LV_EVENT_VALUE_CHANGED, (void *)NOMI_KEY_VOLUME);
    lv_obj_add_event_cb(s_radio_volume, on_slider, LV_EVENT_RELEASED, (void *)NOMI_KEY_VOLUME);

    // 播客：进度条（能拖）和时间
    s_progress_row = lv_obj_create(s_music_tile);
    lv_obj_remove_style_all(s_progress_row);
    lv_obj_set_size(s_progress_row, 228, 56);
    lv_obj_set_pos(s_progress_row, 16, 246);
    lv_obj_remove_flag(s_progress_row, LV_OBJ_FLAG_SCROLLABLE);
    s_progress = lv_slider_create(s_progress_row);
    lv_obj_set_size(s_progress, 212, 8);
    lv_obj_align(s_progress, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(s_progress, lv_color_hex(C_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_progress, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_add_event_cb(s_progress, on_progress, LV_EVENT_RELEASED, NULL);
    s_time_pos = lv_label_create(s_progress_row);
    lv_obj_set_style_text_font(s_time_pos, &lv_font_montserrat_14, 0);
    set_color(s_time_pos, C_MUTED);
    lv_label_set_text(s_time_pos, "");
    lv_obj_align(s_time_pos, LV_ALIGN_TOP_LEFT, 0, 28);
    s_time_dur = lv_label_create(s_progress_row);
    lv_obj_set_style_text_font(s_time_dur, &lv_font_montserrat_14, 0);
    set_color(s_time_dur, C_MUTED);
    lv_label_set_text(s_time_dur, "");
    lv_obj_align(s_time_dur, LV_ALIGN_TOP_RIGHT, 0, 28);
    lv_obj_add_flag(s_progress_row, LV_OBJ_FLAG_HIDDEN);

    // 右半边：台单 / 订阅 / 节目单，上下滑（左右滑还是翻页）
    s_radio_list = lv_obj_create(s_music_tile);
    lv_obj_remove_style_all(s_radio_list);
    lv_obj_set_size(s_radio_list, 206, FACE_H - 36);  // 一直到屏幕底（翻页小圆点是浮在上面的）
    lv_obj_set_pos(s_radio_list, 260, 36);
    lv_obj_set_flex_flow(s_radio_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_radio_list, 6, 0);
    lv_obj_set_style_pad_bottom(s_radio_list, 12, 0);
    lv_obj_set_scroll_dir(s_radio_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_radio_list, LV_SCROLLBAR_MODE_ACTIVE);

    lv_obj_add_flag(s_music_tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_music_tile, on_face_pressed, LV_EVENT_PRESSED, NULL);

    // 表情页左上角的小条（浮在主界面上，只在表情页显示）
    s_radio_mini = lv_obj_create(s_main_tile);
    lv_obj_remove_style_all(s_radio_mini);
    lv_obj_set_size(s_radio_mini, LV_SIZE_CONTENT, 30);
    lv_obj_set_flex_flow(s_radio_mini, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_radio_mini, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_radio_mini, 6, 0);
    lv_obj_set_style_pad_hor(s_radio_mini, 10, 0);
    lv_obj_set_style_radius(s_radio_mini, 15, 0);
    lv_obj_set_style_bg_color(s_radio_mini, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(s_radio_mini, LV_OPA_80, 0);
    lv_obj_add_flag(s_radio_mini, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_radio_mini, on_radio_mini, LV_EVENT_CLICKED, NULL);
    lv_obj_t *note = lv_label_create(s_radio_mini);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_14, 0);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    set_color(note, C_ACCENT);
    s_radio_mini_label = lv_label_create(s_radio_mini);
    lv_obj_set_style_max_width(s_radio_mini_label, 150, 0);
    lv_obj_set_height(s_radio_mini_label, lv_font_get_line_height(s_font));
    lv_label_set_long_mode(s_radio_mini_label, LV_LABEL_LONG_MODE_DOTS);
    set_color(s_radio_mini_label, C_MUTED);
    lv_label_set_text(s_radio_mini_label, "");
    lv_obj_set_pos(s_radio_mini, 8, 6);
    lv_obj_add_flag(s_radio_mini, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- 第四页：时钟 ---------- */

// 天气图标（FontAwesome，字库里只收了这几个）。code 夜里加了 1000
static const char *weather_icon(int code, uint32_t *color)
{
    bool night = code >= 1000;
    code %= 1000;
    *color = C_TEXT;
    if (code == 0) {
        if (!night) *color = C_WARM;
        return night ? "\xEF\x86\x86" : "\xEF\x86\x85";  // 月亮 / 太阳
    }
    if (code <= 2) return night ? "\xEF\x9B\x83" : "\xEF\x9B\x84";  // 云遮月 / 云遮日
    if (code == 3) return "\xEF\x83\x82";                            // 云
    if (code == 45 || code == 48) return "\xEF\x9D\x9F";             // 雾
    if (code >= 95) {
        *color = C_WARM;
        return "\xEF\x83\xA7";  // 闪电
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "\xEF\x8B\x9C";  // 雪花
    *color = C_ACCENT;
    if (code == 65 || code == 66 || code == 67 || code == 82) return "\xEF\x9D\x80";  // 大雨
    if ((code == 80 || code == 81) && !night) return "\xEF\x9D\x83";                  // 阵雨（带太阳）
    return "\xEF\x9C\xBD";                                                             // 雨
}

// 文字变了才设（设了就要重画那块）
static void set_text_if_changed(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void on_alarm_delete(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    user_activity();
    if (s_armed_delete != btn) {
        // 第一次点只变成"删除？"，再点一次才删
        if (s_armed_delete) {
            lv_label_set_text(lv_obj_get_child(s_armed_delete, 0), LV_SYMBOL_CLOSE);
            set_color(lv_obj_get_child(s_armed_delete, 0), C_MUTED);
        }
        s_armed_delete = btn;
        lv_obj_t *label = lv_obj_get_child(btn, 0);
        lv_obj_set_style_text_font(label, s_font, 0);
        lv_label_set_text(label, "删除？");
        set_color(label, C_DANGER);
        return;
    }
    char name[32];
    snprintf(name, sizeof(name), "alarm_delete:%d", (int)(intptr_t)lv_obj_get_user_data(btn));
    s_armed_delete = NULL;
    action(name);
    clock_refresh(true);
}

// 闹钟列表：最多两行，再多就写"还有 n 个"；没有就教一句怎么定
static void rebuild_alarm_list(const char *alarms)
{
    lv_obj_clean(s_alarm_list);
    s_armed_delete = NULL;
    if (!alarms[0]) {
        lv_obj_t *hint = lv_label_create(s_alarm_list);
        lv_label_set_text(hint, "说「明早 7 点叫我」就能定闹钟");
        set_color(hint, C_MUTED);
        lv_obj_set_style_text_opa(hint, LV_OPA_70, 0);
        return;
    }
    char buf[sizeof(s_alarms_shown)];
    snprintf(buf, sizeof(buf), "%s", alarms);
    int shown = 0, total = 0;
    for (char *line = buf; line && *line; total++) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (shown < 2) {
            char *when = strchr(line, '\t');
            char *what = when ? strchr(when + 1, '\t') : NULL;
            if (when && what) {
                *when++ = '\0';
                *what++ = '\0';
                lv_obj_t *row = lv_obj_create(s_alarm_list);
                lv_obj_remove_style_all(row);
                lv_obj_set_size(row, LV_PCT(100), 30);
                lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                lv_obj_set_style_pad_column(row, 10, 0);
                lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_t *bell = lv_label_create(row);
                lv_obj_set_style_text_font(bell, &lv_font_montserrat_14, 0);
                lv_label_set_text(bell, LV_SYMBOL_BELL);
                set_color(bell, C_WARM);
                lv_obj_t *label = lv_label_create(row);
                lv_obj_set_flex_grow(label, 1);
                lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
                lv_label_set_text_fmt(label, "%s%s%s", when, what[0] ? "  " : "", what);
                set_color(label, C_TEXT);
                lv_obj_t *del = lv_button_create(row);
                lv_obj_remove_style_all(del);
                lv_obj_set_size(del, 64, 30);
                lv_obj_set_user_data(del, (void *)(intptr_t)atoi(line));
                lv_obj_add_event_cb(del, on_alarm_delete, LV_EVENT_CLICKED, NULL);
                lv_obj_t *x = lv_label_create(del);
                lv_obj_set_style_text_font(x, &lv_font_montserrat_14, 0);
                lv_label_set_text(x, LV_SYMBOL_CLOSE);
                set_color(x, C_MUTED);
                lv_obj_align(x, LV_ALIGN_RIGHT_MID, -4, 0);
                shown++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    if (total > shown) {
        lv_obj_t *more = lv_label_create(s_alarm_list);
        lv_label_set_text_fmt(more, "还有 %d 个", total - shown);
        set_color(more, C_MUTED);
    }
}

// 对一次时钟页：时间、日期、天气、闹钟。force 时不管闹钟列表有没有变都重建
static void clock_refresh(bool force)
{
    if (!s_clock_tile) return;
    const char *t = get_text("clock_time");
    if (t[0]) {
        // 一个字一个格子，等宽排：比例字体的 1 比 0 窄，整串居中的话时间一变就左右晃
        for (int i = 0; i < 5; i++) {
            char ch[2] = {t[i] ? t[i] : ' ', 0};
            set_text_if_changed(s_clock_digit[i], ch);
            lv_obj_set_style_text_opa(s_clock_digit[i], LV_OPA_COVER, 0);
        }
        set_text_if_changed(s_clock_date, get_text("clock_date"));
    } else {
        static const char *const blank[] = {"0", "0", ":", "0", "0"};
        for (int i = 0; i < 5; i++) {
            set_text_if_changed(s_clock_digit[i], blank[i]);
            lv_obj_set_style_text_opa(s_clock_digit[i], LV_OPA_20, 0);
        }
        set_text_if_changed(s_clock_date, "还没对上时间 · 联网后自动对时");
    }

    int code = get_int("weather_code", -1);
    // get_text 的返回值只到下一次调用前有效（设备上是同一个 std::string），先拷出来
    char wx_text[96];
    snprintf(wx_text, sizeof(wx_text), "%s", get_text("weather_text"));
    bool has_wx = code >= 0 || wx_text[0];
    if (lv_obj_has_flag(s_wx_row, LV_OBJ_FLAG_HIDDEN) == has_wx) lv_obj_set_flag(s_wx_row, LV_OBJ_FLAG_HIDDEN, !has_wx);
    if (code >= 0) {
        uint32_t color;
        set_text_if_changed(s_wx_icon, weather_icon(code, &color));
        set_color(s_wx_icon, color);
        set_text_if_changed(s_wx_temp, get_text("weather_temp"));
    } else {
        set_text_if_changed(s_wx_icon, "");
        set_text_if_changed(s_wx_temp, "");
    }
    set_text_if_changed(s_wx_text, wx_text);

    const char *alarms = get_text("alarms");
    if (force || strncmp(alarms, s_alarms_shown, sizeof(s_alarms_shown) - 1) != 0 || !lv_obj_get_child_count(s_alarm_list)) {
        // 正在确认删除时别重建（重建会把"删除？"复原）
        if (!s_armed_delete || force) {
            snprintf(s_alarms_shown, sizeof(s_alarms_shown), "%s", alarms);
            rebuild_alarm_list(alarms);
        }
    }
}

static lv_obj_t *add_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (font) lv_obj_set_style_text_font(label, font, 0);
    set_color(label, color);
    lv_label_set_text(label, "");
    return label;
}

static void build_clock_page(void)
{
    // 整页一列、上下居中：闹钟多一行少一行，空白都均匀分到上下，不全堆在底下
    lv_obj_t *col = lv_obj_create(s_clock_tile);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, FACE_W, FACE_H);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(col, 20, 0);  // 顶上有状态文字和电量
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);  // 点到空白处交给整页（叫醒）

    // 大数字：四个数字格 80 宽、冒号格 34 宽
    const int digit_w = 80, colon_w = 34;
    lv_obj_t *digits = lv_obj_create(col);
    lv_obj_remove_style_all(digits);
    lv_obj_set_size(digits, digit_w * 4 + colon_w, 86);
    lv_obj_remove_flag(digits, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(digits, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0, x = 0; i < 5; i++) {
        int w = i == 2 ? colon_w : digit_w;
        s_clock_digit[i] = add_label(digits, &nomi_font_clock_120, C_TEXT);
        lv_obj_set_width(s_clock_digit[i], w);
        lv_obj_set_style_text_align(s_clock_digit[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_clock_digit[i], x, 0);
        x += w;
    }
    s_clock_date = add_label(col, NULL, C_MUTED);
    lv_obj_set_width(s_clock_date, FACE_W);
    lv_obj_set_style_text_align(s_clock_date, LV_TEXT_ALIGN_CENTER, 0);

    // 天气一行：图标、温度、说明（没有天气时隐藏，不占位置）
    s_wx_row = lv_obj_create(col);
    lv_obj_remove_style_all(s_wx_row);
    lv_obj_set_size(s_wx_row, LV_SIZE_CONTENT, 44);
    lv_obj_set_flex_flow(s_wx_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_wx_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_wx_row, 12, 0);
    lv_obj_remove_flag(s_wx_row, LV_OBJ_FLAG_CLICKABLE);
    s_wx_icon = add_label(s_wx_row, &nomi_font_icon_36, C_TEXT);
    s_wx_temp = add_label(s_wx_row, &nomi_font_icon_36, C_TEXT);
    s_wx_text = add_label(s_wx_row, NULL, C_MUTED);
    lv_obj_add_flag(s_wx_row, LV_OBJ_FLAG_HIDDEN);

    // 闹钟列表
    s_alarm_list = lv_obj_create(col);
    lv_obj_remove_style_all(s_alarm_list);
    lv_obj_set_size(s_alarm_list, 380, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_alarm_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_alarm_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_alarm_list, 2, 0);
    lv_obj_remove_flag(s_alarm_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_alarm_list, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_flag(s_clock_tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_clock_tile, on_face_pressed, LV_EVENT_PRESSED, NULL);
}

/* ---------- 响铃 ---------- */

static void on_alarm_button(lv_event_t *e)
{
    user_activity();
    action((const char *)lv_event_get_user_data(e));
}

static void bell_wobble(void *obj, int32_t v) { lv_obj_set_style_translate_x(obj, v, 0); }

static lv_obj_t *add_alarm_button(lv_obj_t *parent, const char *text, const char *name, bool primary)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 170, 56);
    lv_obj_set_style_radius(btn, 28, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(primary ? C_ACCENT : C_CONTROL), 0);
    lv_obj_add_event_cb(btn, on_alarm_button, LV_EVENT_CLICKED, (void *)name);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    set_color(label, primary ? 0x0b1220 : C_TEXT);
    lv_obj_center(label);
    return btn;
}

static void create_alarm_layer(void)
{
    s_alarm_layer = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_alarm_layer);
    lv_obj_set_size(s_alarm_layer, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_alarm_layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_alarm_layer, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_alarm_layer, s_font, 0);
    lv_obj_add_flag(s_alarm_layer, LV_OBJ_FLAG_CLICKABLE);  // 挡住下面的页面
    lv_obj_remove_flag(s_alarm_layer, LV_OBJ_FLAG_SCROLLABLE);

    s_alarm_bell = add_label(s_alarm_layer, &nomi_font_icon_36, C_WARM);
    lv_label_set_text(s_alarm_bell, "\xEF\x83\xB3");  // 铃铛
    lv_obj_align(s_alarm_bell, LV_ALIGN_TOP_MID, 0, 22);
    s_alarm_time = add_label(s_alarm_layer, &nomi_font_clock_120, C_TEXT);
    lv_obj_align(s_alarm_time, LV_ALIGN_TOP_MID, 0, 66);
    s_alarm_label = add_label(s_alarm_layer, NULL, C_TEXT);
    lv_obj_set_width(s_alarm_label, FACE_W - 60);
    lv_obj_set_style_text_align(s_alarm_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_alarm_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_alarm_label, 52);
    lv_obj_align(s_alarm_label, LV_ALIGN_TOP_MID, 0, 170);

    lv_obj_t *row = lv_obj_create(s_alarm_layer);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 56);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 24, 0);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -24);
    add_alarm_button(row, "稍后提醒", "alarm_snooze", false);
    add_alarm_button(row, "知道了", "alarm_stop", true);
}

void nomi_ui_alarm_show(const char *time, const char *label)
{
    if (!s_alarm_layer) create_alarm_layer();
    lv_label_set_text(s_alarm_time, time ? time : "");
    lv_label_set_text(s_alarm_label, label && label[0] ? label : "闹钟");
    lv_obj_remove_flag(s_alarm_layer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_alarm_layer);

    // 铃铛左右晃
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_alarm_bell);
    lv_anim_set_exec_cb(&a, bell_wobble);
    lv_anim_set_values(&a, -6, 6);
    lv_anim_set_duration(&a, 120);
    lv_anim_set_reverse_duration(&a, 120);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

void nomi_ui_alarm_hide(void)
{
    if (!s_alarm_layer) return;
    lv_anim_delete(s_alarm_bell, bell_wobble);
    lv_obj_set_style_translate_x(s_alarm_bell, 0, 0);
    lv_obj_add_flag(s_alarm_layer, LV_OBJ_FLAG_HIDDEN);
    clock_refresh(true);  // 一次性的闹钟响完就没了
}

// 外层开始上下翻：从主界面往下拉时，设置页上的值有变化才重建；没变只刷新一下信号强度
static void on_outer_scroll(lv_event_t *e)
{
    (void)e;
    user_activity();
    if (lv_tileview_get_tile_active(s_outer) != s_main_tile) return;
    char sig[sizeof(s_settings_sig)];
    settings_signature(sig, sizeof(sig));
    if (strcmp(sig, s_settings_sig) != 0) {
        build_settings();
    } else if (s_wifi_label) {
        const char *wifi = get_text("wifi");
        lv_label_set_text(s_wifi_label, wifi[0] ? wifi : "没连上");
    }
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

    // 主界面里从左到右：历史对话、表情（首页）、电台、时钟
    s_modes = add_tileview(s_main_tile);
    s_hist_tile = add_tile(s_modes, PAGE_HIST, 0, LV_DIR_RIGHT, C_PAGE);
    s_chat_tile = add_tile(s_modes, PAGE_FACE, 0, LV_DIR_HOR, 0x000000);
    s_music_tile = add_tile(s_modes, PAGE_MUSIC, 0, LV_DIR_HOR, 0x000000);
    s_clock_tile = add_tile(s_modes, PAGE_CLOCK, 0, LV_DIR_LEFT, 0x000000);
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

    build_music_page();
    build_clock_page();

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

    create_dots();

    build_settings();
    show_main(false);
    show_page(PAGE_FACE, false);  // 让左右翻页的 tileview 也有"当前页"（不设的话一直是 NULL，直到第一次滑动）
    s_dots_until = lv_tick_get() + 4000;  // 开机时亮一会儿，告诉人左右还有页面

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
