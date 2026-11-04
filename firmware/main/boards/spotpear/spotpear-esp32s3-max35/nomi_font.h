// 看书用的字：系统字（资源分区里的常用字子集，约 6800 个字，GB2312 的汉字都缺两千多）为主，
// 系统字里没有的字从 SD 卡 fonts/ 文件夹里的第一个 .ttf 补（LVGL 的 tiny_ttf，按需从卡上读字形，带缓存）。
// 卡上放和系统字同一款的字体（Noto Sans SC 的 TrueType 版），补出来的字看不出区别。
// 卡上没有字体就只有系统字，缺的字显示不出来。.otf（CFF 轮廓）和可变字体 tiny_ttf 画不对，要用静态的 .ttf。
#pragma once

#include "lvgl.h"

namespace NomiFont {

// 系统字：显示那边设好字体时调（资源分区里的字，一直有效）
void SetBase(const lv_font_t* font);

// 显示锁里调。第一个用的人进来时挂卡、打开卡上的字体；最后一个 Release 时关掉、放开卡
const lv_font_t* AcquireReading();
void ReleaseReading();

}  // namespace NomiFont
