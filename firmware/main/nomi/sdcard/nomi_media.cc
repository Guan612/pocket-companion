#include "nomi_media.h"

#include "nomi_files.h"
#include "nomi_sdcard.h"

#include <dirent.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#define TAG "NomiMedia"

namespace {

constexpr const char* kDir = "/sd/nomi/cache/media";
constexpr const char* kKeyFile = "/sd/nomi/cache/media/index.txt";
constexpr const char* kFiles[] = {"", "/sd/nomi/cache/media/audio.txt", "/sd/nomi/cache/media/book.txt",
                                  "/sd/nomi/cache/media/image.txt"};
constexpr int kMaxDepth = 8;
constexpr int64_t kMaxScanUs = 60 * 1000000LL;

// 这次开机核对过的（s_mutex）：卡、卡上的东西没变就直接用存着的
std::mutex s_mutex;
bool s_known = false;
uint32_t s_change = 0, s_epoch = 0, s_serial = 0;
uint64_t s_total = 0;
int32_t s_counts[4];

int KindOf(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot || name[0] == '.') return 0;  // . 开头的（macOS 的 ._ 文件）不要
    char ext[8];
    size_t n = strlen(dot + 1);
    if (n == 0 || n >= sizeof(ext)) return 0;
    for (size_t i = 0; i <= n; i++) {
        char c = dot[1 + i];
        ext[i] = c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
    }
    static const struct {
        const char* ext;
        int kind;
    } kTypes[] = {{"mp3", NomiMedia::kAudio}, {"flac", NomiMedia::kAudio}, {"wav", NomiMedia::kAudio},
                  {"aac", NomiMedia::kAudio}, {"m4a", NomiMedia::kAudio},  {"txt", NomiMedia::kBook},
                  {"jpg", NomiMedia::kImage}, {"jpeg", NomiMedia::kImage}, {"png", NomiMedia::kImage}};
    for (const auto& t : kTypes) {
        if (strcmp(ext, t.ext) == 0) return t.kind;
    }
    return 0;
}

struct Scan {
    FILE* out[4] = {};
    int32_t counts[4] = {};
    int64_t deadline = 0;
};

// 一层：先这一层的文件（按名字排），再一个个子文件夹。同一个相册、专辑的挨在一起
void Walk(Scan& scan, const std::string& dir, int depth) {
    if (depth > kMaxDepth || esp_timer_get_time() > scan.deadline) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    bool root = dir == "/sd";
    std::vector<std::string> files, subdirs;
    while (dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || NomiFiles::Hidden(e->d_name, root)) continue;
        if (e->d_type == DT_DIR) subdirs.push_back(e->d_name);
        else if (KindOf(e->d_name)) files.push_back(e->d_name);
    }
    closedir(d);
    auto by_name = [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; };
    std::sort(files.begin(), files.end(), by_name);
    std::sort(subdirs.begin(), subdirs.end(), by_name);
    for (const auto& name : files) {
        std::string path = dir + "/" + name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        int kind = KindOf(name.c_str());
        fprintf(scan.out[kind], "sd:/%s\t%lld\n", path.c_str() + 4, static_cast<long long>(st.st_size));
        scan.counts[kind]++;
    }
    for (const auto& sub : subdirs) Walk(scan, dir + "/" + sub, depth + 1);
}

bool Rescan() {
    int64_t t0 = esp_timer_get_time();
    mkdir("/sd/nomi", 0775);
    mkdir("/sd/nomi/cache", 0775);
    mkdir(kDir, 0775);
    Scan scan;
    scan.deadline = t0 + kMaxScanUs;
    bool ok = true;
    for (int k = 1; k <= 3; k++) {
        scan.out[k] = fopen(kFiles[k], "w");
        ok &= scan.out[k] != nullptr;
    }
    if (ok) Walk(scan, "/sd", 0);
    for (int k = 1; k <= 3; k++) {
        if (scan.out[k]) fclose(scan.out[k]);
    }
    if (!ok) {
        ESP_LOGW(TAG, "Can't write %s", kDir);
        return false;
    }
    uint64_t total = 0, free_bytes = 0;
    esp_vfs_fat_info(NomiSdCard::kMountPoint, &total, &free_bytes);
    uint32_t serial = NomiSdCard::GetInstance().Serial();
    if (FILE* f = fopen(kKeyFile, "w")) {
        fprintf(f, "1 %lu %llu %llu\n%ld %ld %ld\n", static_cast<unsigned long>(serial),
                static_cast<unsigned long long>(total), static_cast<unsigned long long>(free_bytes),
                static_cast<long>(scan.counts[1]), static_cast<long>(scan.counts[2]), static_cast<long>(scan.counts[3]));
        fclose(f);
    }
    std::copy(scan.counts, scan.counts + 4, s_counts);
    s_serial = serial;
    s_total = total;
    ESP_LOGI(TAG, "Scanned: %ld audio, %ld books, %ld images in %lld ms%s", static_cast<long>(scan.counts[1]),
             static_cast<long>(scan.counts[2]), static_cast<long>(scan.counts[3]), (esp_timer_get_time() - t0) / 1000,
             esp_timer_get_time() > scan.deadline ? " (stopped: too long)" : "");
    return true;
}

// 开机后第一次用：卡上存着的结果还算不算数（同一张卡、剩余空间没变 = 关机期间没在电脑上改过）
bool LoadSaved() {
    FILE* f = fopen(kKeyFile, "r");
    if (!f) return false;
    int version = 0;
    unsigned long serial = 0;
    unsigned long long total = 0, free_saved = 0;
    long counts[3] = {};
    int n = fscanf(f, "%d %lu %llu %llu %ld %ld %ld", &version, &serial, &total, &free_saved, &counts[0], &counts[1],
                   &counts[2]);
    fclose(f);
    if (n != 7 || version != 1) return false;
    uint64_t now_total = 0, now_free = 0;
    esp_vfs_fat_info(NomiSdCard::kMountPoint, &now_total, &now_free);
    if (serial != NomiSdCard::GetInstance().Serial() || total != now_total || free_saved != now_free) return false;
    for (int k = 1; k <= 3; k++) s_counts[k] = counts[k - 1];
    s_serial = serial;
    s_total = total;
    return true;
}

}  // namespace

namespace NomiMedia {

int32_t List(Kind kind, int32_t first, bool rescan, std::string& out, size_t cap) {
    out.clear();
    if (kind < kAudio || kind > kImage) return -1;
    std::lock_guard<std::mutex> lock(s_mutex);
    auto& card = NomiSdCard::GetInstance();
    uint32_t change = NomiFiles::ChangeCount(), epoch = card.Epoch();
    bool fresh = !rescan && s_known && s_change == change && s_epoch == epoch && s_serial == card.Serial();
    if (!fresh && !rescan && !s_known) fresh = LoadSaved();
    if (!fresh && !Rescan()) return -1;
    s_known = true;
    s_change = change;
    s_epoch = epoch;

    FILE* f = fopen(kFiles[kind], "r");
    if (!f) return -1;
    char line[1024];  // 中文长路径：255 个字的文件名 UTF-8 能到 700 多字节
    int32_t index = 0;
    while (fgets(line, sizeof(line), f)) {
        if (index++ < first) continue;
        size_t n = strlen(line);
        if (out.size() + n + 1 > cap) break;
        out.append(line, n);
    }
    fclose(f);
    return s_counts[kind];
}

}  // namespace NomiMedia
