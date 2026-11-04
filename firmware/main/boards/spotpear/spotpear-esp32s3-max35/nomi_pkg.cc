#include "nomi_pkg.h"

#include "assets.h"
#include "board.h"
#include "display/display.h"
#include "nomi_flash_io.h"
#include "nomi_open.h"
#include "nomi_sdcard.h"
#include "nomi_ui.h"
#include "nomi_wasm.h"

#include <cJSON.h>
#include <dirent.h>
#include <esp_vfs_fat.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#define TAG "NomiPkg"

namespace {

constexpr int kApi = 3;                       // 固件支持的接口版本（docs/app-sdk.md）：2 阅读器、fs_find；3 图片、写卡、打开文件
constexpr size_t kMaxPackage = 4 * 1024 * 1024;
constexpr size_t kMaxWasm = 1024 * 1024;
constexpr size_t kMaxManifest = 16 * 1024;
const char* const kPermissions[] = {"sd.read", "sd.write", "media", "assistant", "keep_awake", "net"};
// 预装的应用包：资源分区里的文件名（板子目录 preinstall/ 下，tools/app-sdk/build.py 打的）
const char* const kPreinstalled[] = {"nomi.music.npk", "nomi.novel.npk", "nomi.photo.npk"};

Display* display() { return NomiWasm::GetDisplay(); }  // 不在这调 Board::GetInstance()，见 nomi_wasm.h

bool MakeDirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); i++) {
        if (i == path.size() || path[i] == '/') {
            std::string part = path.substr(0, i);
            struct stat st;
            if (stat(part.c_str(), &st) != 0 && mkdir(part.c_str(), 0775) != 0) return false;
        }
    }
    return true;
}

void RemoveTree(const std::string& path) {
    if (DIR* d = opendir(path.c_str())) {
        while (dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            std::string child = path + "/" + e->d_name;
            if (e->d_type == DT_DIR) RemoveTree(child);
            else unlink(child.c_str());
        }
        closedir(d);
        rmdir(path.c_str());
    } else {
        unlink(path.c_str());
    }
}

uint64_t TreeSize(const std::string& path) {
    uint64_t total = 0;
    if (DIR* d = opendir(path.c_str())) {
        while (dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            std::string child = path + "/" + e->d_name;
            if (e->d_type == DT_DIR) {
                total += TreeSize(child);
            } else {
                struct stat st;
                if (stat(child.c_str(), &st) == 0) total += st.st_size;
            }
        }
        closedir(d);
    }
    return total;
}

bool ValidId(const std::string& id) {
    if (id.size() < 2 || id.size() > 48) return false;
    for (size_t i = 0; i < id.size(); i++) {
        char c = id[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (i > 0 && (c == '.' || c == '_' || c == '-'));
        if (!ok) return false;
    }
    return true;
}

bool ValidVersion(const std::string& v) {
    int dots = 0;
    bool digit = false;
    for (char c : v) {
        if (c == '.') {
            if (!digit) return false;
            dots++;
            digit = false;
        } else if (c >= '0' && c <= '9') {
            digit = true;
        } else {
            return false;
        }
    }
    return dots == 2 && digit;
}

// 包里只认这几样文件，路径里不许有 .. 和反斜杠
bool SafeName(const std::string& n) {
    if (n == "manifest.json" || n == "app.wasm") return true;
    return n.rfind("assets/", 0) == 0 && n.size() > 7 && n.size() < 160 && n.find("..") == std::string::npos &&
           n.find('\\') == std::string::npos && n.find("//") == std::string::npos;
}

uint64_t Octal(const char* p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n && p[i]; i++) {
        if (p[i] >= '0' && p[i] <= '7') v = v * 8 + (p[i] - '0');
    }
    return v;
}

