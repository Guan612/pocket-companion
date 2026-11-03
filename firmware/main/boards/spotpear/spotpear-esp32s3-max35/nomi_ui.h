// Nomi 界面：四页（历史对话 / 表情 / 电台 / 时钟）左右滑切换，顶部下拉打开设置。
//
// 说话靠 BOOT 键和唤醒词，触摸只负责切模式、开设置和叫醒。
// 只依赖 LVGL 和 face.c，设备（NomiDisplay）和浏览器模拟器（tools/device-sim）共用同一份代码。
// 设置项的存取和平台相关的动作都通过回调交给调用方。所有函数都要在持有 LVGL 锁的上下文里调用。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NOMI_STATE_IDLE,
    NOMI_STATE_CONNECTING,
    NOMI_STATE_LISTENING,
    NOMI_STATE_SPEAKING,
    NOMI_STATE_ERROR,
    NOMI_STATE_WIFI_SETUP,  // 配网模式（开着配网热点）：状态文字常显
    NOMI_STATE_THINKING,    // 一句话录完、还没开口：识别 / 大模型 / 工具调用中，已经不在收音
} nomi_state_t;

// 设置项（get_int / set_int 的 key）
#define NOMI_KEY_TALK_KEY "talk_key"      // BOOT 键：0 按住说话，1 点一下开始
#define NOMI_KEY_VOLUME "volume"          // 0..100
#define NOMI_KEY_BRIGHTNESS "brightness"  // 10..100
#define NOMI_KEY_AEC "aec"                // 随时插话打断（设备端回声消除）
#define NOMI_KEY_AEC_SUPPORTED "aec_ok"   // 只读：硬件有没有喇叭回采
#define NOMI_KEY_SUBTITLES "subtitles"    // 显示字幕
#define NOMI_KEY_TTS_SPEAK "tts_speak"    // 语音播报（静音模式）：0 关——只出字幕，不调合成。
                                          // 存在 directapi 命名空间，网页设置页 / self.voice.set_speak 共用
#define NOMI_KEY_PERSONA "persona"        // personas 列表里的下标，-1 为自定义
#define NOMI_KEY_VOICE "voice"            // voices 列表里的下标，-1 为自定义
#define NOMI_KEY_DIM "dim"                // NOMI_DIM_SECONDS 的下标
#define NOMI_KEY_OFF "off"                // NOMI_OFF_SECONDS 的下标
#define NOMI_KEY_SLEEP_SHOW "sleep_show"  // 睡着时显示：0 时钟页，1 表情（还没对上时间时总是表情）
#define NOMI_KEY_ONE_SHOT "one_shot"      // 一问一答：1 = 回答完一句就回待机，0 = 连续对话（默认）

// 多久后调暗 / 息屏，-1 表示从不；设置页的选项和这里一一对应
#define NOMI_DIM_SECONDS {30, 60, 120, 300, -1}
#define NOMI_OFF_SECONDS {120, 300, 600, 1800, -1}

