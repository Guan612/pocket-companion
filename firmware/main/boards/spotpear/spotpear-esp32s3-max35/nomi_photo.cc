#include "nomi_photo.h"

#include "application.h"
#include "display/display.h"
#include "nomi_image.h"
#include "nomi_sdcard.h"
#include "nomi_style.h"
#include "nomi_ui.h"
#include "settings.h"

#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#define TAG "NomiPhoto"

namespace {

using Token = std::shared_ptr<std::atomic<bool>>;  // 控件还在、这次请求还要（false 了就不解、不交）
using Done = std::function<void(NomiImage::Image&& img, const std::string& error)>;

constexpr const char* kCacheDir = "/sd/nomi/cache/images";
constexpr int kScreenW = 480, kScreenH = 320;  // 全屏图（看图、相框、预读）的大小
constexpr const char* kFrameNs = "nomi_frame";
constexpr int kSwipeMin = 40, kTapMax = 16;

struct Job {
    std::string path;
    std::function<std::string()> pick;  // 路径在后台才定（相框：列文件夹、挑下一张）
    int w = 0, h = 0;
    NomiImage::Fit fit = NomiImage::kContain;
    NomiImage::Source source = NomiImage::kMain;
    bool cache = false;          // 读写卡上的缓存（按 w × h 认）：缩略图、全屏图
    int skip_w = 0, skip_h = 0;  // 预览：这么大的全屏图已经有缓存了，马上就能显示，不用预览
    Token token;
    Done done;  // 空 = 只是预热缓存（预读）
};

Display* s_display = nullptr;
std::mutex s_mutex;
std::deque<Job> s_urgent, s_thumbs;  // 全屏看图、相框先解
SemaphoreHandle_t s_wake = nullptr;

Token NewToken() { return std::make_shared<std::atomic<bool>>(true); }

void Request(Job job, bool urgent) {
    if (!s_wake) return;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        (urgent ? s_urgent : s_thumbs).push_back(std::move(job));
    }
    xSemaphoreGive(s_wake);
}

uint32_t Fnv(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}

// 缓存文件：按路径、文件大小、修改时间、尺寸认（换了图、换了尺寸都对不上）
std::string CachePath(const std::string& path, int w, int h) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return "";
    char name[48];
    snprintf(name, sizeof(name), "%08lx%08lx.565", static_cast<unsigned long>(Fnv(path)),
             static_cast<unsigned long>(Fnv(std::to_string(static_cast<long long>(st.st_size)) + "|" +
                                            std::to_string(static_cast<long long>(st.st_mtime)) + "|" +
                                            std::to_string(w) + "x" + std::to_string(h))));
    return std::string(kCacheDir) + "/" + name;
}

bool LoadCache(const std::string& file, int w, int h, NomiImage::Image& img) {
    FILE* f = fopen(file.c_str(), "rb");
    if (!f) return false;
    size_t n = static_cast<size_t>(w) * h;
    img.pixels = static_cast<uint16_t*>(heap_caps_malloc(n * 2, MALLOC_CAP_SPIRAM));
    bool ok = img.pixels && fread(img.pixels, 2, n, f) == n;
    fclose(f);
    if (!ok) img = NomiImage::Image();
    else img.w = w, img.h = h;
    return ok;
}

void SaveCache(const std::string& file, const NomiImage::Image& img) {
    for (size_t i = 1; i < file.size(); i++) {  // 建文件夹
        if (file[i] != '/') continue;
        std::string dir = file.substr(0, i);
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) mkdir(dir.c_str(), 0775);
    }
    if (FILE* f = fopen(file.c_str(), "wb")) {
        fwrite(img.pixels, 2, static_cast<size_t>(img.w) * img.h, f);
        fclose(f);
    }
}

