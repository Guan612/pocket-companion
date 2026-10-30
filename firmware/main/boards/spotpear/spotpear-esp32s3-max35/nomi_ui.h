// Nomi 界面：全屏表情 + 状态栏 + 字幕 + 通知 + 设置页。
//
// 只依赖 LVGL 和 face.c，设备（NomiDisplay）和浏览器模拟器（tools/device-sim）共用同一份代码。
// 设置项的存取和平台相关的动作都通过回调交给调用方。
// 所有函数都要在持有 LVGL 锁的上下文里调用。
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
} nomi_state_t;

// 设置项（get_int / set_int 的 key）
#define NOMI_KEY_VOLUME "volume"          // 0..100
#define NOMI_KEY_BRIGHTNESS "brightness"  // 10..100
#define NOMI_KEY_CHAT_MODE "chat_mode"    // 0 点一下开始，1 按住说话
#define NOMI_KEY_AEC "aec"                // 随时插话打断（设备端回声消除）
#define NOMI_KEY_AEC_SUPPORTED "aec_ok"   // 只读：硬件有没有喇叭回采
#define NOMI_KEY_SUBTITLES "subtitles"    // 显示字幕
#define NOMI_KEY_PERSONA "persona"        // personas 列表里的下标，-1 为自定义
#define NOMI_KEY_VOICE "voice"            // voices 列表里的下标，-1 为自定义
#define NOMI_KEY_DIM "dim"                // NOMI_DIM_SECONDS 的下标
#define NOMI_KEY_OFF "off"                // NOMI_OFF_SECONDS 的下标

// 多久后调暗 / 息屏，-1 表示从不；设置页的选项和这里一一对应
#define NOMI_DIM_SECONDS {30, 60, 120, 300, -1}
#define NOMI_OFF_SECONDS {120, 300, 600, 1800, -1}

typedef struct {
    nomi_state_t (*get_state)(void);
    void (*toggle_chat)(void);      // 点一下模式：开始 / 结束对话
    void (*start_listening)(void);  // 按住说话模式：按下
    void (*stop_listening)(void);   // 按住说话模式：松开
    void (*user_activity)(void);    // 有触摸操作（用来重置休眠计时）
    int (*get_int)(const char *key, int def);
    // 拖动滑条过程中 final 为 false，松手时为 true（设备只在 final 时写 NVS）
    void (*set_int)(const char *key, int value, bool final);
    // 只读文字：personas、voices（'\n' 分隔）、wifi、ip、config_url、version
    const char *(*get_text)(const char *key);
    // 动作：take_photo、test_api、reconfigure_wifi、factory_reset
    void (*action)(const char *name);
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

// 休眠：表情闭眼；休眠中点屏幕只唤醒，不触发对话
void nomi_ui_set_sleep(bool sleeping);
// 右上角电量；level < 0 表示没有电池信息，隐藏
void nomi_ui_set_battery(int level, bool charging);

#ifdef __cplusplus
}
#endif
