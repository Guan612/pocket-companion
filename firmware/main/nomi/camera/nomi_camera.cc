#include "nomi_camera.h"

#include <esp_cache.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_jpeg_enc.h>
#include <esp_lcd_panel_interface.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>
#include <mutex>

#define TAG "NomiCamera"

// esp32-camera 内部的启停（driver/cam_hal.c，没进公开头文件）：只停 DMA 和场同步中断，
// 传感器、帧缓冲、采集任务都留着，再 cam_start() 从下一帧接着收
extern "C" {
void cam_stop(void);
void cam_start(void);
}

namespace NomiCamera {
namespace {

// 取景器、照片都按传感器原样（横的 640×464）：2026-11-05 先按房间照片猜着转成竖的，用户要取景器转回去、横着铺满屏。
// 要转的话 JPEG 编码器能顺手转（宽高是 16 的倍数时 cfg.rotate），取景器的取样要跟着改
constexpr int kSkipRows = 16;     // 顶上偶发颜色错的几行，丢掉一个 JPEG 块行（见头文件）
constexpr int kMaxFrames = 4;     // 一次最多收几帧（正常开起来两帧、开着时一帧）
constexpr size_t kMaxJpegBytes = 96 * 1024;  // 超了降质量重编：base64 后整个请求要在 PSRAM 里拼好几份
constexpr int kInFlightMs = 40;   // 关闸门后等在途的那次刷屏发完（80 行一块约 15ms）
constexpr int kPreviewGapMs = 80; // 预览两帧之间：让屏幕把这帧刷出去、界面喘口气
constexpr uint8_t kTailMark[16] = {0xA5, 0x5A, 0xC3, 0x3C, 0x96, 0x69, 0x0F, 0xF0,
                                   0x5A, 0xA5, 0x3C, 0xC3, 0x69, 0x96, 0xF0, 0x0F};

bool s_ready;
std::mutex s_mutex;       // 摄像头：同一时间只有一个人收帧
bool s_running;           // cam_start 了（s_mutex）
bool s_first;             // 开起来后的第一帧不信（s_mutex）
camera_fb_t* s_held;      // 拿着的那帧，拿着时 DMA 停着（s_mutex）

/* ---------- 刷屏闸门 ---------- */

esp_err_t (*s_draw)(esp_lcd_panel_t*, int, int, int, int, const void*);
EventGroupHandle_t s_gate;
constexpr EventBits_t kOpen = 1;

esp_err_t GatedDraw(esp_lcd_panel_t* panel, int x0, int y0, int x1, int y1, const void* data) {
    xEventGroupWaitBits(s_gate, kOpen, pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
    return s_draw(panel, x0, y0, x1, y1, data);
}

// 收帧的这一会儿别刷屏
struct FlushPause {
    FlushPause() {
        if (!s_gate) return;
        xEventGroupClearBits(s_gate, kOpen);
        vTaskDelay(pdMS_TO_TICKS(kInFlightMs));
    }
    ~FlushPause() {
        if (s_gate) xEventGroupSetBits(s_gate, kOpen);
    }
};

/* ---------- 收帧 ---------- */

bool TailMarked(const camera_fb_t* fb) {
    return memcmp(fb->buf + fb->len - sizeof(kTailMark), kTailMark, sizeof(kTailMark)) == 0;
}

// 还帧前打帧尾记号，并把 CPU 写过的地方（记号、原地换算的整帧）从缓存写回 PSRAM：
// 不写回的话，脏缓存行过后才被挤出去，会盖掉下一次 DMA 刚写进去的像素
void ReturnFrame(camera_fb_t* fb) {
    if (fb->len >= sizeof(kTailMark)) {
        memcpy(fb->buf + fb->len - sizeof(kTailMark), kTailMark, sizeof(kTailMark));
        esp_cache_msync(fb->buf, fb->len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
    esp_camera_fb_return(fb);
}

void StopLocked() {
    if (s_held) {
        ReturnFrame(s_held);
        s_held = nullptr;
    }
    if (s_running) {
        cam_stop();
        s_running = false;
    }
}

// 收一帧新的（s_mutex 里）：还掉拿着的那帧、等下一整帧，收的时候关着刷屏闸门。拿到的帧留在 s_held
camera_fb_t* NextFrameLocked(int* frames, int* short_frames) {
    if (!s_running) {
        cam_start();
        s_running = true;
        s_first = true;
    }
    FlushPause pause;
    if (s_held) {
        ReturnFrame(s_held);
        s_held = nullptr;
    }
    for (int i = 0; i < kMaxFrames; i++) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb == nullptr) break;
        ++*frames;
        if (!s_first && fb->len >= sizeof(kTailMark) && !TailMarked(fb)) {
            s_held = fb;
            return fb;
        }
        if (!s_first) ++*short_frames;
        s_first = false;
        ReturnFrame(fb);
    }
    // 收不到：停下来，下次重新开（DMA 可能正收到一半，开起来的第一帧会丢掉）
    StopLocked();
    return nullptr;
}

/* ---------- JPEG ---------- */

// 摄像头的 RGB565 是大端：两个像素（4 字节）一组换成 Y0 Cb Y1 Cr（也是 4 字节）写回原处（BT.601 全范围）
void RgbToYuyvInPlace(uint8_t* p, size_t pixels) {
    auto clamp = [](int v) { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
    for (size_t i = 0; i + 1 < pixels; i += 2, p += 4) {
        int r[2], g[2], b[2];
        for (int k = 0; k < 2; k++) {
            int v = (p[k * 2] << 8) | p[k * 2 + 1];
            int r5 = v >> 11, g6 = (v >> 5) & 0x3f, b5 = v & 0x1f;
            r[k] = (r5 << 3) | (r5 >> 2);
            g[k] = (g6 << 2) | (g6 >> 4);
            b[k] = (b5 << 3) | (b5 >> 2);
        }
        int rs = r[0] + r[1], gs = g[0] + g[1], bs = b[0] + b[1];  // 两个像素合用一份色度
        p[0] = clamp((77 * r[0] + 150 * g[0] + 29 * b[0]) >> 8);
        p[1] = clamp(((-43 * rs - 85 * gs + 128 * bs) >> 9) + 128);
        p[2] = clamp((77 * r[1] + 150 * g[1] + 29 * b[1]) >> 8);
        p[3] = clamp(((128 * rs - 107 * gs - 21 * bs) >> 9) + 128);
    }
}

std::string EncodeYuyv(const uint8_t* yuyv, int width, int height, int quality) {
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = width;
    cfg.height = height;
    cfg.src_type = JPEG_PIXEL_FORMAT_YCbYCr;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = quality;
    cfg.rotate = JPEG_ROTATE_0D;
    cfg.task_enable = false;
    jpeg_enc_handle_t enc = nullptr;
    if (jpeg_enc_open(&cfg, &enc) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg_enc_open failed");
        return "";
    }
    constexpr int kCap = 256 * 1024;  // VGA、质量 80 一般几十 KB
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(kCap, MALLOC_CAP_SPIRAM));
    std::string jpeg;
    int len = 0;
    jpeg_error_t err = buf ? jpeg_enc_process(enc, yuyv, width * height * 2, buf, kCap, &len) : JPEG_ERR_NO_MEM;
    if (err == JPEG_ERR_OK) jpeg.assign(reinterpret_cast<char*>(buf), len);
    else ESP_LOGW(TAG, "JPEG encode failed: %d", static_cast<int>(err));
    heap_caps_free(buf);
    jpeg_enc_close(enc);
    return jpeg;
}

/* ---------- 取景器 ---------- */

// 显示锁里读写
lv_obj_t* s_canvas;
uint16_t* s_pixels;  // PSRAM，s_view_w × s_view_h
int s_view_w, s_view_h;
std::atomic<bool> s_view_on{false};  // 预览任务看它；Capture 看它决定拍完关不关摄像头
SemaphoreHandle_t s_wake;
TaskHandle_t s_task;

// 一帧缩到取景器上：铺满（多出来的上下或左右裁掉，居中），最近邻取样，换成本机字节序的 RGB565。
// 顶上 kSkipRows 行不用（偶发颜色错）
void Downscale(const camera_fb_t* fb, uint16_t* out, int w, int h) {
    const uint8_t* src = fb->buf;
    int sw = fb->width, sh = fb->height - kSkipRows;
    // 铺满：取两个方向里缩得少的那个比例，用 定点 16.16 表示"目标一个像素 = 源几个像素"
    uint32_t step = sw * h > sh * w ? (static_cast<uint32_t>(sh) << 16) / h : (static_cast<uint32_t>(sw) << 16) / w;
    int x0 = (sw - static_cast<int>((step * w) >> 16)) / 2;
    int y0 = kSkipRows + (sh - static_cast<int>((step * h) >> 16)) / 2;
    for (int oy = 0; oy < h; oy++) {
        const uint8_t* row = src + static_cast<size_t>(y0 + ((oy * step) >> 16)) * sw * 2;
        uint16_t* dst = out + oy * w;
        uint32_t fx = static_cast<uint32_t>(x0) << 16;
        for (int ox = 0; ox < w; ox++, fx += step) {
            const uint8_t* p = row + (fx >> 16) * 2;
            dst[ox] = static_cast<uint16_t>((p[0] << 8) | p[1]);
        }
    }
}

void PreviewTask(void*) {
    for (;;) {
        if (!s_view_on) {
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                StopLocked();
            }
            xSemaphoreTake(s_wake, portMAX_DELAY);
            continue;
        }
        // 取景器被压在下面（应用开了别的页）、屏幕上看不见：摄像头先关着，隔一会儿再看
        bool visible = false;
        if (lvgl_port_lock(1000)) {
            visible = s_canvas && lv_obj_is_visible(s_canvas);
            lvgl_port_unlock();
        }
        if (!visible) {
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                StopLocked();
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            int frames = 0, short_frames = 0;
            camera_fb_t* fb = NextFrameLocked(&frames, &short_frames);
            if (fb && fb->format == PIXFORMAT_RGB565 && lvgl_port_lock(1000)) {
                if (s_canvas && fb->height > kSkipRows) {
                    Downscale(fb, s_pixels, s_view_w, s_view_h);
                    lv_obj_invalidate(s_canvas);
                }
                lvgl_port_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kPreviewGapMs));
    }
}

}  // namespace

std::function<bool(std::string jpeg, const std::string& question)> ask_handler;

void GatePanel(esp_lcd_panel_handle_t panel) {
    s_gate = xEventGroupCreate();
    xEventGroupSetBits(s_gate, kOpen);
    s_draw = panel->draw_bitmap;
    panel->draw_bitmap = GatedDraw;
}

void Init() {
    s_ready = true;
    s_wake = xSemaphoreCreateBinary();
}

bool Ready() { return s_ready; }

std::string Capture() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_ready) return "";
    int64_t t0 = esp_timer_get_time();
    int frames = 0, short_frames = 0;
    camera_fb_t* fb = NextFrameLocked(&frames, &short_frames);
    std::string out;
    if (fb == nullptr) {
        ESP_LOGW(TAG, "Photo failed: %d frame(s), %d short", frames, short_frames);
    } else if (fb->format != PIXFORMAT_RGB565 || fb->width % 16 != 0 || (fb->height - kSkipRows) % 16 != 0 ||
               (reinterpret_cast<uintptr_t>(fb->buf) & 15) != 0) {
        ESP_LOGW(TAG, "Camera frame not usable: format %d, %ux%u, buf %p", fb->format,
                 static_cast<unsigned>(fb->width), static_cast<unsigned>(fb->height), fb->buf);
    } else {
        uint8_t* top = fb->buf + static_cast<size_t>(fb->width) * 2 * kSkipRows;
        int height = fb->height - kSkipRows;
        RgbToYuyvInPlace(top, fb->width * height);
        int quality = 80;
        out = EncodeYuyv(top, fb->width, height, quality);
        if (out.size() > kMaxJpegBytes) {
            quality = 50;
            out = EncodeYuyv(top, fb->width, height, quality);
        }
        ESP_LOGI(TAG, "Photo %ux%d: %u bytes JPEG (q%d), %d frame(s), %d short, %d ms",
                 static_cast<unsigned>(fb->width), height,
                 static_cast<unsigned>(out.size()), quality, frames, short_frames,
                 static_cast<int>((esp_timer_get_time() - t0) / 1000));
    }
    // 这帧换算成 YUYV 了，取景器不能再拿它画；取景器没开就关掉摄像头
    if (s_held) {
        ReturnFrame(s_held);
        s_held = nullptr;
    }
    if (!s_view_on) StopLocked();
    return out;
}