// manifest.json → 包的信息；不合格返回原因
std::string ParseManifest(const std::string& text, NomiPackage& pkg) {
    cJSON* root = cJSON_Parse(text.c_str());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return "manifest.json 不是合法的 JSON";
    }
    auto str = [&](const char* key) {
        cJSON* v = cJSON_GetObjectItem(root, key);
        return std::string(cJSON_IsString(v) ? v->valuestring : "");
    };
    std::string problem;
    pkg.id = str("id");
    pkg.name = str("name");
    pkg.version = str("version");
    pkg.description = str("description");
    cJSON* api = cJSON_GetObjectItem(root, "api");
    cJSON* mem = cJSON_GetObjectItem(root, "memory_kb");
    if (!ValidId(pkg.id)) problem = "id 不对：只能用小写字母、数字、点、下划线、减号";
    else if (pkg.name.empty() || pkg.name.size() > 36) problem = "name 要填，最多十来个字";
    else if (!ValidVersion(pkg.version)) problem = "version 要写成 1.0.0 这样";
    else if (!cJSON_IsNumber(api) || api->valueint < 1) problem = "api 要填";
    else if (api->valueint > kApi) problem = "这个应用要更新的固件（接口版本 " + std::to_string(api->valueint) + "）";
    if (problem.empty()) {
        pkg.memory_kb = cJSON_IsNumber(mem) ? std::clamp(mem->valueint, 64, 1024) : 256;
        cJSON* perm = nullptr;
        cJSON_ArrayForEach(perm, cJSON_GetObjectItem(root, "permissions")) {
            if (!cJSON_IsString(perm)) continue;
            bool known = false;
            for (const char* k : kPermissions) known |= strcmp(k, perm->valuestring) == 0;
            if (!known) {
                problem = std::string("不认识的权限：") + perm->valuestring;
                break;
            }
            pkg.permissions.push_back(perm->valuestring);
        }
    }
    if (problem.empty()) {
        pkg.tile = cJSON_IsTrue(cJSON_GetObjectItem(root, "tile"));
        cJSON* cmd = nullptr;
        cJSON_ArrayForEach(cmd, cJSON_GetObjectItem(root, "voice")) {
            std::string name = cJSON_IsString(cJSON_GetObjectItem(cmd, "name")) ? cJSON_GetObjectItem(cmd, "name")->valuestring : "";
            cJSON* desc = cJSON_GetObjectItem(cmd, "description");
            bool name_ok = !name.empty() && name.size() <= 24 &&
                           std::all_of(name.begin(), name.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
            if (!pkg.Has("assistant")) problem = "有 voice 指令要申请 assistant 权限";
            else if (!name_ok) problem = "voice 指令的 name 只能用小写字母、数字、下划线（最多 24 个）";
            else if (!cJSON_IsString(desc) || !*desc->valuestring || strlen(desc->valuestring) > 300) problem = "voice 指令「" + name + "」要写 description（最多一百来字）";
            else if (pkg.voice.size() >= 8) problem = "voice 指令最多 8 个";
            if (!problem.empty()) break;
            pkg.voice.push_back({name, desc->valuestring});
        }
    }
    if (problem.empty()) {
        // 能打开的文件类型：小写字母数字的扩展名，不带点
        cJSON* ext = nullptr;
        cJSON_ArrayForEach(ext, cJSON_GetObjectItem(root, "opens")) {
            std::string e = cJSON_IsString(ext) ? ext->valuestring : "";
            bool ok = !e.empty() && e.size() <= 8 &&
                      std::all_of(e.begin(), e.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
            if (!ok || pkg.opens.size() >= 16) {
                problem = "opens 里写小写的扩展名、不带点（比如 \"txt\"），最多 16 个";
                break;
            }
            if (std::find(pkg.opens.begin(), pkg.opens.end(), e) == pkg.opens.end()) pkg.opens.push_back(e);
        }
        if (problem.empty() && !pkg.opens.empty() && !pkg.Has("sd.read") && !pkg.Has("sd.write"))
            problem = "能打开卡上的文件（opens）要申请 sd.read 权限";
    }
    if (problem.empty()) {
        cJSON* icon = cJSON_GetObjectItem(root, "icon");
        cJSON* line = nullptr;
        cJSON_ArrayForEach(line, cJSON_GetObjectItem(icon, "lines")) {
            std::vector<lv_point_precise_t> pts;
            cJSON* pt = nullptr;
            cJSON_ArrayForEach(pt, line) {
                if (cJSON_GetArraySize(pt) != 2 || pts.size() >= 64) continue;
                int x = std::clamp(cJSON_GetArrayItem(pt, 0)->valueint, 0, 36);
                int y = std::clamp(cJSON_GetArrayItem(pt, 1)->valueint, 0, 36);
                pts.push_back({x, y});
            }
            if (pts.size() >= 2 && pkg.icon_points.size() < 16) pkg.icon_points.push_back(std::move(pts));
        }
        cJSON* c = nullptr;
        cJSON_ArrayForEach(c, cJSON_GetObjectItem(icon, "circles")) {
            if (cJSON_GetArraySize(c) != 3 || pkg.icon_circles.size() >= 8) continue;
            pkg.icon_circles.push_back({static_cast<int16_t>(std::clamp(cJSON_GetArrayItem(c, 0)->valueint, 0, 36)),
                                        static_cast<int16_t>(std::clamp(cJSON_GetArrayItem(c, 1)->valueint, 0, 36)),
                                        static_cast<int16_t>(std::clamp(cJSON_GetArrayItem(c, 2)->valueint, 1, 36))});
        }
    }
    cJSON_Delete(root);
    return problem;
}

// 填好登记到桌面的那一项（指针都指向 pkg 自己的成员，pkg 不再搬家）
void FinishApp(NomiPackage& pkg) {
    for (auto& pts : pkg.icon_points) {
        pkg.icon_lines.push_back({pts.data(), static_cast<uint16_t>(pts.size())});
    }
    pkg.icon = {pkg.icon_lines.data(), static_cast<uint8_t>(pkg.icon_lines.size()), pkg.icon_circles.data(),
                static_cast<uint8_t>(pkg.icon_circles.size())};
    pkg.app = {};
    pkg.app.id = pkg.id.c_str();
    pkg.app.name = pkg.name.c_str();
    pkg.app.icon = &pkg.icon;
    pkg.app.open = NomiWasm::Open;
    pkg.app.close = NomiWasm::Close;
    pkg.app.back = NomiWasm::Back;
    pkg.app.user = &pkg;
    pkg.app.dimmed = pkg.absent;
    pkg.app.dimmed_hint = pkg.hint.c_str();
}

// 装在 SD 卡上、现在用不了的应用，点了弹什么
std::string SdHint(const NomiPackage& pkg, bool card_present) {
    return card_present ? "SD 卡上没有找到「" + pkg.name + "」，重新装一下" : "插上 SD 卡才能打开「" + pkg.name + "」";
}

std::string ReadSmallFile(const std::string& path, size_t max) {
    std::string text;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < max) text.append(buf, n);
        fclose(f);
    }
    return text;
}