void Worker(void*) {
    for (;;) {
        xSemaphoreTake(s_wake, portMAX_DELAY);
        for (;;) {
            Job job;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                std::deque<Job>& q = !s_urgent.empty() ? s_urgent : s_thumbs;
                if (q.empty()) break;
                job = std::move(q.front());
                q.pop_front();
            }
            if (job.token && !*job.token) continue;
            NomiImage::Image img;
            std::string error;
            {
                NomiSdCard::Use sd;  // 图都在卡上；挂着期间连着解几张不用反复挂
                std::string path = job.pick ? job.pick() : job.path;
                if (path.empty()) {
                    error = "没有图";
                } else if (!sd && path.rfind("/sd/", 0) == 0) {
                    error = "SD 卡没插";
                } else {
                    struct stat st;
                    if (job.skip_w && stat(CachePath(path, job.skip_w, job.skip_h).c_str(), &st) == 0) continue;
                    std::string cache = job.cache ? CachePath(path, job.w, job.h) : "";
                    if (!job.done && !cache.empty() && stat(cache.c_str(), &st) == 0) continue;  // 预读：已经有了
                    if (cache.empty() || !LoadCache(cache, job.w, job.h, img)) {
                        int64_t t0 = esp_timer_get_time();
                        if (NomiImage::Load(path, job.w, job.h, job.fit, job.source, img, error)) {
                            ESP_LOGI(TAG, "%s %dx%d%s in %lld ms", path.c_str(), job.w, job.h,
                                     job.done ? "" : " (preload)", (esp_timer_get_time() - t0) / 1000);
                            if (!cache.empty()) SaveCache(cache, img);
                        } else if (job.source != NomiImage::kThumbOnly) {
                            ESP_LOGW(TAG, "%s: %s", path.c_str(), error.c_str());
                        }
                    }
                }
            }
            if (!job.done || !s_display) continue;
            DisplayLockGuard lock(s_display);
            if (!job.token || *job.token) job.done(std::move(img), error);
        }
    }
}

void SetImage(lv_obj_t* image, lv_image_dsc_t& dsc, const NomiImage::Image& img) {
    memset(&dsc, 0, sizeof(dsc));
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc.header.w = img.w;
    dsc.header.h = img.h;
    dsc.header.stride = img.w * 2;
    dsc.data = reinterpret_cast<const uint8_t*>(img.pixels);
    dsc.data_size = static_cast<uint32_t>(img.w) * img.h * 2;
    lv_image_set_src(image, &dsc);
    lv_obj_invalidate(image);
}

/* ---------- 缩略图 ---------- */

struct Thumb {
    Token token = NewToken();
    NomiImage::Image img;
    lv_image_dsc_t dsc;
    lv_obj_t* image = nullptr;
};

/* ---------- 全屏看图 ---------- */

struct Viewer {
    Token alive = NewToken();
    Token pending;  // 正在等的那张（换图时作废上一张）
    NomiImage::Image img, preview;  // 清楚的、预览（EXIF 小图，半屏大小放大显示）
    lv_image_dsc_t dsc, pdsc;
    bool full = false;  // 清楚的已经显示了
    lv_obj_t *box = nullptr, *image = nullptr, *pimage = nullptr, *label = nullptr;
    int flags = NomiPhoto::kSwipeLr;
    int w = 480, h = 320;
    NomiPhoto::Callback cb;
    lv_point_t press = {0, 0};
    bool long_pressed = false;
};

std::vector<lv_obj_t*> s_viewers;

Viewer* GetViewer(lv_obj_t* obj) {
    return NomiPhoto::IsViewer(obj) ? static_cast<Viewer*>(lv_obj_get_user_data(obj)) : nullptr;
}

