#include "nomi_files.h"

#include "nomi_sdcard.h"

#include <cJSON.h>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#define TAG "NomiFiles"

namespace {

constexpr size_t kMaxEntries = 1000;  // 一个文件夹最多列多少个（再多网页、屏幕上也翻不过来）
std::atomic<uint32_t> s_changes{0};

std::string Field(cJSON* root, const char* key) {
    cJSON* v = cJSON_GetObjectItem(root, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

std::string Print(cJSON* root) {
    char* json = cJSON_PrintUnformatted(root);
    std::string out = json ? json : "{}";
    cJSON_free(json);
    cJSON_Delete(root);
    return out;
}

std::string Ok() { return "{\"ok\":true}"; }

void RemoveTree(const std::string& path, int& removed) {
    if (DIR* d = opendir(path.c_str())) {
        while (dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            std::string child = path + "/" + e->d_name;
            if (e->d_type == DT_DIR) {
                RemoveTree(child, removed);
            } else if (unlink(child.c_str()) == 0) {
                removed++;
            }
        }
        closedir(d);
    }
    rmdir(path.c_str());
}

const char* TypeOf(const std::string& name) {
    static const struct {
        const char *ext, *type;
    } kTypes[] = {{".jpg", "image/jpeg"},  {".jpeg", "image/jpeg"}, {".png", "image/png"},
                  {".txt", "text/plain"},  {".mp3", "audio/mpeg"},  {".flac", "audio/flac"},
                  {".m4a", "audio/mp4"},   {".wav", "audio/wav"},   {".json", "application/json"}};
    size_t dot = name.rfind('.');
    if (dot != std::string::npos) {
        for (const auto& t : kTypes) {
            if (!strcasecmp(name.c_str() + dot, t.ext)) return t.type;
        }
    }
    return "application/octet-stream";
}

}  // namespace

namespace NomiFiles {

std::string UrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool Hidden(const std::string& name, bool at_root) {
    return name.empty() || name[0] == '.' || !strcasecmp(name.c_str(), "System Volume Information") ||
           !strcasecmp(name.c_str(), "$RECYCLE.BIN") || (at_root && !strcasecmp(name.c_str(), "nomi"));
}

std::string Resolve(const std::string& rel, bool write, std::string& full) {
    std::string r = rel;
    while (!r.empty() && r[0] == '/') r.erase(0, 1);
    while (!r.empty() && r.back() == '/') r.pop_back();
    if (r.find("..") != std::string::npos || r.find('\\') != std::string::npos || r.find("//") != std::string::npos) {
        return "路径不对";
    }
    std::string first = r.substr(0, r.find('/'));
    if (!r.empty() && !strcasecmp(first.c_str(), "nomi")) return "nomi 是系统自己用的文件夹，不能动";
    if (write && r.empty()) return "不能动卡的根";
    full = r.empty() ? "/sd" : "/sd/" + r;
    return "";
}

std::string List(const std::string& rel, std::vector<Entry>& out, bool& truncated) {
    out.clear();
    truncated = false;
    std::string full, err = Resolve(rel, false, full);
    if (!err.empty()) return err;
    DIR* d = opendir(full.c_str());
    if (!d) return "没有这个文件夹";
    bool root = full == "/sd";
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || Hidden(e->d_name, root)) continue;
        if (out.size() >= kMaxEntries) {
            truncated = true;
            break;
        }
        Entry en;
        en.name = e->d_name;
        en.dir = e->d_type == DT_DIR;
        struct stat st;
        if (stat((full + "/" + en.name).c_str(), &st) == 0) {
            en.size = en.dir ? 0 : st.st_size;
            en.mtime = st.st_mtime;
        }
        out.push_back(std::move(en));
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return "";
}

std::string Mkdir(const std::string& rel) {
    std::string full, err = Resolve(rel, true, full);
    if (!err.empty()) return err;
    struct stat st;
    if (stat(full.c_str(), &st) == 0) return "已经有同名的了";
    if (mkdir(full.c_str(), 0775) != 0) return "建不了（上一级文件夹不在？卡满了？）";
    Changed();
    return "";
}

std::string Rename(const std::string& from, const std::string& to) {
    std::string a, b, err = Resolve(from, true, a);
    if (err.empty()) err = Resolve(to, true, b);
    if (!err.empty()) return err;
    struct stat st;
    if (stat(a.c_str(), &st) != 0) return "没有这个文件";
    if (stat(b.c_str(), &st) == 0) return "已经有同名的了";
    if (rename(a.c_str(), b.c_str()) != 0) return "改不了（要挪去的文件夹不在？）";
    Changed();
    return "";
}

std::string Remove(const std::string& rel, int* removed) {
    std::string full, err = Resolve(rel, true, full);
    if (!err.empty()) return err;
    struct stat st;
    if (stat(full.c_str(), &st) != 0) return "没有这个文件";
    int n = 0;
    if (S_ISDIR(st.st_mode)) {
        RemoveTree(full, n);
        if (stat(full.c_str(), &st) == 0) err = "没删干净（有文件正被打开着？）";
    } else if (unlink(full.c_str()) == 0) {
        n = 1;
    } else {
        err = "删不掉";
    }
    if (removed) *removed = n;
    Changed();
    return err;
}

void Changed() { s_changes++; }
uint32_t ChangeCount() { return s_changes; }

/* ---------- 网页 ---------- */

std::string ListJson(const std::string& body) {
    cJSON* req = cJSON_Parse(body.c_str());
    std::string rel = Field(req, "path");
    cJSON_Delete(req);
    NomiSdCard::Use sd;
    if (!sd) return "!SD 卡没插";
    std::vector<Entry> entries;
    bool truncated;
    std::string err = List(rel, entries, truncated);
    if (!err.empty()) return "!" + err;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON* arr = cJSON_AddArrayToObject(root, "entries");
    for (const auto& e : entries) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", e.name.c_str());
        cJSON_AddBoolToObject(item, "dir", e.dir);
        cJSON_AddNumberToObject(item, "size", static_cast<double>(e.size));
        cJSON_AddNumberToObject(item, "time", static_cast<double>(e.mtime));
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddBoolToObject(root, "truncated", truncated);
    uint64_t total = 0, free_bytes = 0;
    esp_vfs_fat_info(NomiSdCard::kMountPoint, &total, &free_bytes);
    cJSON_AddNumberToObject(root, "total", static_cast<double>(total));
    cJSON_AddNumberToObject(root, "free", static_cast<double>(free_bytes));
    return Print(root);
}

std::string MkdirJson(const std::string& body) {
    cJSON* req = cJSON_Parse(body.c_str());
    std::string rel = Field(req, "path");
    cJSON_Delete(req);
    NomiSdCard::Use sd;
    if (!sd) return "!SD 卡没插";
    std::string err = Mkdir(rel);
    return err.empty() ? Ok() : "!" + err;
}

std::string RenameJson(const std::string& body) {
    cJSON* req = cJSON_Parse(body.c_str());
    std::string from = Field(req, "from"), to = Field(req, "to");
    cJSON_Delete(req);
    NomiSdCard::Use sd;
    if (!sd) return "!SD 卡没插";
    std::string err = Rename(from, to);
    return err.empty() ? Ok() : "!" + err;
}

std::string DeleteJson(const std::string& body) {
    cJSON* req = cJSON_Parse(body.c_str());
    std::string rel = Field(req, "path");
    cJSON_Delete(req);
    NomiSdCard::Use sd;
    if (!sd) return "!SD 卡没插";
    int removed = 0;
    std::string err = Remove(rel, &removed);
    ESP_LOGI(TAG, "Delete %s: %d file(s)%s%s", rel.c_str(), removed, err.empty() ? "" : ", ", err.c_str());
    if (!err.empty()) return "!" + err;
    return "{\"ok\":true,\"removed\":" + std::to_string(removed) + "}";
}

bool Download(const std::string& uri, const Begin& begin, const Write& write) {
    const std::string prefix = "/sd/file/";
    if (uri.rfind(prefix, 0) != 0) return false;
    std::string full, rel = UrlDecode(uri.substr(prefix.size()));
    if (!Resolve(rel, false, full).empty()) return false;
    NomiSdCard::Use sd;
    if (!sd) return false;
    struct stat st;
    if (stat(full.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return false;
    FILE* f = fopen(full.c_str(), "rb");
    if (!f) return false;
    std::string name = full.substr(full.rfind('/') + 1);
    begin(TypeOf(name), name);
    constexpr size_t kChunk = 8192;
    auto* buf = static_cast<char*>(heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM));
    size_t n;
    while (buf && (n = fread(buf, 1, kChunk, f)) > 0) {
        if (!write(buf, n)) break;
    }
    heap_caps_free(buf);
    fclose(f);
    return true;
}

}  // namespace NomiFiles