std::string Error(const std::string& msg) { return "!" + msg; }

// "1.2.3" 比大小：a 新返回正数
int CompareVersion(const std::string& a, const std::string& b) {
    int x[3] = {0, 0, 0}, y[3] = {0, 0, 0};
    sscanf(a.c_str(), "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b.c_str(), "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++) {
        if (x[i] != y[i]) return x[i] - y[i];
    }
    return 0;
}

// 把 from 下的文件一个个拷到 to（跨文件系统：SD 卡 → 内部分区）
bool CopyTree(const std::string& from, const std::string& to) {
    DIR* d = opendir(from.c_str());
    if (!d) return false;
    MakeDirs(to);
    bool ok = true;
    char* buf = static_cast<char*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        std::string src = from + "/" + e->d_name, dst = to + "/" + e->d_name;
        if (e->d_type == DT_DIR) {
            ok &= CopyTree(src, dst);
            continue;
        }
        FILE* in = fopen(src.c_str(), "rb");
        FILE* out = in ? fopen(dst.c_str(), "wb") : nullptr;
        size_t n;
        while (in && out && buf && (n = fread(buf, 1, 4096, in)) > 0) ok &= fwrite(buf, 1, n, out) == n;
        ok &= in && out;
        if (in) fclose(in);
        if (out) fclose(out);
    }
    heap_caps_free(buf);
    closedir(d);
    return ok;
}

/* 应用管理（nomi_app.c）的钩子：只有应用包能删，编进固件的不能 */
bool CanUninstall(const nomi_app_t* app) {
    return app->open == NomiWasm::Open && !static_cast<const NomiPackage*>(app->user)->builtin;
}

void Uninstall(const nomi_app_t* app) {
    const auto* pkg = static_cast<const NomiPackage*>(app->user);
    // 灰着的（卡不在）直接说，别在界面任务里再去挂一次卡（没卡时挂载要等超时）
    std::string problem = pkg->absent && !pkg->internal
                              ? "SD 卡没插：「" + pkg->name + "」装在卡上，插上卡再删"
                              : NomiPkg::GetInstance().Remove(app->id);
    if (!problem.empty()) nomi_ui_notify(problem.c_str(), 3500);
}

void LauncherShown() { NomiPkg::GetInstance().CheckSd("", true); }

// 应用管理：这个应用是哪些文件的默认打开方式（nomi_open 记着），清掉
const char* Defaults(const nomi_app_t* app) {
    static std::string text;  // 应用管理马上拷进标签
    text = NomiOpen::DefaultsOf(app->id);
    if (text.empty()) return nullptr;
    text = "默认打开 " + text;
    return text.c_str();
}

void ClearDefaults(const nomi_app_t* app) { NomiOpen::ClearDefaults(app->id); }

void DimmedTap(const nomi_app_t* app) {
    if (app->open == NomiWasm::Open) NomiPkg::GetInstance().CheckSd(app->id);
}

const char* Summary() {
    static char text[200];
    snprintf(text, sizeof(text),
             "装应用：网页设置页 → 设备 → 应用，上传 .npk 文件（插着 SD 卡装卡上，不然装机器里）。已装 %d 个，"
             "预装的不能删。",
             NomiPkg::GetInstance().Count());
    return text;
}

}  // namespace

bool NomiPackage::Has(const char* permission) const {
    return std::find(permissions.begin(), permissions.end(), permission) != permissions.end();
}

NomiPkg& NomiPkg::GetInstance() {
    static NomiPkg instance;
    return instance;
}

int NomiPkg::Count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return packages_.size();
}

const NomiPackage* NomiPkg::Register(std::unique_ptr<NomiPackage> pkg) {
    FinishApp(*pkg);
    const NomiPackage* added = pkg.get();
    {
        DisplayLockGuard ui(display());
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = packages_.begin(); it != packages_.end(); ++it) {
            if ((*it)->id == pkg->id) {  // 同一个应用的新版本：先把旧的从桌面去掉（开着的话会关掉）
                nomi_apps_remove(&(*it)->app);
                retired_.push_back(std::move(*it));
                packages_.erase(it);
                break;
            }
        }
        nomi_apps_add(&pkg->app);
        packages_.push_back(std::move(pkg));
    }
    if (added->tile && !added->absent) NomiWasm::StartBackground(added);  // 首页卡片：开机、装好就画出来
    return added;
}

const NomiPackage* NomiPkg::Find(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : packages_) {
        if (p->id == id) return p.get();
    }
    return nullptr;
}