lv_obj_t* CreateViewfinder(lv_obj_t* parent, lv_obj_t** side) {
    // 画面铺满整块屏（横的，和传感器、照片一个方向），按钮那一列浮在画面右边
    lv_obj_t* screen = lv_screen_active();
    const int kW = lv_obj_get_width(screen), kH = lv_obj_get_height(screen);
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    auto add_side = [box, side]() {
        *side = lv_obj_create(box);  // 后建的盖在画面上面
        lv_obj_remove_style_all(*side);
        lv_obj_set_size(*side, 120, LV_PCT(100));
        lv_obj_align(*side, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_flex_flow(*side, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(*side, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(*side, 14, 0);
        lv_obj_remove_flag(*side, LV_OBJ_FLAG_SCROLLABLE);
    };

    if (!s_ready || s_canvas) {  // 没有摄像头，或者已经有一个取景器开着（只支持一个）
        lv_obj_t* l = lv_label_create(box);
        lv_obj_set_style_text_color(l, lv_color_hex(0x8b90a0), 0);
        lv_label_set_text(l, s_ready ? "摄像头正忙" : "没有摄像头");
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        add_side();
        return box;
    }
    s_pixels = static_cast<uint16_t*>(heap_caps_calloc(kW * kH, sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    if (!s_pixels) {
        lv_obj_t* l = lv_label_create(box);
        lv_label_set_text(l, "内存不够，开不了取景器");
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
        add_side();
        return box;
    }
    s_view_w = kW;
    s_view_h = kH;
    s_canvas = lv_canvas_create(box);
    lv_canvas_set_buffer(s_canvas, s_pixels, kW, kH, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);
    add_side();
    lv_obj_add_event_cb(
        s_canvas,
        [](lv_event_t* e) {
            (void)e;
            s_view_on = false;  // 预览任务下一轮看到就关摄像头
            s_canvas = nullptr;
            heap_caps_free(s_pixels);  // 显示锁里：预览任务只在显示锁里写它
            s_pixels = nullptr;
        },
        LV_EVENT_DELETE, nullptr);
    if (!s_task) {
        // 栈放 PSRAM（内部 RAM 紧）：这个任务不碰内部闪存
        xTaskCreateWithCaps(PreviewTask, "cam_view", 6144, nullptr, 2, &s_task, MALLOC_CAP_SPIRAM);
    }
    s_view_on = true;
    xSemaphoreGive(s_wake);
    return box;
}

}  // namespace NomiCamera
