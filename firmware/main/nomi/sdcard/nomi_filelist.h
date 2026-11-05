// 卡上文件的列表（系统控件）：一层层点进去，顶上是路径和「上一级」。设备上的「文件」、应用的 ui_files 都用它。
// 列文件夹、删东西放到后台做（卡上东西多时要一会儿），界面不卡；一次先画 60 行，多的点「再列」。
// 都在显示锁里调；回调也在显示锁里调。
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "lvgl.h"
#include "nomi_files.h"

namespace NomiFileList {

struct Options {
    std::string top;                // 最上到哪（卡上的相对路径，空 = 卡的根）：不往上走过它
    std::vector<std::string> exts;  // 只列这些扩展名的文件（小写，不带点；空 = 都列）。文件夹都列
    bool pick_dir = false;          // 选文件夹：每层顶上一行「选这个文件夹」
    // 点了文件（dir false），或选了文件夹（dir true）。rel 是卡上的相对路径
    std::function<void(const std::string& rel, bool dir)> on_pick;
    // 长按文件或文件夹（不给就不响应长按）
    std::function<void(const std::string& rel, const NomiFiles::Entry& e)> on_long;
};

// 在 parent 里建一个列表（宽占满、高随内容：放在会上下滚的页面里），先列 dir
lv_obj_t* Create(lv_obj_t* parent, const std::string& dir, Options opt);
bool Up(lv_obj_t* list);        // 回上一层；已经在最上面返回 false
void Refresh(lv_obj_t* list);   // 重新列这一层（删了东西之后）
std::string Dir(lv_obj_t* list);

// 卡上的活放到后台做（列文件夹、删东西），一个一个来。job 里要碰界面自己拿显示锁
void RunInBackground(std::function<void()> job);
std::string SizeText(uint64_t bytes);  // "1.2 MB"

}  // namespace NomiFileList
