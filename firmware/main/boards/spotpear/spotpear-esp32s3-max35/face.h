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

#ifdef __cplusplus
}
#endif