void ViewerEvent(lv_event_t* e) {
    auto* v = static_cast<Viewer*>(lv_event_get_user_data(e));
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        *v->alive = false;
        if (v->pending) *v->pending = false;
        s_viewers.erase(std::remove(s_viewers.begin(), s_viewers.end(), v->box), s_viewers.end());
        delete v;
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_LONG_PRESSED && code != LV_EVENT_RELEASED) return;
    lv_point_t p = v->press;
    if (lv_indev_t* indev = lv_indev_active()) lv_indev_get_point(indev, &p);
    int dx = p.x - v->press.x, dy = p.y - v->press.y;
    auto fire = [v](NomiPhoto::Event ev) {
        if (v->cb) v->cb(ev);
    };
    if (code == LV_EVENT_PRESSED) {
        v->press = p;
        v->long_pressed = false;
    } else if (code == LV_EVENT_LONG_PRESSED) {
        if (std::abs(dx) < kTapMax && std::abs(dy) < kTapMax) {
            v->long_pressed = true;
            fire(NomiPhoto::kMenu);
        }
    } else if (!v->long_pressed) {
        bool horizontal = std::abs(dx) > std::abs(dy);
        if ((v->flags & NomiPhoto::kSwipeLr) && horizontal && std::abs(dx) >= kSwipeMin) {
            fire(dx < 0 ? NomiPhoto::kNext : NomiPhoto::kPrev);
        } else if ((v->flags & NomiPhoto::kSwipeUd) && !horizontal && std::abs(dy) >= kSwipeMin) {
            fire(dy < 0 ? NomiPhoto::kNext : NomiPhoto::kPrev);
        } else if (std::abs(dx) < kTapMax && std::abs(dy) < kTapMax) {
            lv_area_t a;
            lv_obj_get_coords(v->box, &a);
            int32_t len = a.x2 - a.x1 + 1, at = p.x - a.x1;
            if (at >= len / 3 && at < len * 2 / 3) fire(NomiPhoto::kMenu);
            else if (v->flags & NomiPhoto::kTapLr) fire(at < len / 3 ? NomiPhoto::kPrev : NomiPhoto::kNext);
        }
    }
}

/* ---------- 电子相框 ---------- */

std::string s_frame_dir;
int s_frame_sec = 60;

struct Frame {
    Token token = NewToken();
    NomiImage::Image img;
    lv_image_dsc_t dsc;
    lv_obj_t *box = nullptr, *image = nullptr;
    lv_timer_t* timer = nullptr;
    std::function<void()> on_touch;
    // 后台任务里用：文件夹里的图（打乱）、放到第几张
    std::mutex mutex;
    std::vector<std::string> files;
    size_t next = 0;
    bool listed = false;
    std::string dir;
};
std::shared_ptr<Frame> s_frame;  // 后台任务里的请求也拿着它：放掉时等最后一个请求做完才真删

bool IsImageName(const char* name) {
    const char* dot = strrchr(name, '.');
    return name[0] != '.' && dot &&
           (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png"));
}

// 后台任务里调：第一次先列文件夹、打乱，之后一张张往下
std::string FramePick(const std::shared_ptr<Frame>& f) {
    std::lock_guard<std::mutex> lock(f->mutex);
    if (!f->listed) {
        f->listed = true;
        if (DIR* d = opendir(f->dir.c_str())) {
            while (dirent* e = readdir(d)) {
                if (e->d_type != DT_DIR && IsImageName(e->d_name)) f->files.push_back(f->dir + "/" + e->d_name);
            }
            closedir(d);
        }
        for (size_t i = f->files.size(); i > 1; i--) std::swap(f->files[i - 1], f->files[esp_random() % i]);
        ESP_LOGI(TAG, "Frame %s: %u pictures", f->dir.c_str(), static_cast<unsigned>(f->files.size()));
    }
    if (f->files.empty()) return "";
    std::string path = f->files[f->next % f->files.size()];
    f->next++;
    return path;
}

void FrameNext() {
    std::shared_ptr<Frame> f = s_frame;
    if (!f) return;
    Job job;
    job.pick = [f]() { return FramePick(f); };
    job.w = kScreenW;
    job.h = kScreenH;
    job.fit = NomiImage::kContain;
    job.cache = true;  // 和看图共用全屏缓存：转过一圈以后都是直接读
    job.token = f->token;
    job.done = [f](NomiImage::Image&& img, const std::string& error) {
        if (!img.pixels) {
            ESP_LOGW(TAG, "Frame: %s", error.c_str());
            if (f->img.pixels == nullptr) NomiPhoto::FrameStop();  // 一张都放不出来：露出下面的时钟
            return;
        }
        lv_image_set_src(f->image, nullptr);
        f->img = std::move(img);
        SetImage(f->image, f->dsc, f->img);
    };
    Request(std::move(job), true);
}

}  // namespace

