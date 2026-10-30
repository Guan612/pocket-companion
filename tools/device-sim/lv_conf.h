// 设备模拟器的 LVGL 配置。只写和默认值不同的项，其余由 lv_conf_internal.h 补齐。
// LVGL 版本和固件一致（9.5.0），中文字体用 LVGL 自带的思源黑体子集，设备上用的是小智的字体。
#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_DEF_REFR_PERIOD 33
#define LV_USE_LOG 0

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_SOURCE_HAN_SANS_SC_16_CJK 1
// 小智字体文件按"大字体"格式生成（字形索引 32 位），必须和设备一致
#define LV_FONT_FMT_TXT_LARGE 1
// 只为带进 qrcodegen（设备上由 esp_emote_gfx 提供），nomi_ui 不用 qrcode 控件
#define LV_USE_QRCODE 1

#include "lv_conf_device.h"

#endif
