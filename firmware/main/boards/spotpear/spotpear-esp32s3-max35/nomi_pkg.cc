#include "nomi_pkg.h"

#include "board.h"
#include "display/display.h"
#include "nomi_sdcard.h"
#include "nomi_ui.h"
#include "nomi_wasm.h"

#include <cJSON.h>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#define TAG "NomiPkg"

namespace {

constexpr int kApi = 1;                       // 固件支持的接口版本（docs/app-sdk.md）
constexpr size_t kMaxPackage = 4 * 1024 * 1024;
constexpr size_t kMaxWasm = 1024 * 1024;
constexpr size_t kMaxManifest = 16 * 1024;
const char* const kPermissions[] = {"sd.read", "media", "assistant", "keep_awake", "net"};

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

/* 应用管理（nomi_app.c）的钩子：只有应用包能删，编进固件的不能 */
bool CanUninstall(const nomi_app_t* app) { return app->open == NomiWasm::Open; }

void Uninstall(const nomi_app_t* app) {
    std::string id = app->id;
    NomiPkg::GetInstance().Remove(id);
}

const char* Summary() {
    static char text[200];
    snprintf(text, sizeof(text),
             "装应用：网页设置页 → 设备 → 应用，上传 .npk 文件。现在装在 SD 卡上（nomi/apps），已装 %d 个。",
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
    if (added->tile) NomiWasm::StartBackground(added);  // 首页卡片：开机、装好就画出来
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

void NomiPkg::Scan() {
    NomiSdCard::Use sd;
    if (!sd) {
        ESP_LOGI(TAG, "No SD card, no installed apps");
        return;
    }
    DIR* d = opendir(kAppsDir);
    if (!d) return;
    std::vector<std::string> dirs;
    while (dirent* e = readdir(d)) {
        if (e->d_type == DT_DIR && e->d_name[0] != '.') dirs.push_back(e->d_name);
    }
    closedir(d);
    std::sort(dirs.begin(), dirs.end());
    for (const auto& name : dirs) {
        std::string dir = std::string(kAppsDir) + "/" + name;
        auto pkg = std::make_unique<NomiPackage>();
        std::string problem = ParseManifest(ReadSmallFile(dir + "/manifest.json", kMaxManifest), *pkg);
        struct stat st;
        if (problem.empty() && stat((dir + "/app.wasm").c_str(), &st) != 0) problem = "没有 app.wasm";
        if (!problem.empty() || pkg->id != name) {
            ESP_LOGW(TAG, "Skip %s: %s", dir.c_str(), problem.empty() ? "目录名和 id 不一致" : problem.c_str());
            continue;
        }
        pkg->dir = dir;
        ESP_LOGI(TAG, "Installed: %s %s", pkg->id.c_str(), pkg->version.c_str());
        Register(std::move(pkg));
    }
}

void NomiPkg::Start() {
    {
        DisplayLockGuard ui(display());
        static const nomi_appmgr_hooks_t hooks = {CanUninstall, Uninstall, Summary};
        nomi_appmgr_set_hooks(&hooks);
    }
    // 在应用任务里扫：要读 SD 卡，开机先让界面、联网起来
    NomiWasm::RunInWorker(
        [](void*) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            NomiPkg::GetInstance().Scan();
        },
        nullptr);
}

std::string NomiPkg::InstallJson(size_t length, const std::function<int(char*, size_t)>& read) {
    if (length == 0 || length > kMaxPackage) return Error("应用包要小于 4MB");
    NomiSdCard::Use sd;
    if (!sd) return Error("没插 SD 卡：现在应用只能装在 SD 卡上");

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

    const std::string tmp_dir = std::string(kAppsDir) + "/.installing";
    RemoveTree(tmp_dir);
    if (!MakeDirs(tmp_dir)) return Error("SD 卡上建不了目录（卡是不是只读、满了）");
    auto fail = [&](const std::string& msg) {
        RemoveTree(tmp_dir);
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
            problem = "写不了 SD 卡";
            break;
        }
        uint64_t left = size;
        while (left > 0 && problem.empty()) {
            size_t n = left < 4096 ? left : 4096;
            if (!read_full(chunk, n)) problem = "上传断了";
            else if (fwrite(chunk, 1, n, f) != n) problem = "写 SD 卡出错（卡满了？）";
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
    std::string dir = std::string(kAppsDir) + "/" + pkg->id;
    struct stat st;
    bool updated = stat(dir.c_str(), &st) == 0;
    if (updated) {
        rename((dir + "/data").c_str(), (tmp_dir + "/data").c_str());
        RemoveTree(dir);
    }
    if (rename(tmp_dir.c_str(), dir.c_str()) != 0) return fail("装不上：改目录名失败");
    pkg->dir = dir;
    std::string id = pkg->id, name = pkg->name, version = pkg->version;
    ESP_LOGI(TAG, "%s %s %s", updated ? "Updated" : "Installed", id.c_str(), version.c_str());
    Register(std::move(pkg));
    {
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
            cJSON_AddStringToObject(item, "location", "sd");
            uint64_t total = sd ? TreeSize(p->dir) : 0, data = sd ? TreeSize(p->dir + "/data") : 0;
            cJSON_AddNumberToObject(item, "size", static_cast<double>(total - data));
            cJSON_AddNumberToObject(item, "data", static_cast<double>(data));
            struct stat st;  // 应用自带网页设置页
            cJSON_AddBoolToObject(item, "web", sd && stat((p->dir + "/assets/web/index.html").c_str(), &st) == 0);
            cJSON* perms = cJSON_AddArrayToObject(item, "permissions");
            for (const auto& perm : p->permissions) cJSON_AddItemToArray(perms, cJSON_CreateString(perm.c_str()));
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

bool NomiPkg::Remove(const std::string& id) {
    std::unique_ptr<NomiPackage> victim;
    {
        DisplayLockGuard ui(display());
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = packages_.begin(); it != packages_.end(); ++it) {
            if ((*it)->id != id) continue;
            nomi_apps_remove(&(*it)->app);  // 开着的话先关掉
            victim = std::move(*it);
            packages_.erase(it);
            break;
        }
    }
    if (!victim) return false;
    NomiWasm::Kill(id);
    {
        NomiSdCard::Use sd;
        if (sd) RemoveTree(victim->dir);
    }
    ESP_LOGI(TAG, "Removed %s", id.c_str());
    std::lock_guard<std::mutex> lock(mutex_);
    retired_.push_back(std::move(victim));
    return true;
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
    NomiSdCard::Use sd;
    if (!sd) return false;
    out = ReadSmallFile(pkg->dir + "/assets/web/" + rest, 512 * 1024);
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
    if (!Remove(s)) return Error("没有装这个应用");
    return "{\"ok\":true}";
}