std::string NomiPkg::PromptContext() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    for (const auto& p : packages_) {
        for (const auto& v : p->voice) {
            out += "- app=\"" + p->id + "\" command=\"" + v.name + "\"（" + p->name + "）：" + v.description + "\n";
        }
    }
    if (out.empty()) return "";
    return "装好的应用能做的事（用 self.app.command 调，主人的话合适时优先用这些）：\n" + out;
}

std::vector<std::unique_ptr<NomiPackage>> NomiPkg::CollectDir(const std::string& base, bool internal) {
    std::vector<std::unique_ptr<NomiPackage>> found;
    DIR* d = opendir(base.c_str());
    if (!d) return found;
    std::vector<std::string> dirs;
    while (dirent* e = readdir(d)) {
        if (e->d_type == DT_DIR && e->d_name[0] != '.') dirs.push_back(e->d_name);
    }
    closedir(d);
    std::sort(dirs.begin(), dirs.end());
    for (const auto& name : dirs) {
        std::string dir = base + "/" + name;
        auto pkg = std::make_unique<NomiPackage>();
        std::string text = ReadSmallFile(dir + "/manifest.json", kMaxManifest);
        std::string problem = ParseManifest(text, *pkg);
        struct stat st;
        if (problem.empty() && stat((dir + "/app.wasm").c_str(), &st) != 0) problem = "没有 app.wasm";
        if (problem.empty() && pkg->id != name) problem = "目录名和 id 不一致";
        if (!problem.empty()) {
            ESP_LOGW(TAG, "Skip %s: %s", dir.c_str(), problem.c_str());
            continue;
        }
        pkg->dir = dir;
        pkg->internal = internal;
        if (!internal) pkg->manifest = std::move(text);
        for (const char* npk : kPreinstalled) {
            pkg->builtin |= internal && pkg->id + ".npk" == npk;
        }
        ESP_LOGI(TAG, "Installed (%s): %s %s", internal ? "internal" : "sd", pkg->id.c_str(), pkg->version.c_str());
        found.push_back(std::move(pkg));
    }
    return found;
}

void NomiPkg::Scan() {
    // 机器里的先登记：卡上有同一个的不再要
    std::vector<std::unique_ptr<NomiPackage>> found, cached;
    if (internal_ok_) {
        NomiFlashIo::Run([&] {
            found = CollectDir(kInternalAppsDir, true);
            cached = ReadSdCache();
        });
    }
    for (auto& p : found) Register(std::move(p));
    bool present;
    {
        NomiSdCard::Use sd;
        present = static_cast<bool>(sd);
        std::vector<std::unique_ptr<NomiPackage>> on_card;
        if (present) on_card = CollectDir(kAppsDir, false);
        for (auto& p : on_card) {
            if (Find(p->id)) {
                ESP_LOGW(TAG, "Skip %s on SD card: already installed in internal flash", p->id.c_str());
                continue;
            }
            CacheSd(*Register(std::move(p)));
        }
    }
    // 机器里记着、这次没登记上的卡上应用：灰着放在桌面上
    int dimmed = 0;
    for (auto& p : cached) {
        if (const NomiPackage* have = Find(p->id)) {
            if (have->internal) UncacheSd(p->id);  // 后来装进了机器里（预装）：卡上那份不算了
            continue;
        }
        p->absent = true;
        p->hint = SdHint(*p, present);
        Register(std::move(p));
        dimmed++;
    }
    if (!present) ESP_LOGI(TAG, "No SD card: %d app(s) on the card shown dimmed", dimmed);
    else if (dimmed) ESP_LOGW(TAG, "%d app(s) not found on this SD card, shown dimmed", dimmed);
}

void NomiPkg::CheckSd(const std::string& open_id, bool throttled) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (throttled) {
            bool any = std::any_of(packages_.begin(), packages_.end(), [](const auto& p) { return !p->internal; });
            if (!any || esp_timer_get_time() - last_sd_check_us_ < 3 * 1000000LL) return;
        }
        if (!open_id.empty()) sd_open_id_ = open_id;
        if (sd_check_queued_) return;  // 排着的那次会顺便打开 sd_open_id_
        sd_check_queued_ = true;
    }
    NomiWasm::RunInWorker([](void*) { NomiPkg::GetInstance().DoCheckSd(); }, nullptr);
}

