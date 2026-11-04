#include "nomi_font.h"

#include "nomi_sdcard.h"

#include <dirent.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <strings.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#define TAG "NomiFont"

namespace {

constexpr const char* kFontDir = "/sd/fonts";
constexpr int kTtfSize = 20;          // 和系统字一样大（资源分区里的是 20 像素的 Noto Sans SC）
constexpr size_t kGlyphCache = 256;   // 缓存多少个补出来的字（每个约 400 字节，在 PSRAM）

const lv_font_t* s_base = nullptr;
lv_font_t s_reading;
lv_font_t* s_ttf = nullptr;
int s_users = 0;
std::unique_ptr<NomiSdCard::Use> s_sd;  // 字体文件开着的时候卡一直挂着

std::string FindTtf() {
    DIR* d = opendir(kFontDir);
    if (!d) return "";
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (e->d_type != DT_DIR && n[0] != '.' && n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".ttf") == 0) {
            names.push_back(n);
        }
    }
    closedir(d);
    if (names.empty()) return "";
    std::sort(names.begin(), names.end());
    return std::string(kFontDir) + "/" + names[0];
}

}  // namespace

namespace NomiFont {

void SetBase(const lv_font_t* font) { s_base = font; }

const lv_font_t* Base() { return s_base ? s_base : LV_FONT_DEFAULT; }

const lv_font_t* AcquireReading() {
    if (!s_base) return LV_FONT_DEFAULT;
    if (s_users++ > 0) return &s_reading;
    s_reading = *s_base;
    s_reading.fallback = nullptr;
    s_sd = std::make_unique<NomiSdCard::Use>();
    std::string path = *s_sd ? FindTtf() : "";
    if (!path.empty()) {
        int64_t t0 = esp_timer_get_time();
        // "S:" 是 LVGL 的 stdio 文件驱动（sdkconfig 里 LV_FS_STDIO_LETTER = 'S'，路径前缀为空）
        s_ttf = lv_tiny_ttf_create_file_ex(("S:" + path).c_str(), kTtfSize, LV_FONT_KERNING_NONE, kGlyphCache);
        if (s_ttf) {
            // 行高对不上就按比例调字号，补出来的字和系统字一样大
            int base = s_base->line_height, mine = s_ttf->line_height;
            if (mine > 0 && (mine > base + 1 || mine < base - 1)) {
                lv_tiny_ttf_set_size(s_ttf, kTtfSize * base / mine);
            }
            s_reading.fallback = s_ttf;
            ESP_LOGI(TAG, "Fallback font %s (line height %d, system %d, %lld ms)", path.c_str(),
                     static_cast<int>(s_ttf->line_height), base, (esp_timer_get_time() - t0) / 1000);
        } else {
            ESP_LOGW(TAG, "Can't load %s (needs a static TrueType .ttf)", path.c_str());
        }
    }
    if (!s_ttf) s_sd.reset();  // 没用上卡上的字体：不占着卡
    return &s_reading;
}

void ReleaseReading() {
    if (s_users == 0 || --s_users > 0) return;
    s_reading.fallback = nullptr;
    if (s_ttf) {
        lv_tiny_ttf_destroy(s_ttf);
        s_ttf = nullptr;
    }
    s_sd.reset();
}

}  // namespace NomiFont
