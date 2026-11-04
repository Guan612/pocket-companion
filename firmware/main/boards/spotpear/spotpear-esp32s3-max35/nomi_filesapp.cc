#include "nomi_filesapp.h"

#include "display/display.h"
#include "nomi_app.h"
#include "nomi_filelist.h"
#include "nomi_files.h"
#include "nomi_open.h"
#include "nomi_sdcard.h"
#include "nomi_sheet.h"
#include "nomi_style.h"
#include "nomi_ui.h"
#include "nomi_wasm.h"

#include <ctime>
#include <string>

namespace {

lv_obj_t* s_list;     // 开着时的文件列表
std::string s_dir;    // 上次看到哪一层（关了再开接着看，重启从头）
// 长按菜单开着时：对着哪个文件，删除点过一次没有
std::string s_menu_rel;
lv_obj_t* s_menu_delete;

std::string TimeText(int64_t t) {
    if (t <= 0) return "";
    time_t tt = static_cast<time_t>(t);
    struct tm tm;
    localtime_r(&tt, &tm);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

void OnDelete(lv_event_t* e) {
    lv_obj_t* row = lv_event_get_current_target_obj(e);
    if (s_menu_delete != row) {  // 第一下：变成「再点一下删除」
        s_menu_delete = row;
        lv_label_set_text(lv_obj_get_child(row, 0), "再点一下删除（删了找不回来）");
        return;
    }
    std::string rel = s_menu_rel;
    NomiSheet::Close();
    nomi_ui_notify("正在删…", 1500);
    Display* display = NomiWasm::GetDisplay();
    NomiFileList::RunInBackground([rel, display] {
        std::string err;
        {
            NomiSdCard::Use sd;
            err = sd ? NomiFiles::Remove(rel) : "SD 卡没插";
        }
        DisplayLockGuard lock(display);
        nomi_ui_notify(err.empty() ? "删好了" : err.c_str(), 2500);
        if (s_list) NomiFileList::Refresh(s_list);
    });
}

void ShowMenu(const std::string& rel, const NomiFiles::Entry& e) {
    std::string sub = e.dir ? "文件夹" : NomiFileList::SizeText(e.size);
    std::string when = TimeText(e.mtime);
    if (!when.empty()) sub += " · " + when;
    lv_obj_t* panel = NomiSheet::Open(e.name, sub, [] {
        s_menu_rel.clear();
        s_menu_delete = nullptr;
    });
    s_menu_rel = rel;
    if (!e.dir) {
        NomiSheet::AddButton(
            panel, "打开方式…", C_TEXT,
            [](lv_event_t*) {
                std::string rel = s_menu_rel;
                NomiSheet::Close();
                NomiOpen::Choose(rel);
            },
            nullptr);
    }
    NomiSheet::AddButton(panel, e.dir ? "删除（连里面的东西）" : "删除", C_DANGER, OnDelete, nullptr);
    NomiSheet::AddButton(panel, "取消", C_MUTED, [](lv_event_t*) { NomiSheet::Close(); }, nullptr);
}

void FilesOpen(const nomi_app_t*, lv_obj_t* root) {
    // 和应用的页面一个样子：黑底一列、上下滚
    lv_obj_t* page = lv_obj_create(root);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(page, 36, 0);
    lv_obj_set_style_pad_ver(page, 20, 0);
    lv_obj_set_style_pad_row(page, 12, 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_AUTO);
    lv_label_set_text(lv_label_create(page), "文件");
    NomiFileList::Options opt;
    opt.on_pick = [](const std::string& rel, bool) { NomiOpen::Open(rel); };
    opt.on_long = ShowMenu;
    s_list = NomiFileList::Create(page, s_dir, opt);
}

void FilesClose(const nomi_app_t*) {
    if (s_list) s_dir = NomiFileList::Dir(s_list);
    s_list = nullptr;
    if (!s_menu_rel.empty()) NomiSheet::Close();  // 长按菜单还开着
}

bool FilesBack(const nomi_app_t*) { return s_list && NomiFileList::Up(s_list); }

// 图标：文件夹（带个页签）
const lv_point_precise_t kFolder[] = {{4, 10}, {4, 29}, {32, 29}, {32, 13}, {17, 13}, {14, 8}, {4, 8}, {4, 10}};
const lv_point_precise_t kFolderTab[] = {{4, 17}, {32, 17}};
const nomi_icon_line_t kLines[] = {{kFolder, 8}, {kFolderTab, 2}};
const nomi_icon_t kIcon = {kLines, 2, nullptr, 0};

nomi_app_t s_app = {
    .id = "files",
    .name = "文件",
    .icon = &kIcon,
    .open = FilesOpen,
    .close = FilesClose,
    .back = FilesBack,
    .context = nullptr,
    .user = nullptr,
    .dimmed = false,
    .dimmed_hint = nullptr,
    .system = true,
};

}  // namespace

namespace NomiFilesApp {

void Register() { nomi_apps_add(&s_app); }

}  // namespace NomiFilesApp
