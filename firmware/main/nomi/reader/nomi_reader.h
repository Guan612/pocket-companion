// 阅读器：一个占满父对象的控件，给它一个 TXT 文件和起始位置，系统排版、分页、翻页（应用包的 ui_reader）。
//
// - 读：NomiBook（认 UTF-8 / GBK，空行去掉、段首缩进两格）；字：NomiFont（系统字为主，缺的字用卡上的 .ttf 补）。
// - 排版用 LVGL 自己的断行（lv_text_get_next_line），一页的每一行都排好再交给标签显示，显示和分页对得上。
// - 往后翻：从这页结尾接着排。往回翻：刚往后翻过的原样退回去；从书签、目录跳进来的没有记录，就从前面的
//   段落开头排到这页开头，取最后一整页（最后一行可能不满）。
// - 手势按 flags：kSwipe 上滑下一页、下滑上一页；kTap 点上三分之一上一页、下三分之一下一页；kTapLr 换成
//   点左三分之一上一页、右三分之一下一页。不管哪种，点中间（三分之一那段）和长按都是要菜单（快速跳转）。
//   左边缘右划是应用层的返回，不归这里。
// 所有函数都要在显示锁里调。
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "lvgl.h"

namespace NomiReader {

enum Flags { kSwipe = 1, kTap = 2, kTapLr = 4 };
enum Event {
    kPage,  // 翻到新的一页：offset 是这页开头在文件里的位置（存下来就是书签）
    kMenu,  // 点中间、长按
    kEnd,   // 在最后一页还往后翻
};
using Callback = std::function<void(Event ev, uint32_t offset)>;

// path 是设备上的路径。打不开返回 nullptr，error 里是给人看的原因。建好时就来一次 kPage
// Goto 给的位置对齐到那一段的开头（拖进度条、跳章不会从半句话开始）；Create 给的是书签，原样用
lv_obj_t* Create(lv_obj_t* parent, const std::string& path, uint32_t offset, int flags, Callback cb,
                 std::string& error);
bool Is(lv_obj_t* obj);
void Goto(lv_obj_t* reader, uint32_t offset);
void SetFlags(lv_obj_t* reader, int flags);
std::string PageText(lv_obj_t* reader);

}  // namespace NomiReader