void NomiPkg::DoCheckSd() {
    std::string open_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sd_check_queued_ = false;
        open_id.swap(sd_open_id_);
    }
    std::vector<std::unique_ptr<NomiPackage>> on_card;
    bool present;
    {
        NomiSdCard::Use sd;
        present = static_cast<bool>(sd);
        if (present) on_card = CollectDir(kAppsDir, false);
    }
    std::vector<const NomiPackage*> restored;
    {
        DisplayLockGuard ui(display());
        std::lock_guard<std::mutex> lock(mutex_);
        last_sd_check_us_ = esp_timer_get_time();
        bool changed = false;
        for (auto& p : packages_) {
            if (p->internal) continue;
            auto it = std::find_if(on_card.begin(), on_card.end(), [&](const auto& c) { return c->id == p->id; });
            if (it != on_card.end() && (*it)->manifest == p->manifest) {  // 卡上的就是登记着的这个
                if (p->absent) {
                    p->absent = false;
                    p->app.dimmed = false;
                    restored.push_back(p.get());
                    changed = true;
                }
                on_card.erase(it);
            } else if (it == on_card.end()) {  // 卡不在，或者卡上没有它：灰掉
                std::string hint = SdHint(*p, present);
                if (!p->absent || p->hint != hint) {
                    p->absent = true;
                    p->hint = hint;
                    p->app.dimmed = true;
                    p->app.dimmed_hint = p->hint.c_str();
                    changed = true;
                }
            }
            // 卡上是别的版本：留在 on_card 里，下面重新登记换掉它
        }
        if (changed) nomi_apps_refresh();
    }
    // 卡上新装的、换了版本的：登记（同一个 id 的旧的会被换下来），机器里记一份
    for (auto& p : on_card) {
        const NomiPackage* have = Find(p->id);
        if (have && have->internal) continue;  // 机器里装着同一个：用机器里的
        ESP_LOGI(TAG, "Found %s %s on SD card", p->id.c_str(), p->version.c_str());
        CacheSd(*Register(std::move(p)));
    }
    for (const NomiPackage* p : restored) {
        ESP_LOGI(TAG, "SD card back: %s", p->id.c_str());
        if (p->tile) NomiWasm::StartBackground(p);
    }
    if (!open_id.empty()) {
        const NomiPackage* p = Find(open_id);
        if (p && !p->absent) {
            DisplayLockGuard ui(display());
            nomi_app_open(open_id.c_str());
        }
    }
}

std::vector<std::unique_ptr<NomiPackage>> NomiPkg::ReadSdCache() {
    std::vector<std::unique_ptr<NomiPackage>> out;
    DIR* d = opendir(kSdCacheDir);
    if (!d) return out;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (e->d_type != DT_DIR && n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0) names.push_back(n);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (const auto& n : names) {
        auto pkg = std::make_unique<NomiPackage>();
        std::string text = ReadSmallFile(std::string(kSdCacheDir) + "/" + n, kMaxManifest);
        if (!ParseManifest(text, *pkg).empty() || pkg->id + ".json" != n) continue;
        pkg->dir = std::string(kAppsDir) + "/" + pkg->id;
        pkg->manifest = std::move(text);
        out.push_back(std::move(pkg));
    }
    return out;
}

void NomiPkg::CacheSd(const NomiPackage& pkg) {
    if (!internal_ok_ || pkg.internal || pkg.manifest.empty()) return;
    std::string path = std::string(kSdCacheDir) + "/" + pkg.id + ".json";
    NomiFlashIo::Run([&] {
        if (ReadSmallFile(path, kMaxManifest + 1) == pkg.manifest) return;
        MakeDirs(kSdCacheDir);
        if (FILE* f = fopen(path.c_str(), "wb")) {
            fwrite(pkg.manifest.data(), 1, pkg.manifest.size(), f);
            fclose(f);
        }
    });
}

void NomiPkg::UncacheSd(const std::string& id) {
    if (!internal_ok_) return;
    std::string path = std::string(kSdCacheDir) + "/" + id + ".json";
    NomiFlashIo::Run([&] { unlink(path.c_str()); });
}

// 内部应用分区：闪存里 2MB 的 FAT（带磨损均衡），第一次用时格式化。缓冲走 PSRAM（大于 1KB 的分配默认放 PSRAM）
void NomiPkg::MountInternal() {
    esp_vfs_fat_mount_config_t cfg = {};
    cfg.format_if_mount_failed = true;
    cfg.max_files = 4;
    cfg.allocation_unit_size = 4096;
    wl_handle_t wl = WL_INVALID_HANDLE;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(kInternalMount, "apps", &cfg, &wl);
    internal_ok_ = err == ESP_OK;
    if (!internal_ok_) {
        ESP_LOGE(TAG, "Internal app partition not mounted: %s", esp_err_to_name(err));
        return;
    }
    uint64_t total = 0, free_bytes = 0;
    esp_vfs_fat_info(kInternalMount, &total, &free_bytes);
    ESP_LOGI(TAG, "Internal apps: %llu KB free of %llu KB (mount %lld ms)", free_bytes / 1024, total / 1024,
             (esp_timer_get_time() - t0) / 1000);
}

