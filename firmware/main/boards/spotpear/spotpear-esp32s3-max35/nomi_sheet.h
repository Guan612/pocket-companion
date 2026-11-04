// 浮层面板（系统）：压在所有东西上面、四周压暗，点外面收起。「用什么打开」、「文件」里长按的菜单都用它。
// 一次只有一个：新开的顶掉旧的；要睡着了收起（nomi_display.cc，不然压在时钟上）。都在显示锁里调。
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "lvgl.h"

namespace NomiSheet {

// 开一个：标题，下面一行小字（可以空）。返回面板，往里加按钮。on_close 收起时调（不管怎么收的）
lv_obj_t* Open(const std::string& title, const std::string& sub, std::function<void()> on_close = nullptr);
// 一行按钮（整行能点，字是 color 色）。返回这一行：里面是一个横排，要放图标就建好挪到最前面
lv_obj_t* AddButton(lv_obj_t* panel, const std::string& text, uint32_t color, lv_event_cb_t cb, void* user);
// 一行空的横排（44 高），自己往里放东西
lv_obj_t* AddRow(lv_obj_t* panel);
void Close();

}  // namespace NomiSheet