typedef struct {
    nomi_state_t (*get_state)(void);
    void (*user_activity)(void);  // 有触摸操作（用来重置休眠计时）
    int (*get_int)(const char *key, int def);
    // 拖动滑条过程中 final 为 false，松手时为 true（设备只在 final 时写 NVS）
    void (*set_int)(const char *key, int value, bool final);
    // 只读文字：personas、voices（'\n' 分隔）、wifi（带信号）、wifi_ssid、ip、config_url、version、
    // saved_wifi（已存的 Wi-Fi，'\n' 分隔）、hotspot / hotspot_url（配网热点名和网址，不在配网模式时为空）。
    // 时钟页：clock_time（"14:32"，没对上时间为空）、clock_date、weather_temp（"26°"）、
    // weather_text（"多云 · 深圳  23~32°"，城市没找到时是提示）、weather_city（设的城市，空 = 自动）、
    // alarms（接下来要响的闹钟，按时间排，一行一个 "编号\t什么时候\t事由"）。
    // get_int("weather_code") 是 WMO 天气代码，夜里加 1000，没有天气时 -1。
    // 电台页：radio_name（当前台名）、radio_title（流里带的歌名，没有为空）、radio_list（台名，'\n' 分隔）；
    // get_int("radio_state") 0 没在放 / 1 连接中 / 2 在放 / 3 对话中暂停 / 4 连不上 / 5 为加载别的暂停，
    // get_int("radio_index") 当前台在台单里的下标（搜来没收藏的台为 -1）。
    // 播客：pod_list（订阅名）、pod_episodes（打开的订阅最新几期，一行一期 "标题\t日期 · 时长"）、
    // pod_name / pod_title（正在放的播客名、这期标题；放的是歌时是歌手、歌名）；get_int：pod_playing（放的是播客或歌）、
    // pod_opened（打开的是第几个订阅，-1 在订阅列表）、pod_load（0 好了 1 加载中 2 失败）、pod_pos / pod_dur（秒）。
    // 音乐：get_int("music_playing")（放的是在线音乐的一首歌）、music_lyrics（7 行歌词，第 4 行是唱到的这句；
    // 没有歌词为空）、music_hint（没在放歌时显示的提示）。
    // 不认识的 key 返回空串
    const char *(*get_text)(const char *key);
    // 动作：test_api、reconfigure_wifi、factory_reset、alarm_stop / alarm_snooze（响铃时"知道了" /
    // "稍后提醒"）、alarm_delete:<编号>（时钟页上删闹钟）、radio_toggle / radio_next / radio_prev、
    // radio_play:<下标>（电台页）、pod_open:<下标> / pod_back / pod_play:<下标>、pod_seek:<秒>、pod_seek_to:<秒>
    void (*action)(const char *name);
    // 聊天历史（第二页）：下标 0 最旧；role 为 "user" / "assistant"，文本只需在下一次调用前有效
    int (*history_count)(void);
    const char *(*history_get)(int index, const char **role);
    // 可选：记录满了从头上丢掉的消息总数（只增不减）。有它历史页只增量更新，不用每次重建整个列表
    int (*history_dropped)(void);
    // 可选：图片（RGB565，size 见方），"pod_cover" 是正在放的播客的封面，"music_cover" 是正在放的歌的专辑封面；
    // 没有返回 NULL。缓冲要一直有效（换图时换一块新的、version 加一）
    const uint16_t *(*get_image)(const char *key, int *size, int *version);
} nomi_ui_callbacks_t;

// face_buf：480×320 RGB565 画布缓冲，由调用方分配；font：中文字体
void nomi_ui_create(lv_obj_t *screen, uint16_t *face_buf, const lv_font_t *font,
                    const nomi_ui_callbacks_t *callbacks);

// 换字体（比如资源分区加载出完整中文字体之后）
void nomi_ui_set_font(const lv_font_t *font);

void nomi_ui_set_status(const char *status);
// role 为 "user" / "assistant"；text 为空时隐藏字幕
void nomi_ui_set_subtitle(const char *role, const char *text);
void nomi_ui_set_emotion(const char *emotion);
void nomi_ui_notify(const char *text, uint32_t duration_ms);

// 休眠：表情闭眼；休眠中点屏幕只唤醒
void nomi_ui_set_sleep(bool sleeping);
// 右上角电量；level < 0 表示没有电池信息，隐藏
void nomi_ui_set_battery(int level, bool charging);

// 闹钟响了：盖住整屏显示时间和事由，带"知道了""稍后提醒"两个按钮（点了走 action）
void nomi_ui_alarm_show(const char *time, const char *label);
void nomi_ui_alarm_hide(void);

// 播放一段表情脚本（JSON，格式见 face_script.h）；不合格返回 false，原因写进 err
bool nomi_ui_play_face(const char *json, char *err, size_t err_size);

// 首页「收听」那张卡片交给应用包画（音乐应用的卡片，像 Wear OS 的 tile）：返回盖住系统收听页的全屏容器
// （第一次调时建）。release 清空、藏起来，露出系统自己的收听页（没装音乐应用时就是它）
lv_obj_t *nomi_ui_listen_slot(void);
void nomi_ui_listen_slot_release(void);

// 在 parent（一般是上面那个卡片容器）里建一份「正在放」：和系统收听页一个样——左半边封面、标题、
// 上一首 / 放停 / 下一首、进度或音量，右半边歌词——跟着系统一起刷新。hint 是没歌词可显示时右边中间那行。
// 返回顶上一排的容器（系统收听页放标签的地方），应用往里放自己的入口按钮；控件删掉时自动注销。
// 同时最多两份，超了返回 NULL
lv_obj_t *nomi_ui_now_playing(lv_obj_t *parent, const char *hint);

#ifdef __cplusplus
}
#endif