// 预装：资源分区里的包比机器里装着的新（或者还没装）就装进去
void NomiPkg::Preinstall() {
    if (!internal_ok_) return;
    for (const char* npk : kPreinstalled) {
        void* ptr = nullptr;
        size_t size = 0;
        if (!Assets::GetInstance().GetAssetData(npk, ptr, size) || size < 1024) {
            ESP_LOGW(TAG, "Preinstalled %s not in assets", npk);
            continue;
        }
        const char* data = static_cast<const char*>(ptr);
        // 包里第一个文件是 manifest.json：先看版本
        NomiPackage incoming, current;
        std::string first(data, strnlen(data, 100));
        size_t msize = Octal(data + 124, 12);
        if (first != "manifest.json" || msize > kMaxManifest || 512 + msize > size ||
            !ParseManifest(std::string(data + 512, msize), incoming).empty()) {
            ESP_LOGW(TAG, "Preinstalled %s is broken", npk);
            continue;
        }
        std::string dir = std::string(kInternalAppsDir) + "/" + incoming.id;
        bool installed = ParseManifest(ReadSmallFile(dir + "/manifest.json", kMaxManifest), current).empty();
        if (installed && CompareVersion(current.version, incoming.version) >= 0) continue;
        size_t pos = 0;
        auto read = [&](char* buf, size_t n) -> int {
            n = std::min(n, size - pos);
            memcpy(buf, data + pos, n);
            pos += n;
            return static_cast<int>(n);
        };
        std::string result = Install(kInternalAppsDir, size, read, false);
        if (!result.empty() && result[0] == '!') {
            ESP_LOGE(TAG, "Preinstall %s failed: %s", npk, result.c_str() + 1);
            continue;
        }
        ESP_LOGI(TAG, "Preinstalled %s %s", incoming.id.c_str(), incoming.version.c_str());
        // SD 卡上装过同一个应用：把它的数据（订阅、进度这些）搬过来，删掉卡上那份
        NomiSdCard::Use sd;
        std::string old = std::string(kAppsDir) + "/" + incoming.id;
        struct stat st;
        if (sd && stat(old.c_str(), &st) == 0) {
            if (!installed && stat((old + "/data").c_str(), &st) == 0) CopyTree(old + "/data", dir + "/data");
            RemoveTree(old);
            ESP_LOGI(TAG, "Moved %s data from SD card", incoming.id.c_str());
        }
    }
}

void NomiPkg::Start() {
    {
        DisplayLockGuard ui(display());
        static const nomi_appmgr_hooks_t hooks = {CanUninstall, Uninstall,  Summary,      LauncherShown,
                                                  DimmedTap,    Defaults,   ClearDefaults};
        nomi_appmgr_set_hooks(&hooks);
    }
    NomiFlashIo::Start();
    // 在应用任务里做：挂内部应用分区、装预装应用、扫两个地方。开机先让界面、联网起来。
    // 应用任务的栈在 PSRAM：碰内部闪存的部分交给 NomiFlashIo
    NomiWasm::RunInWorker(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            auto& pkg = NomiPkg::GetInstance();
            NomiFlashIo::Run([&pkg] {
                pkg.MountInternal();
                pkg.Preinstall();
            });
            pkg.Scan();
        },
        nullptr);
}

std::string NomiPkg::InstallJson(size_t length, const std::function<int(char*, size_t)>& read) {
    return Install("", length, read, true);
}

