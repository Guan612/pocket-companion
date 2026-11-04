#include "nomi_open.h"

#include "application.h"
#include "display/display.h"
#include "nomi_app.h"
#include "nomi_display.h"
#include "nomi_files.h"
#include "nomi_font.h"
#include "nomi_pkg.h"
#include "nomi_sdcard.h"
#include "nomi_sheet.h"
#include "nomi_style.h"
#include "nomi_ui.h"
#include "nomi_wasm.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>
#include <sys/stat.h>

#include <map>
#include <mutex>

#define TAG "NomiOpen"

namespace {

constexpr const char* kNs = "nomi_open";  // NVS：一个 "map"，一行一个「扩展名=应用 id」

std::mutex s_mutex;
std::map<std::string, std::string> s_defaults;  // 扩展名 → 应用 id

// 选应用的浮层开着时（显示锁里读写）：选的是哪个文件、「以后都用它」的开关
struct Chooser {
    lv_obj_t* remember = nullptr;
    std::string rel, ext;
};
Chooser s_chooser;

std::string ExtOf(const std::string& rel) {
    size_t slash = rel.rfind('/');
    size_t dot = rel.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot + 1 == rel.size()) return "";
    std::string ext = rel.substr(dot + 1);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    }
    return ext;
}

std::string NameOf(const std::string& rel) {
    size_t slash = rel.rfind('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

// 记住的默认写进 NVS：写 NVS 要在内部内存的栈上做，交给主循环
void Save() {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (const auto& [ext, id] : s_defaults) text += ext + "=" + id + "\n";
    }
    Application::GetInstance().Schedule([text]() {
        Settings s(kNs, true);
        s.SetString("map", text);
    });
}

std::string DefaultFor(const std::string& ext) {
    std::lock_guard<std::mutex> lock(s_mutex);
    auto it = s_defaults.find(ext);
    return it == s_defaults.end() ? "" : it->second;
}

void Launch(const NomiPackage* pkg, const std::string& rel) {
    ESP_LOGI(TAG, "%s -> %s", rel.c_str(), pkg->id.c_str());
    NomiWasm::OpenFile(pkg, "sd:/" + rel);
}

void OnPick(lv_event_t* e) {
    auto* pkg = static_cast<const NomiPackage*>(lv_event_get_user_data(e));
    std::string rel = s_chooser.rel, ext = s_chooser.ext;
    bool remember = s_chooser.remember && lv_obj_has_state(s_chooser.remember, LV_STATE_CHECKED);
    NomiSheet::Close();
    if (remember && !ext.empty()) {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_defaults[ext] = pkg->id;
        }
        Save();
    }
    Launch(pkg, rel);
}

// 弹出来选：一行一个应用，下面「以后 .ext 都用它」、取消。点外面也算取消
void ShowChooser(const std::string& rel, const std::string& ext, const std::vector<const NomiPackage*>& apps) {
    lv_obj_t* panel = NomiSheet::Open("用什么打开", NameOf(rel), [] { s_chooser = Chooser(); });
    s_chooser.rel = rel;
    s_chooser.ext = ext;
    for (const NomiPackage* pkg : apps) {
        lv_obj_t* row = NomiSheet::AddButton(panel, pkg->name, pkg->app.dimmed ? C_DIMMED : C_TEXT, OnPick,
                                             const_cast<NomiPackage*>(pkg));
        lv_obj_t* icon = lv_obj_create(row);
        lv_obj_remove_style_all(icon);
        lv_obj_set_size(icon, 32, 32);
        lv_obj_remove_flag(icon, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        nomi_icon_draw(icon, pkg->app.icon, 32);
        lv_obj_move_to_index(icon, 0);
    }
    if (!ext.empty()) {
        lv_obj_t* row = NomiSheet::AddRow(panel);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);  // 点整行都能拨开关
        lv_obj_t* label = lv_label_create(row);
        lv_obj_set_flex_grow(label, 1);
        lv_label_set_text(label, ("以后 ." + ext + " 都用它").c_str());
        lv_obj_t* sw = s_chooser.remember = lv_switch_create(row);
        lv_obj_set_size(sw, 52, 28);
        lv_obj_set_style_bg_color(sw, lv_color_hex(C_CONTROL), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, lv_color_hex(C_ACCENT),
                                  static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED);
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* e) {
                lv_obj_t* sw = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
                if (lv_event_get_target(e) == sw) return;  // 点的是开关本身，它自己会拨
                lv_obj_set_state(sw, LV_STATE_CHECKED, !lv_obj_has_state(sw, LV_STATE_CHECKED));
            },
            LV_EVENT_CLICKED, sw);
    }
    NomiSheet::AddButton(panel, "取消", C_MUTED, [](lv_event_t*) { NomiSheet::Close(); }, nullptr);
}

