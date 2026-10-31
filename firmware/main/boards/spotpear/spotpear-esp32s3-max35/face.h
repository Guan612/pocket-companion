// 方形 Nomi 表情：状态机 + 弹簧动画 + 绘制。
//
// 与 preview/nomi-square.html 一一对应，电脑上可用 tools/face-sim 预览：逻辑坐标 240×160，乘 2 画到 480×320。
// 不依赖 ESP-IDF，可以在电脑上编译测试。
#pragma once

#include <stdint.h>

#include "raster.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_W 480
#define FACE_H 320

typedef enum {
    FACE_IDLE,
    FACE_LISTEN,
    FACE_THINK,
    FACE_SPEAK,
    FACE_HAPPY,
    FACE_CONFUSED,
    FACE_MODE_COUNT,
} face_mode_t;

void face_init(uint32_t seed);
void face_set_mode(face_mode_t mode);
face_mode_t face_get_mode(void);
const char *face_mode_name(face_mode_t mode);

// 说话音量 0..1，由音频播放驱动；传负数表示没有真实音量，说话时用模拟音量
void face_set_level(float level);

// 睡着：闭眼、收手、缓慢呼吸，盖过当前状态；醒来后回到原状态
void face_set_sleep(int sleeping);

// 按固定步长推进动画，建议 dt = 1/60 秒
void face_step(float dt);

// 画一帧到 fb（FACE_W×FACE_H，RGB565），返回需要刷到屏幕的区域
rrect_t face_render(uint16_t *fb);

/* ---------- 表情脚本：大模型现编的一小段表演 ----------
 *
 * 几步（每步一个姿势 + 时长）依次播放，播完回到跟着设备状态走的表情。每步只覆盖写了的部分，
 * 没写的仍按状态来（比如说话时嘴的动作）。道具是白色线条画，坐标 0～100；内置图标也是转成线条再画的。
 * JSON 怎么写、怎么解析见 face_script.h。
 */
#define FACE_SCRIPT_MAX_STEPS 6
#define FACE_SCRIPT_MAX_STROKES 16
#define FACE_SCRIPT_MAX_POINTS 96
#define FACE_SCRIPT_MAX_SECONDS 10.0f

typedef enum {
    FACE_EYES_AUTO,  // 跟着状态
    FACE_EYES_CAPSULE,
    FACE_EYES_ARC,  // ∩ 笑眯眯
    FACE_EYES_RING,  // 0 睁大的空心眼（NOMI 最常见的睁眼）
    FACE_EYES_DASH,  // — 平静
    FACE_EYES_DOT,
    FACE_EYES_CLOSED,
    FACE_EYES_HEART,
    FACE_EYES_STAR,
    FACE_EYES_X,
    FACE_EYES_SHADES,  // 墨镜
    FACE_EYES_SPIRAL,  // 转圈的螺旋（晕）
} face_eyes_t;

typedef enum {
    FACE_HANDS_AUTO,
    FACE_HANDS_NONE,
    FACE_HANDS_CHIN,  // 托腮
    FACE_HANDS_EAR,   // 手放耳边
    FACE_HANDS_WAVE,
    FACE_HANDS_UP,  // 举双手
    FACE_HANDS_COVER,  // 捂眼
    FACE_HANDS_MIC,
    FACE_HANDS_CHEEKS,  // 双手托腮
} face_hands_t;

typedef enum { FACE_MOTION_NONE, FACE_MOTION_NOD, FACE_MOTION_BOUNCE, FACE_MOTION_SHAKE, FACE_MOTION_SWAY } face_motion_t;
typedef enum { FACE_BROW_NONE, FACE_BROW_UP, FACE_BROW_DOWN } face_brow_t;
typedef enum {
    FACE_PROP_ANIM_NONE,
    FACE_PROP_ANIM_BOB,
    FACE_PROP_ANIM_SWING,
    FACE_PROP_ANIM_SPIN,
    FACE_PROP_ANIM_FLOAT,  // 往上飘、淡出，循环（音符、蒸汽）
    FACE_PROP_ANIM_POP,    // 弹出来
    FACE_PROP_ANIM_ORBIT,  // 三个缩小的道具绕着转（晕了的星星、绕头的爱心）
} face_prop_anim_t;
typedef enum { FACE_PROP_AT_MOUTH, FACE_PROP_AT_LEFT, FACE_PROP_AT_RIGHT, FACE_PROP_AT_TOP } face_prop_at_t;

// 一笔：折线（pts 里从 start 起连续 count 个点）、圆圈或实心点（pts[start] 是圆心，r 是半径）
enum { FACE_STROKE_LINE, FACE_STROKE_RING, FACE_STROKE_DOT };
typedef struct {
    uint8_t kind, start, count, r;
} face_stroke_t;

typedef struct {
    float dur;
    uint8_t eyes, hands, motion, brow, sweat, prop_anim, prop_at;
    float look_x, look_y, tilt, blush;  // 没写的是 NAN，交给状态
    uint8_t stroke0, nstrokes;           // 道具的线条在 strokes[] 里的范围，nstrokes 为 0 表示没有道具
} face_step_t;

typedef struct {
    face_step_t steps[FACE_SCRIPT_MAX_STEPS];
    int nsteps;
    face_stroke_t strokes[FACE_SCRIPT_MAX_STROKES];
    int nstrokes;
    uint8_t pts[FACE_SCRIPT_MAX_POINTS][2];
    int npts;
} face_script_t;

// 开始播放（会拷贝一份）；睡着时不播
void face_play(const face_script_t *script);
void face_stop(void);
int face_is_playing(void);

#ifdef __cplusplus
}
#endif