std::string NomiPkg::Install(const std::string& forced_base, size_t length,
                             const std::function<int(char*, size_t)>& read, bool web) {
    if (length == 0 || length > kMaxPackage) return Error("应用包要小于 4MB");
    NomiSdCard::Use sd;  // 有卡就挂着；装在哪看了 manifest 再定

    size_t consumed = 0;
    auto read_full = [&](char* buf, size_t size) {
        size_t got = 0;
        while (got < size) {
            int n = read(buf + got, size - got);
            if (n <= 0) return false;
            got += n;
        }
        consumed += size;
        return true;
    };
    auto skip = [&](size_t size) {
        char tmp[512];
        while (size > 0) {
            size_t n = size < sizeof(tmp) ? size : sizeof(tmp);
            if (!read_full(tmp, n)) return false;
            size -= n;
        }
        return true;
    };

    std::string base, tmp_dir;  // 看了 manifest 再定：装过的装回原处
    auto fail = [&](const std::string& msg) {
        if (!tmp_dir.empty()) RemoveTree(tmp_dir);
        return Error(msg);
    };

    auto pkg = std::make_unique<NomiPackage>();
    bool have_manifest = false, have_wasm = false;
    auto* chunk = static_cast<char*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
    if (!chunk) return fail("内存不够");
    std::string problem;
    char header[512];
    // tar：每个文件一个 512 字节的头，后面是内容，补齐到 512 的倍数；两个全零的块结尾
    while (problem.empty() && consumed + 512 <= length) {
        if (!read_full(header, 512)) {
            problem = "上传断了";
            break;
        }
        if (std::all_of(header, header + 512, [](char c) { return c == 0; })) break;
        std::string name(header, strnlen(header, 100));
        std::string prefix(header + 345, strnlen(header + 345, 155));
        if (!prefix.empty()) name = prefix + "/" + name;
        if (name.rfind("./", 0) == 0) name = name.substr(2);
        uint64_t size = Octal(header + 124, 12);
        char type = header[156];
        size_t padded = (size + 511) / 512 * 512;
        if (type == '5' || (type != '0' && type != '\0')) {  // 目录、链接之类：跳过
            if (!skip(padded)) problem = "上传断了";
            continue;
        }
        if (!SafeName(name)) {
            problem = "包里有不认识的文件：" + name;
            break;
        }
        if (name == "manifest.json") {
            if (size > kMaxManifest) {
                problem = "manifest.json 太大";
                break;
            }
            std::string text(size, '\0');
            if (!read_full(text.data(), size) || !skip(padded - size)) {
                problem = "上传断了";
                break;
            }
            problem = ParseManifest(text, *pkg);
            if (!problem.empty()) break;
            pkg->manifest = text;
            base = forced_base;
            if (base.empty()) {
                const NomiPackage* old = Find(pkg->id);
                if (old) base = old->internal ? kInternalAppsDir : kAppsDir;
                else base = sd ? kAppsDir : internal_ok_ ? kInternalAppsDir : "";
                if (old && !old->internal && !sd) base.clear();
            }
            if (base.empty()) {
                problem = "没插 SD 卡（这个应用装在卡上），机器内部的应用分区也用不了";
                break;
            }
            tmp_dir = base + "/.installing";
            RemoveTree(tmp_dir);
            if (!MakeDirs(tmp_dir)) {
                problem = "建不了目录（存储是不是只读、满了）";
                break;
            }
            have_manifest = true;
            NomiWasm::Kill(pkg->id);  // 旧版本在跑的话先结束（后面还要传一会儿，够它收尾）
            if (FILE* f = fopen((tmp_dir + "/manifest.json").c_str(), "wb")) {
                fwrite(text.data(), 1, text.size(), f);
                fclose(f);
            }
            continue;
        }
        if (!have_manifest) {
            problem = "manifest.json 要放在包的最前面（用 tools/app-sdk/build.py 打的包就是）";
            break;
        }
        if (name == "app.wasm") {
            if (size > kMaxWasm) {
                problem = "app.wasm 超过 1MB";
                break;
            }
            have_wasm = true;
        }
        std::string path = tmp_dir + "/" + name;
        MakeDirs(path.substr(0, path.rfind('/')));
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) {
            problem = "写不了存储";
            break;
        }
        uint64_t left = size;
        while (left > 0 && problem.empty()) {
            size_t n = left < 4096 ? left : 4096;
            if (!read_full(chunk, n)) problem = "上传断了";
            else if (fwrite(chunk, 1, n, f) != n) problem = "写存储出错（满了？）";
            left -= n;
        }
        fclose(f);
        if (problem.empty() && !skip(padded - size)) problem = "上传断了";
    }
    heap_caps_free(chunk);
    if (consumed < length) skip(length - consumed);  // 把剩下的收完，浏览器才拿得到回复
    if (problem.empty() && (!have_manifest || !have_wasm)) problem = "包里要有 manifest.json 和 app.wasm";
    if (!problem.empty()) return fail(problem);

    // 换上去：旧版本的 data/（进度）搬过来，再删旧目录、改名
    std::string dir = base + "/" + pkg->id;
    struct stat st;
    bool updated = stat(dir.c_str(), &st) == 0;
    if (updated) {
        rename((dir + "/data").c_str(), (tmp_dir + "/data").c_str());
        RemoveTree(dir);
    }
    if (rename(tmp_dir.c_str(), dir.c_str()) != 0) return fail("装不上：改目录名失败");
    pkg->dir = dir;
    pkg->internal = base == kInternalAppsDir;
    if (pkg->internal) pkg->manifest.clear();  // 只有卡上的要在机器里记一份
    for (const char* npk : kPreinstalled) {
        pkg->builtin |= pkg->internal && pkg->id + ".npk" == npk;
    }
    std::string id = pkg->id, name = pkg->name, version = pkg->version;
    ESP_LOGI(TAG, "%s %s %s (%s)", updated ? "Updated" : "Installed", id.c_str(), version.c_str(),
             pkg->internal ? "internal" : "sd");
    bool internal = pkg->internal;
    if (web) {
        CacheSd(*Register(std::move(pkg)));
        DisplayLockGuard ui(display());
        std::string msg = (updated ? "已更新「" : "已安装「") + name + "」，在桌面上";
        nomi_ui_notify(msg.c_str(), 3000);
    }

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", true);
    cJSON_AddStringToObject(out, "id", id.c_str());
    cJSON_AddStringToObject(out, "name", name.c_str());
    cJSON_AddStringToObject(out, "version", version.c_str());
    cJSON_AddBoolToObject(out, "updated", updated);
    cJSON_AddStringToObject(out, "location", internal ? "internal" : "sd");
    char* json = cJSON_PrintUnformatted(out);
    std::string result = json ? json : "{}";
    cJSON_free(json);
    cJSON_Delete(out);
    return result;
}

std::string NomiPkg::ListJson() {
    cJSON* out = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(out, "apps");
    {
        NomiSdCard::Use sd;
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& p : packages_) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "id", p->id.c_str());
            cJSON_AddStringToObject(item, "name", p->name.c_str());
            cJSON_AddStringToObject(item, "version", p->version.c_str());
            cJSON_AddStringToObject(item, "description", p->description.c_str());
            cJSON_AddStringToObject(item, "location", p->internal ? "internal" : "sd");
            cJSON_AddBoolToObject(item, "builtin", p->builtin);
            bool readable = p->internal || sd;
            uint64_t total = 0, data = 0;
            if (readable) {
                NomiFlashIo::ForPath(p->dir, [&] {
                    total = TreeSize(p->dir);
                    data = TreeSize(p->dir + "/data");
                });
            }
            cJSON_AddNumberToObject(item, "size", static_cast<double>(total - data));
            cJSON_AddNumberToObject(item, "data", static_cast<double>(data));
            bool web = false;  // 应用自带网页设置页
            if (readable) {
                NomiFlashIo::ForPath(p->dir, [&] {
                    struct stat st;
                    web = stat((p->dir + "/assets/web/index.html").c_str(), &st) == 0;
                });
            }
            cJSON_AddBoolToObject(item, "web", web);
            cJSON* perms = cJSON_AddArrayToObject(item, "permissions");
            for (const auto& perm : p->permissions) cJSON_AddItemToArray(perms, cJSON_CreateString(perm.c_str()));
            cJSON* opens = cJSON_AddArrayToObject(item, "opens");
            for (const auto& e : p->opens) cJSON_AddItemToArray(opens, cJSON_CreateString(e.c_str()));
            cJSON_AddItemToArray(arr, item);
        }
    }
    cJSON_AddNumberToObject(out, "api", kApi);
    char* json = cJSON_PrintUnformatted(out);
    std::string result = json ? json : "{}";
    cJSON_free(json);
    cJSON_Delete(out);
    return result;
}