std::string NoApp(const std::string& ext) {
    return ext.empty() ? "没有能打开这个文件的应用" : "没有能打开 ." + ext + " 的应用";
}

std::string Json(bool ok, const std::string& key, const std::string& value) {
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ok);
    cJSON_AddStringToObject(out, key.c_str(), value.c_str());
    char* json = cJSON_PrintUnformatted(out);
    std::string result = json ? json : "{}";
    cJSON_free(json);
    cJSON_Delete(out);
    return result;
}

}  // namespace

namespace NomiOpen {

void Start() {
    std::string text;
    {
        Settings s(kNs, false);
        text = s.GetString("map");
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        size_t eq = line.find('=');
        if (eq != std::string::npos && eq > 0 && eq + 1 < line.size()) s_defaults[line.substr(0, eq)] = line.substr(eq + 1);
        pos = end + 1;
    }
}

void Open(const std::string& rel) {
    std::string ext = ExtOf(rel);
    auto apps = NomiPkg::GetInstance().AppsFor(ext);
    std::string def = DefaultFor(ext);
    for (const NomiPackage* pkg : apps) {
        if (pkg->id == def) return Launch(pkg, rel);
    }
    if (apps.empty()) {
        NomiSheet::Close();
        nomi_ui_notify(NoApp(ext).c_str(), 3000);
    } else if (apps.size() == 1) {
        NomiSheet::Close();
        Launch(apps[0], rel);
    } else {
        ShowChooser(rel, ext, apps);
    }
}

void Choose(const std::string& rel) {
    std::string ext = ExtOf(rel);
    auto apps = NomiPkg::GetInstance().AppsFor(ext);
    if (apps.empty()) {
        NomiSheet::Close();
        nomi_ui_notify(NoApp(ext).c_str(), 3000);
        return;
    }
    ShowChooser(rel, ext, apps);
}

std::string DefaultsOf(const std::string& app_id) {
    std::string out;
    std::lock_guard<std::mutex> lock(s_mutex);
    for (const auto& [ext, id] : s_defaults) {
        if (id == app_id) out += (out.empty() ? "." : " .") + ext;
    }
    return out;
}

void ClearDefaults(const std::string& app_id) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto it = s_defaults.begin(); it != s_defaults.end();) {
            if (it->second == app_id) {
                it = s_defaults.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
    }
    if (changed) Save();
}

std::string OpenJson(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    cJSON* path = cJSON_GetObjectItem(root, "path");
    std::string rel = cJSON_IsString(path) ? path->valuestring : "";
    cJSON_Delete(root);
    std::string full;
    std::string problem = NomiFiles::Resolve(rel, false, full);
    if (!problem.empty()) return Json(false, "error", problem);
    {
        NomiSdCard::Use sd;
        if (!sd) return Json(false, "error", "SD 卡没插");
        struct stat st;
        if (stat(full.c_str(), &st) != 0) return Json(false, "error", "卡上没有这个文件");
        if (S_ISDIR(st.st_mode)) return Json(false, "error", "这是文件夹");
    }
    std::string ext = ExtOf(rel);
    auto apps = NomiPkg::GetInstance().AppsFor(ext);
    if (apps.empty()) return Json(false, "error", NoApp(ext));
    if (NomiDisplay::on_user_activity) NomiDisplay::on_user_activity();  // 睡着、息屏了先叫醒
    std::string def = DefaultFor(ext);
    const NomiPackage* chosen = nullptr;
    for (const NomiPackage* pkg : apps) {
        if (pkg->id == def || apps.size() == 1) chosen = pkg;
    }
    {
        DisplayLockGuard ui(NomiWasm::GetDisplay());
        Open(rel);
    }
    return chosen ? Json(true, "app", chosen->name) : Json(true, "choose", "设备上弹出来了，选一下用哪个应用");
}

}  // namespace NomiOpen