namespace NomiPhoto {

void Start(Display* display) {
    s_display = display;
    {
        Settings s(kFrameNs, false);
        s_frame_dir = s.GetString("dir");
        s_frame_sec = s.GetInt("sec", 60);
    }
    if (s_frame_sec < 10) s_frame_sec = 60;
    s_wake = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCoreWithCaps(Worker, "photo", 8192, nullptr, 1, nullptr, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
}

lv_obj_t* CreateThumb(lv_obj_t* parent, const std::string& path, int size) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, size, size);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 10, 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    auto* t = new Thumb();
    t->image = lv_image_create(box);
    lv_obj_center(t->image);
    lv_obj_add_event_cb(
        box,
        [](lv_event_t* e) {
            auto* t = static_cast<Thumb*>(lv_event_get_user_data(e));
            *t->token = false;
            delete t;
        },
        LV_EVENT_DELETE, t);
    Job job;
    job.path = path;
    job.w = job.h = size;
    job.fit = NomiImage::kCover;
    job.source = NomiImage::kThumbFirst;
    job.cache = true;
    job.token = t->token;
    job.done = [t](NomiImage::Image&& img, const std::string&) {
        if (!img.pixels) return;  // 解不开的留个灰块
        t->img = std::move(img);
        SetImage(t->image, t->dsc, t->img);
    };
    Request(std::move(job), false);
    return box;
}