std::vector<const NomiPackage*> NomiPkg::AppsFor(const std::string& ext) {
    std::vector<const NomiPackage*> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& p : packages_) {
        if (std::find(p->opens.begin(), p->opens.end(), ext) != p->opens.end()) out.push_back(p.get());
    }
    return out;
}

std::string NomiPkg::Remove(const std::string& id) {
    const NomiPackage* pkg = Find(id);
    if (!pkg) return "没有装这个应用";
    if (pkg->builtin) return "预装应用不能删";
    // 装在卡上的：卡不在就删不掉卡上的文件，下次插卡又冒出来，所以先不删
    std::unique_ptr<NomiSdCard::Use> sd;
    if (!pkg->internal) {
        sd = std::make_unique<NomiSdCard::Use>();
        if (!*sd) return "SD 卡没插：「" + pkg->name + "」装在卡上，插上卡再删";
    }
    std::unique_ptr<NomiPackage> victim;
    {
        DisplayLockGuard ui(display());
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = packages_.begin(); it != packages_.end(); ++it) {
            if ((*it)->id != id || (*it)->builtin) continue;
            nomi_apps_remove(&(*it)->app);  // 开着的话先关掉
            victim = std::move(*it);
            packages_.erase(it);
            break;
        }
    }
    if (!victim) return "没有装这个应用";
    NomiWasm::Kill(id);
    NomiOpen::ClearDefaults(id);  // 记成默认打开方式的也忘掉
    if (victim->internal) {
        NomiFlashIo::Run([&] { RemoveTree(victim->dir); });
    } else {
        RemoveTree(victim->dir);
        UncacheSd(id);
    }
    ESP_LOGI(TAG, "Removed %s", id.c_str());
    std::lock_guard<std::mutex> lock(mutex_);
    retired_.push_back(std::move(victim));
    return "";
}

std::string NomiPkg::OpenJson(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    cJSON* id = cJSON_GetObjectItem(root, "id");
    std::string s = cJSON_IsString(id) ? id->valuestring : "";
    cJSON_Delete(root);
    DisplayLockGuard ui(display());
    if (!nomi_app_open(s.c_str())) return Error("打不开：没装这个应用，或者它现在不能用");
    return "{\"ok\":true}";
}

bool NomiPkg::WebRequest(const std::string& uri, bool post, const std::string& body, std::string& type,
                         std::string& out) {
    // /app/<id>/<rest>
    const std::string prefix = "/app/";
    if (uri.rfind(prefix, 0) != 0) return false;
    size_t slash = uri.find('/', prefix.size());
    std::string id = uri.substr(prefix.size(), slash == std::string::npos ? std::string::npos : slash - prefix.size());
    std::string rest = slash == std::string::npos ? "" : uri.substr(slash + 1);
    const NomiPackage* pkg = Find(id);
    if (!pkg) return false;
    if (post) {
        if (rest != "call") return false;
        type = "application/json";
        out = NomiWasm::WebCall(id, body);
        return true;
    }
    if (rest.empty()) rest = "index.html";
    if (rest.find("..") != std::string::npos || rest.find('\\') != std::string::npos) return false;
    NomiSdCard::Use sd;  // 装在 SD 卡上的才要
    if (!pkg->internal && !sd) return false;
    NomiFlashIo::ForPath(pkg->dir, [&] { out = ReadSmallFile(pkg->dir + "/assets/web/" + rest, 512 * 1024); });
    if (out.empty()) return false;
    static const struct {
        const char *ext, *type;
    } kTypes[] = {{".html", "text/html; charset=utf-8"}, {".js", "application/javascript"}, {".css", "text/css"},
                  {".json", "application/json"},        {".png", "image/png"},             {".jpg", "image/jpeg"},
                  {".svg", "image/svg+xml"}};
    type = "application/octet-stream";
    for (const auto& t : kTypes) {
        size_t n = strlen(t.ext);
        if (rest.size() > n && rest.compare(rest.size() - n, n, t.ext) == 0) type = t.type;
    }
    return true;
}

std::string NomiPkg::RemoveJson(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    cJSON* id = cJSON_GetObjectItem(root, "id");
    std::string s = cJSON_IsString(id) ? id->valuestring : "";
    cJSON_Delete(root);
    std::string problem = Remove(s);
    if (!problem.empty()) return Error(problem);
    return "{\"ok\":true}";
}
