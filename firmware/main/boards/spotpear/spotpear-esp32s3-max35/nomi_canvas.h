// 画布（系统控件）：应用自己画的东西（地图的转向箭头、轨迹、指南针、仪表）。
//
// 不开像素缓冲：存一份「要画什么」的清单（折线、实心多边形、圆、文字），LVGL 刷新这块时照着清单画，
// 占内存少、细线也是抗锯齿的。应用先 Clear，再一样样加，最后 Show 一次整份换上去——画到一半不会被刷到屏幕上。
// 坐标以画布左上角为原点，像素；颜色 0xRRGGBB。都在显示锁里调。
#pragma once

#include <cstdint>
#include <string>

#include "lvgl.h"

namespace NomiCanvas {

enum TextSize { kNormal = 0, kBig = 1 };     // 系统字；大数字（120 像素，只有 0-9 和冒号）
enum Align { kLeft = 0, kCenter = 1, kRight = 2 };  // 文字的 x 是左边、中间还是右边

// 在 parent 里建一块画布：height 为 0 时占满 parent（把 parent 的留白去掉、不让滚），否则宽占满、高 height
lv_obj_t* Create(lv_obj_t* parent, int height);
bool Is(lv_obj_t* obj);

void Clear(lv_obj_t* canvas);
void Line(lv_obj_t* canvas, const int32_t* xy, int points, int width, uint32_t color);  // 折线，两头圆的
void Fill(lv_obj_t* canvas, const int32_t* xy, int points, uint32_t color);             // 实心凸多边形
void Circle(lv_obj_t* canvas, int x, int y, int r, int width, uint32_t color);           // width 0 = 实心
// y 是文字的顶；返回文字宽度（像素），排版用
int Text(lv_obj_t* canvas, int x, int y, const std::string& text, int size, uint32_t color, int align);
void Show(lv_obj_t* canvas);  // 把清单换上屏幕

int Width(lv_obj_t* canvas);
int Height(lv_obj_t* canvas);

}  // namespace NomiCanvas
