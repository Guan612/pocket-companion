// 软件光栅：在 RGB565 帧缓冲上画抗锯齿的基础图形。
//
// 图形都用"有符号距离"求覆盖率，坐标是逻辑单位，乘 scale 才是屏幕像素。
// 变换只支持平移、旋转和 ±1 缩放（镜像），这样逆变换就是转置，距离也不失真。
// 不依赖 ESP-IDF，可以在电脑上编译测试。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef struct {
    float a, b, c, d, e, f;  // 和 canvas 一致：x' = a*x + c*y + e，y' = b*x + d*y + f
} rxf_t;

typedef struct {
    int x0, y0, x1, y1;  // x1、y1 不含；x1 <= x0 表示空
} rrect_t;

typedef struct {
    uint16_t *fb;
    int w, h;
    float scale;
    rrect_t dirty;  // 本帧画过的像素范围
} raster_t;

void raster_begin(raster_t *r, uint16_t *fb, int w, int h, float scale);

rxf_t rxf_identity(void);
rxf_t rxf_translate(rxf_t m, float x, float y);
rxf_t rxf_rotate(rxf_t m, float rad);
rxf_t rxf_scale(rxf_t m, float sx, float sy);

rrect_t rrect_union(rrect_t a, rrect_t b);
int rrect_empty(rrect_t a);

void raster_clear(raster_t *r, rrect_t area, uint16_t color);
void raster_rrect(raster_t *r, rxf_t m, float cx, float cy, float w, float h, float radius,
                  uint16_t color, float alpha);
void raster_ellipse(raster_t *r, rxf_t m, float cx, float cy, float rx, float ry, float rot,
                    uint16_t color, float alpha);
void raster_arc(raster_t *r, rxf_t m, float cx, float cy, float radius, float a0, float a1,
                float width, uint16_t color, float alpha);
void raster_line(raster_t *r, rxf_t m, float x0, float y0, float x1, float y1, float width,
                 uint16_t color, float alpha);
void raster_star(raster_t *r, rxf_t m, float cx, float cy, float radius, uint16_t color,
                 float alpha);
// 径向渐变光斑：中心透明度 alpha，到半径处线性降到 0
void raster_glow(raster_t *r, rxf_t m, float cx, float cy, float radius, uint16_t color,
                 float alpha);

#ifdef __cplusplus
}
#endif