lv_obj_t* CreateViewer(lv_obj_t* parent, const std::string& path, int flags, Callback cb) {
    auto* v = new Viewer();
    v->flags = flags & (kSwipeLr | kSwipeUd | kTapLr) ? flags & (kSwipeLr | kSwipeUd | kTapLr) : kSwipeLr;
    v->cb = std::move(cb);
    lv_obj_update_layout(parent);
    int32_t w = lv_obj_get_content_width(parent), h = lv_obj_get_content_height(parent);
    if (w < 200 || h < 120) w = 480, h = 320;
    v->w = w;
    v->h = h;
    lv_obj_t* box = v->box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_add_flag(box, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(box, 0, 0);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    v->pimage = lv_image_create(box);  // 预览垫在下面：拉伸铺满
    lv_obj_set_size(v->pimage, w, h);
    lv_image_set_inner_align(v->pimage, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_center(v->pimage);
    lv_obj_add_flag(v->pimage, LV_OBJ_FLAG_HIDDEN);
    v->image = lv_image_create(box);
    lv_obj_center(v->image);
    v->label = lv_label_create(box);  // 「加载中」压在底下，不挡预览
    lv_obj_set_style_text_color(v->label, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_bg_color(v->label, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(v->label, LV_OPA_80, 0);
    lv_obj_set_style_radius(v->label, 12, 0);
    lv_obj_set_style_pad_hor(v->label, 12, 0);
    lv_obj_set_style_pad_ver(v->label, 4, 0);
    lv_obj_align(v->label, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_user_data(box, v);
    lv_obj_add_event_cb(box, ViewerEvent, LV_EVENT_ALL, v);
    s_viewers.push_back(box);
    ViewerShow(box, path);
    return box;
}

bool IsViewer(lv_obj_t* obj) { return obj && std::find(s_viewers.begin(), s_viewers.end(), obj) != s_viewers.end(); }

void ViewerShow(lv_obj_t* viewer, const std::string& path) {
    Viewer* v = GetViewer(viewer);
    if (!v) return;
    if (v->pending) *v->pending = false;  // 上一张还没解完就不要了
    // 先放掉上一张（PSRAM 紧，不同时留两张全屏图）
    lv_image_set_src(v->image, nullptr);
    lv_image_set_src(v->pimage, nullptr);
    lv_obj_add_flag(v->pimage, LV_OBJ_FLAG_HIDDEN);
    v->img = NomiImage::Image();
    v->preview = NomiImage::Image();
    v->full = false;
    lv_label_set_text(v->label, "加载中…");
    lv_obj_remove_flag(v->label, LV_OBJ_FLAG_HIDDEN);
    v->pending = NewToken();
    // 预览：EXIF 小图解成半屏大小、拉伸铺满（全屏图有缓存就不用了）
    Job pv;
    pv.path = path;
    pv.w = v->w / 2;
    pv.h = v->h / 2;
    pv.source = NomiImage::kThumbOnly;
    pv.skip_w = v->w;
    pv.skip_h = v->h;
    pv.token = v->pending;
    pv.done = [v](NomiImage::Image&& img, const std::string&) {
        if (!*v->alive || v->full || !img.pixels) return;
        v->preview = std::move(img);
        SetImage(v->pimage, v->pdsc, v->preview);
        lv_obj_remove_flag(v->pimage, LV_OBJ_FLAG_HIDDEN);
    };
    Request(std::move(pv), true);
    Job job;
    job.path = path;
    job.w = v->w;
    job.h = v->h;
    job.fit = NomiImage::kContain;
    job.cache = true;
    job.token = v->pending;
    job.done = [v](NomiImage::Image&& img, const std::string& error) {
        if (!*v->alive) return;
        if (!img.pixels) {
            lv_label_set_text(v->label, error.empty() ? "打不开这张图" : error.c_str());
            return;
        }
        v->full = true;
        lv_obj_add_flag(v->label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v->pimage, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(v->pimage, nullptr);
        v->preview = NomiImage::Image();
        v->img = std::move(img);
        SetImage(v->image, v->dsc, v->img);
    };
    Request(std::move(job), true);
}

void Preload(const std::string& path) {
    Job job;
    job.path = path;
    job.w = kScreenW;
    job.h = kScreenH;
    job.cache = true;  // done 空：只存缓存
    Request(std::move(job), false);
}

void ViewerSetFlags(lv_obj_t* viewer, int flags) {
    if (Viewer* v = GetViewer(viewer)) {
        v->flags = flags & (kSwipeLr | kSwipeUd | kTapLr) ? flags & (kSwipeLr | kSwipeUd | kTapLr) : kSwipeLr;
    }
}

void SetFrame(const std::string& dir, int seconds) {
    s_frame_dir = dir;
    s_frame_sec = seconds >= 10 ? seconds : 60;
    int sec = s_frame_sec;
    // NVS 只能在栈在内部 RAM 的任务里写：排到主循环
    // 设了相框就把「睡着时显示」切成相册（不然设了也看不到）；不设了、而且正用着相册，切回时钟
    Application::GetInstance().Schedule([dir, sec]() {
        Settings s(kFrameNs, true);
        s.SetString("dir", dir);
        s.SetInt("sec", sec);
        Settings ui("nomi", true);  // 显示设置（nomi_display.cc 的 kNomiNs）
        if (!dir.empty()) ui.SetInt(NOMI_KEY_SLEEP_SHOW, 2);
        else if (ui.GetInt(NOMI_KEY_SLEEP_SHOW, 0) == 2) ui.SetInt(NOMI_KEY_SLEEP_SHOW, 0);
    });
}

bool FrameConfigured() { return !s_frame_dir.empty(); }

bool FrameStart(std::function<void()> on_touch) {
    if (s_frame || s_frame_dir.empty()) return s_frame != nullptr;
    s_frame = std::make_shared<Frame>();
    Frame* f = s_frame.get();
    f->dir = s_frame_dir;
    f->on_touch = std::move(on_touch);
    f->box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(f->box);
    lv_obj_set_size(f->box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(f->box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(f->box, LV_OPA_COVER, 0);
    lv_obj_add_flag(f->box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(f->box, LV_OBJ_FLAG_SCROLLABLE);
    f->image = lv_image_create(f->box);
    lv_obj_center(f->image);
    lv_obj_add_event_cb(
        f->box,
        [](lv_event_t* e) {
            auto* f = static_cast<Frame*>(lv_event_get_user_data(e));
            if (f->on_touch) f->on_touch();  // 摸一下就叫醒（叫醒时会 FrameStop）
        },
        LV_EVENT_PRESSED, f);
    f->timer = lv_timer_create([](lv_timer_t*) { FrameNext(); }, s_frame_sec * 1000, nullptr);
    FrameNext();
    return true;
}

void FrameStop() {
    std::shared_ptr<Frame> f = std::move(s_frame);
    if (!f) return;
    *f->token = false;  // 排着的不解了、解好的不交了
    lv_timer_delete(f->timer);
    // 可能正在它自己的触摸回调里（摸一下叫醒）：藏起来、断开图，下一轮再删
    lv_image_set_src(f->image, nullptr);
    lv_obj_add_flag(f->box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(f->box);
    f->image = nullptr;
}

bool FrameActive() { return s_frame != nullptr; }

bool PreviewBmp(const std::string& uri, std::string& type, std::string& out) {
    const std::string prefix = "/sd/preview/";
    if (uri.rfind(prefix, 0) != 0) return false;
    int w = 0, h = 0;
    size_t slash = uri.find('/', prefix.size());
    if (slash == std::string::npos || sscanf(uri.c_str() + prefix.size(), "%dx%d", &w, &h) != 2) return false;
    w = std::min(std::max(w, 8), 480);
    h = std::min(std::max(h, 8), 480);
    std::string rel;  // 百分号解码
    for (size_t i = slash + 1; i < uri.size(); i++) {
        if (uri[i] == '%' && i + 2 < uri.size()) {
            rel += static_cast<char>(strtol(uri.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            rel += uri[i];
        }
    }
    if (rel.empty() || rel.find("..") != std::string::npos) return false;
    NomiImage::Image img;
    std::string error;
    int64_t t0 = esp_timer_get_time();
    bool ok;
    {
        NomiSdCard::Use sd;
        ok = sd && NomiImage::Load("/sd/" + rel, w, h, NomiImage::kContain, NomiImage::kMain, img, error);
    }
    ESP_LOGI(TAG, "Preview %s %dx%d: %s in %lld ms", rel.c_str(), w, h, ok ? "ok" : error.c_str(),
             (esp_timer_get_time() - t0) / 1000);
    if (!ok) {
        type = "application/json";
        out = "!" + (error.empty() ? std::string("打不开") : error);
        return true;
    }
    // 24 位 BMP（自下而上一行行，每行补齐到 4 字节）
    size_t row = (static_cast<size_t>(w) * 3 + 3) & ~static_cast<size_t>(3), size = 54 + row * h;
    out.assign(size, '\0');
    auto put32 = [&](size_t at, uint32_t v) {
        for (int i = 0; i < 4; i++) out[at + i] = static_cast<char>(v >> (8 * i));
    };
    out[0] = 'B', out[1] = 'M';
    put32(2, size);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    out[26] = 1, out[28] = 24;
    put32(34, row * h);
    for (int y = 0; y < h; y++) {
        char* d = &out[54 + row * (h - 1 - y)];
        for (int x = 0; x < w; x++) {
            uint16_t c = img.pixels[y * w + x];
            d[x * 3] = static_cast<char>((c & 0x1F) << 3);
            d[x * 3 + 1] = static_cast<char>(((c >> 5) & 0x3F) << 2);
            d[x * 3 + 2] = static_cast<char>((c >> 11) << 3);
        }
    }
    type = "image/bmp";
    return true;
}

}  // namespace NomiPhoto
