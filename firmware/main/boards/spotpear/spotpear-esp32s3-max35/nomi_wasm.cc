#include "nomi_wasm.h"

#include "board.h"
#include "display/display.h"
#include "nomi_pkg.h"
#include "nomi_sdcard.h"
#include "nomi_style.h"
#include "nomi_ui.h"

#include <cJSON.h>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <wasm_export.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#define TAG "NomiWasm"

namespace {

constexpr uint32_t kWasmStack = 16 * 1024;  // 解释器给应用开的栈（运行时从 PSRAM 分）
constexpr uint32_t kTaskStack = 32 * 1024;  // 常驻任务的栈（PSRAM）
constexpr int kMaxRead = 32 * 1024;         // 一次读文件最多多少
constexpr size_t kMaxWasm = 1024 * 1024;

// 和 tools/app-sdk/nomi_sdk.h 一致
enum { EV_CLICK = 1, EV_CHANGE, EV_BACK, EV_PAUSE, EV_RESUME, EV_CLOSE, EV_TIMER };
enum { TEXT_TITLE = 0, TEXT_BODY, TEXT_MUTED };
constexpr int32_t E_FAIL = -1, E_DENIED = -2, E_NO_SD = -3, E_NOT_FOUND = -4;

struct Event {
    int32_t type, id, value;
};

struct AppRun;
struct TimerSlot {
    AppRun* run;
    int32_t id;
    esp_timer_handle_t handle;
};

// 开着的一个应用
struct AppRun {
    const NomiPackage* pkg = nullptr;
    lv_obj_t* root = nullptr;  // 应用关了就置空（界面由应用层删）
    lv_obj_t* loading = nullptr;
    std::vector<lv_obj_t*> objs;   // 控件 id - 1 → 控件（删掉的为空）
    std::vector<lv_obj_t*> pages;  // 页面栈
    QueueHandle_t queue = nullptr;
    std::atomic<bool> closed{false};
    cJSON* kv = nullptr;
    std::map<int32_t, TimerSlot*> timers;
};

struct Job {
    AppRun* run;  // 要跑的应用；为空时跑 fn
    void (*fn)(void*);
    void* arg;
};

QueueHandle_t s_jobs;
AppRun* s_current;  // 正开着的（LVGL 锁里读写）
Display* s_display;

Display* display() { return s_display; }

void Post(AppRun* run, int32_t type, int32_t id, int32_t value) {
    if (!run || run->closed) return;
    Event ev = {type, id, value};
    xQueueSend(run->queue, &ev, 0);
}

AppRun* Ctx(wasm_exec_env_t env) {
    return static_cast<AppRun*>(wasm_runtime_get_custom_data(wasm_runtime_get_module_inst(env)));
}

// 改界面：拿显示锁；应用已经关了就什么都不做
class UiScope {
public:
    explicit UiScope(AppRun* run) : lock_(display()), run_(run) {}
    bool ok() const { return run_ && !run_->closed && run_->root; }

private:
    DisplayLockGuard lock_;
    AppRun* run_;
};

lv_obj_t* Obj(AppRun* run, int32_t id) {
    return id >= 1 && id <= static_cast<int32_t>(run->objs.size()) ? run->objs[id - 1] : nullptr;
}

lv_obj_t* Page(AppRun* run) { return run->pages.empty() ? nullptr : run->pages.back(); }

bool IsInside(lv_obj_t* obj, lv_obj_t* ancestor) {
    for (; obj; obj = lv_obj_get_parent(obj)) {
        if (obj == ancestor) return true;
    }
    return false;
}

// 关掉最上面一页（只剩一页时不关）：上面的控件 id 都作废，露出下面那页
bool PopPage(AppRun* run) {
    if (run->pages.size() <= 1) return false;
    lv_obj_t* top = run->pages.back();
    run->pages.pop_back();
    for (auto& o : run->objs) {
        if (o && IsInside(o, top)) o = nullptr;
    }
    lv_obj_delete(top);
    lv_obj_remove_flag(run->pages.back(), LV_OBJ_FLAG_HIDDEN);
    return true;
}

void OnClick(lv_event_t* e) { Post(s_current, EV_CLICK, (int32_t)(intptr_t)lv_event_get_user_data(e), 0); }

lv_obj_t* FirstLabel(lv_obj_t* obj) {
    if (!obj) return nullptr;
    if (lv_obj_check_type(obj, &lv_label_class)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        if (lv_obj_t* l = FirstLabel(lv_obj_get_child(obj, i))) return l;
    }
    return nullptr;
}

void SetColor(lv_obj_t* obj, uint32_t c) { lv_obj_set_style_text_color(obj, lv_color_hex(c), 0); }

/* ---------- 界面 ---------- */

int32_t ui_page(wasm_exec_env_t env, const char* title) {
    AppRun* run = Ctx(env);
    UiScope ui(run);
    if (!ui.ok()) return E_FAIL;
    if (run->loading) {
        lv_obj_delete(run->loading);
        run->loading = nullptr;
    }
    lv_obj_t* page = lv_obj_create(run->root);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(page, 36, 0);
    lv_obj_set_style_pad_ver(page, 20, 0);
    lv_obj_set_style_pad_row(page, 12, 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_AUTO);
    // 放到左边缘拖动条（应用层加在 root 最后）下面
    lv_obj_move_to_index(page, static_cast<int32_t>(lv_obj_get_child_count(run->root)) - 2);
    if (lv_obj_t* prev = Page(run)) lv_obj_add_flag(prev, LV_OBJ_FLAG_HIDDEN);
    run->pages.push_back(page);
    if (title && *title) {
        lv_obj_t* l = lv_label_create(page);
        lv_label_set_text(l, title);
    }
    run->objs.push_back(page);
    return run->objs.size();
}

void ui_page_close(wasm_exec_env_t env) {
    AppRun* run = Ctx(env);
    UiScope ui(run);
    if (ui.ok()) PopPage(run);
}

int32_t ui_text(wasm_exec_env_t env, const char* text, int32_t style) {
    AppRun* run = Ctx(env);
    UiScope ui(run);
    if (!ui.ok() || !Page(run)) return E_FAIL;
    lv_obj_t* l = lv_label_create(Page(run));
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    SetColor(l, style == TEXT_MUTED ? C_MUTED : C_TEXT);
    lv_label_set_text(l, text);
    run->objs.push_back(l);
    return run->objs.size();
}

int32_t ui_button(wasm_exec_env_t env, const char* text, int32_t primary) {
    AppRun* run = Ctx(env);
    UiScope ui(run);
    if (!ui.ok() || !Page(run)) return E_FAIL;
    int32_t id = run->objs.size() + 1;
    lv_obj_t* b = lv_button_create(Page(run));
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 22, 0);
    lv_obj_set_style_pad_ver(b, 10, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? C_ACCENT : C_CONTROL), 0);
    lv_obj_t* l = lv_label_create(b);
    SetColor(l, primary ? 0x0b1220 : C_TEXT);
    lv_label_set_text(l, text);
    lv_obj_add_event_cb(b, OnClick, LV_EVENT_CLICKED, (void*)(intptr_t)id);
    run->objs.push_back(b);
    return id;
}

int32_t ui_item(wasm_exec_env_t env, const char* title, const char* sub, const char* right) {
    AppRun* run = Ctx(env);
    UiScope ui(run);
    if (!ui.ok() || !Page(run)) return E_FAIL;
    int32_t id = run->objs.size() + 1;
    lv_obj_t* row = lv_obj_create(Page(run));
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CONTROL), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_pad_all(row, 14, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* col = lv_obj_create(row);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_t* t = lv_label_create(col);
    lv_label_set_text(t, title);
    if (sub && *sub) {
        lv_obj_t* s = lv_label_create(col);
        SetColor(s, C_MUTED);
        lv_label_set_text(s, sub);
    }
    if (right && *right) {
        lv_obj_t* r = lv_label_create(row);
        SetColor(r, C_MUTED);
        lv_label_set_text(r, right);
    }
    lv_obj_add_event_cb(row, OnClick, LV_EVENT_CLICKED, (void*)(intptr_t)id);
    run->objs.push_back(row);
    return id;
}

void ui_set_text(wasm_exec_env_t env, int32_t id, const char* text) {
    AppRun* run = Ctx(env);
    UiScope ui(run);
    if (!ui.ok()) return;
    if (lv_obj_t* l = FirstLabel(Obj(run, id))) lv_label_set_text(l, text);
}

void ui_set_hidden(wasm_exec_env_t env, int32_t id, int32_t hidden) {
    AppRun* run = Ctx(env);
    UiScope ui(run);
    lv_obj_t* o = ui.ok() ? Obj(run, id) : nullptr;
    if (o && std::find(run->pages.begin(), run->pages.end(), o) == run->pages.end()) {
        lv_obj_set_flag(o, LV_OBJ_FLAG_HIDDEN, hidden != 0);
    }
}

void ui_remove(wasm_exec_env_t env, int32_t id) {
    AppRun* run = Ctx(env);
    UiScope ui(run);
    lv_obj_t* o = ui.ok() ? Obj(run, id) : nullptr;
    if (!o || std::find(run->pages.begin(), run->pages.end(), o) != run->pages.end()) return;  // 页面用 ui_page_close
    for (auto& x : run->objs) {
        if (x && IsInside(x, o)) x = nullptr;
    }
    lv_obj_delete(o);
}

void ui_toast(wasm_exec_env_t env, const char* text) {
    AppRun* run = Ctx(env);
    UiScope ui(run);
    if (ui.ok()) nomi_ui_notify(text, 2500);
}

/* ---------- 存储 ---------- */

// 应用给的路径换成设备上的；不许 ..；权限不够、写只读的地方返回空，err 里是原因
std::string Resolve(AppRun* run, const char* path, bool write, int32_t* err) {
    std::string p = path ? path : "";
    *err = E_FAIL;
    if (p.find("..") != std::string::npos) return "";
    if (p.rfind("data:/", 0) == 0) return run->pkg->dir + "/data/" + p.substr(6);
    if (p.rfind("pkg:/", 0) == 0) {
        if (write) return "";
        return run->pkg->dir + "/assets/" + p.substr(5);
    }
    if (p.rfind("sd:/", 0) == 0) {
        if (write || !run->pkg->Has("sd.read")) {
            *err = E_DENIED;
            return "";
        }
        return std::string("/sd/") + p.substr(4);
    }
    return "";
}

int32_t fs_list(wasm_exec_env_t env, const char* dir, char* buf, uint32_t size) {
    int32_t err;
    std::string path = Resolve(Ctx(env), dir, false, &err);
    if (path.empty()) return err;
    if (!path.empty() && path.back() == '/') path.pop_back();
    DIR* d = opendir(path.c_str());
    if (!d) return NomiSdCard::GetInstance().GetState() == NomiSdCard::kReady ? E_NOT_FOUND : E_NO_SD;
    uint32_t n = 0;
    while (dirent* e = readdir(d)) {
        std::string line = std::string(e->d_name) + (e->d_type == DT_DIR ? "/" : "") + "\n";
        if (n + line.size() + 1 > size) break;
        memcpy(buf + n, line.data(), line.size());
        n += line.size();
    }
    closedir(d);
    if (size) buf[n < size ? n : size - 1] = '\0';
    return n;
}

int32_t fs_size(wasm_exec_env_t env, const char* p) {
    int32_t err;
    std::string path = Resolve(Ctx(env), p, false, &err);
    if (path.empty()) return err;
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? static_cast<int32_t>(st.st_size) : E_NOT_FOUND;
}

int32_t fs_read(wasm_exec_env_t env, const char* p, int32_t offset, void* buf, uint32_t size) {
    int32_t err;
    std::string path = Resolve(Ctx(env), p, false, &err);
    if (path.empty()) return err;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return E_NOT_FOUND;
    if (size > kMaxRead) size = kMaxRead;
    size_t n = fseek(f, offset, SEEK_SET) == 0 ? fread(buf, 1, size, f) : 0;
    fclose(f);
    return static_cast<int32_t>(n);
}

// 小设置和进度：data/kv.json，一个 JSON 对象，改一次写一次（不占设备那 16KB 的 NVS）
void LoadKv(AppRun* run) {
    std::string path = run->pkg->dir + "/data/kv.json";
    FILE* f = fopen(path.c_str(), "rb");
    std::string text;
    if (f) {
        char tmp[512];
        size_t n;
        while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0 && text.size() < 64 * 1024) text.append(tmp, n);
        fclose(f);
    }
    run->kv = cJSON_Parse(text.c_str());
    if (!cJSON_IsObject(run->kv)) {
        cJSON_Delete(run->kv);
        run->kv = cJSON_CreateObject();
    }
}

int32_t kv_get(wasm_exec_env_t env, const char* key, char* buf, uint32_t size) {
    AppRun* run = Ctx(env);
    cJSON* item = cJSON_GetObjectItemCaseSensitive(run->kv, key);
    if (!cJSON_IsString(item) || size == 0) return E_NOT_FOUND;
    size_t len = strlen(item->valuestring);
    size_t n = len < size - 1 ? len : size - 1;
    memcpy(buf, item->valuestring, n);
    buf[n] = '\0';
    return static_cast<int32_t>(n);
}

int32_t kv_set(wasm_exec_env_t env, const char* key, const char* value) {
    AppRun* run = Ctx(env);
    if (!key || !*key) return E_FAIL;
    cJSON* item = cJSON_CreateString(value ? value : "");
    if (cJSON_HasObjectItem(run->kv, key)) cJSON_ReplaceItemInObjectCaseSensitive(run->kv, key, item);
    else cJSON_AddItemToObject(run->kv, key, item);
    std::string dir = run->pkg->dir + "/data";
    mkdir(dir.c_str(), 0775);
    char* json = cJSON_PrintUnformatted(run->kv);
    FILE* f = fopen((dir + "/kv.json").c_str(), "wb");
    bool ok = f && json && fwrite(json, 1, strlen(json), f) == strlen(json);
    if (f) fclose(f);
    cJSON_free(json);
    return ok ? 0 : E_FAIL;
}

/* ---------- 其他 ---------- */

int64_t time_now(wasm_exec_env_t env) {
    (void)env;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec < 1700000000 ? 0 : static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

void StopTimer(AppRun* run, int32_t id) {
    auto it = run->timers.find(id);
    if (it == run->timers.end()) return;
    esp_timer_stop(it->second->handle);
    esp_timer_delete(it->second->handle);
    delete it->second;
    run->timers.erase(it);
}

void timer_start(wasm_exec_env_t env, int32_t id, int32_t ms, int32_t repeat) {
    AppRun* run = Ctx(env);
    StopTimer(run, id);
    auto* slot = new TimerSlot{run, id, nullptr};
    esp_timer_create_args_t args = {};
    args.callback = [](void* arg) {
        auto* s = static_cast<TimerSlot*>(arg);
        Post(s->run, EV_TIMER, s->id, 0);
    };
    args.arg = slot;
    args.name = "app_timer";
    if (esp_timer_create(&args, &slot->handle) != ESP_OK) {
        delete slot;
        return;
    }
    run->timers[id] = slot;
    uint64_t us = static_cast<uint64_t>(ms < 50 ? 50 : ms) * 1000;
    if (repeat) esp_timer_start_periodic(slot->handle, us);
    else esp_timer_start_once(slot->handle, us);
}

void timer_stop(wasm_exec_env_t env, int32_t id) { StopTimer(Ctx(env), id); }

void nomi_log(wasm_exec_env_t env, const char* text) { ESP_LOGI("app", "[%s] %s", Ctx(env)->pkg->id.c_str(), text); }

// 签名：i 是 32 位整数，I 是 64 位，$ 是应用里的字符串，*~ 是应用里的缓冲区和它的长度（运行时检查越界）
NativeSymbol kNatives[] = {
    {"ui_page", (void*)ui_page, "($)i", nullptr},
    {"ui_page_close", (void*)ui_page_close, "()", nullptr},
    {"ui_text", (void*)ui_text, "($i)i", nullptr},
    {"ui_item", (void*)ui_item, "($$$)i", nullptr},
    {"ui_button", (void*)ui_button, "($i)i", nullptr},
    {"ui_set_text", (void*)ui_set_text, "(i$)", nullptr},
    {"ui_set_hidden", (void*)ui_set_hidden, "(ii)", nullptr},
    {"ui_remove", (void*)ui_remove, "(i)", nullptr},
    {"ui_toast", (void*)ui_toast, "($)", nullptr},
    {"fs_list", (void*)fs_list, "($*~)i", nullptr},
    {"fs_size", (void*)fs_size, "($)i", nullptr},
    {"fs_read", (void*)fs_read, "($i*~)i", nullptr},
    {"kv_get", (void*)kv_get, "($*~)i", nullptr},
    {"kv_set", (void*)kv_set, "($$)i", nullptr},
    {"time_now", (void*)time_now, "()I", nullptr},
    {"timer_start", (void*)timer_start, "(iii)", nullptr},
    {"timer_stop", (void*)timer_stop, "(i)", nullptr},
    {"nomi_log", (void*)nomi_log, "($)", nullptr},
};

/* ---------- 跑一个应用 ---------- */

// 出错：弹提示、关掉应用（还开着的话）
void Fail(AppRun* run, const std::string& msg) {
    ESP_LOGW(TAG, "%s: %s", run->pkg->id.c_str(), msg.c_str());
    DisplayLockGuard lock(display());
    nomi_ui_notify(msg.c_str(), 3500);
    if (!run->closed && nomi_app_current() == &run->pkg->app) nomi_app_close();
}

void RunApp(AppRun* run) {
    const NomiPackage* pkg = run->pkg;
    if (run->closed) return;  // 还没轮到它就被关了
    NomiSdCard::Use sd;  // 应用和它的数据在 SD 卡上，跑着的时候一直挂着
    if (!sd) return Fail(run, "SD 卡没插，打不开「" + pkg->name + "」");

    std::string path = pkg->dir + "/app.wasm";
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0 || static_cast<size_t>(st.st_size) > kMaxWasm) {
        return Fail(run, "「" + pkg->name + "」的程序文件不见了，重新装一下");
    }
    // 运行时要一直引用这块（解释器会在里面改写），卸载后才能放
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM));
    FILE* f = fopen(path.c_str(), "rb");
    bool read_ok = buf && f && fread(buf, 1, st.st_size, f) == static_cast<size_t>(st.st_size);
    if (f) fclose(f);
    if (!read_ok) {
        heap_caps_free(buf);
        return Fail(run, "读不了「" + pkg->name + "」的程序文件");
    }

    char err[128] = "";
    wasm_module_t module = wasm_runtime_load(buf, st.st_size, err, sizeof(err));
    wasm_module_inst_t inst = module ? wasm_runtime_instantiate(module, kWasmStack, 0, err, sizeof(err)) : nullptr;
    wasm_exec_env_t env = inst ? wasm_runtime_create_exec_env(inst, kWasmStack) : nullptr;
    wasm_function_inst_t fmain = inst ? wasm_runtime_lookup_function(inst, "nomi_main") : nullptr;
    wasm_function_inst_t fevent = inst ? wasm_runtime_lookup_function(inst, "nomi_event") : nullptr;
    std::string problem;
    if (!env || !fmain || !fevent) {
        problem = err[0] ? err : "找不到 nomi_main / nomi_event";
    } else {
        wasm_runtime_set_custom_data(inst, run);
        LoadKv(run);
        int64_t t0 = esp_timer_get_time();
        if (!wasm_runtime_call_wasm(env, fmain, 0, nullptr)) {
            problem = wasm_runtime_get_exception(inst);
        } else {
            ESP_LOGI(TAG, "%s started in %lld ms", pkg->id.c_str(), (esp_timer_get_time() - t0) / 1000);
        }
        while (problem.empty()) {
            Event ev;
            xQueueReceive(run->queue, &ev, portMAX_DELAY);
            uint32_t argv[3] = {static_cast<uint32_t>(ev.type), static_cast<uint32_t>(ev.id),
                                static_cast<uint32_t>(ev.value)};
            if (!wasm_runtime_call_wasm(env, fevent, 3, argv)) problem = wasm_runtime_get_exception(inst);
            if (ev.type == EV_CLOSE) break;
        }
    }
    if (!problem.empty() && !run->closed) Fail(run, "「" + pkg->name + "」出错了：" + problem);

    if (env) wasm_runtime_destroy_exec_env(env);
    if (inst) wasm_runtime_deinstantiate(inst);
    if (module) wasm_runtime_unload(module);
    heap_caps_free(buf);
    ESP_LOGI(TAG, "%s closed, PSRAM free %u", pkg->id.c_str(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void* Worker(void*) {
    wasm_runtime_init_thread_env();
    for (;;) {
        Job job;
        xQueueReceive(s_jobs, &job, portMAX_DELAY);
        if (!job.run) {
            job.fn(job.arg);
            continue;
        }
        AppRun* run = job.run;
        RunApp(run);
        // 收尾：先停定时器（它们会往队列里塞事件），再放掉其他东西
        while (!run->timers.empty()) StopTimer(run, run->timers.begin()->first);
        cJSON_Delete(run->kv);
        vQueueDelete(run->queue);
        delete run;
    }
}

void* PsramMalloc(unsigned int size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
void* PsramRealloc(void* ptr, unsigned int size) {
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
void PsramFree(void* ptr) { heap_caps_free(ptr); }

}  // namespace

namespace NomiWasm {

void Init(Display* display) {
    s_display = display;
    RuntimeInitArgs args;
    memset(&args, 0, sizeof(args));
    args.mem_alloc_type = Alloc_With_Allocator;  // 运行时和应用内存都放 PSRAM，内部 RAM 很紧
    args.mem_alloc_option.allocator.malloc_func = reinterpret_cast<void*>(PsramMalloc);
    args.mem_alloc_option.allocator.realloc_func = reinterpret_cast<void*>(PsramRealloc);
    args.mem_alloc_option.allocator.free_func = reinterpret_cast<void*>(PsramFree);
    args.native_module_name = "env";
    args.native_symbols = kNatives;
    args.n_native_symbols = sizeof(kNatives) / sizeof(kNatives[0]);
    if (!wasm_runtime_full_init(&args)) {
        ESP_LOGE(TAG, "wasm_runtime_full_init failed");
        return;
    }
    s_jobs = xQueueCreate(4, sizeof(Job));
    // 运行时的 esp-idf 平台层假定自己跑在 pthread 里（加载应用时会调 pthread_self，普通 FreeRTOS 任务里
    // 会断言失败、整机重启），所以常驻任务用 pthread 建，栈照样放 PSRAM
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = kTaskStack;
    cfg.prio = 3;
    cfg.thread_name = "nomi_app";
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_pthread_set_cfg(&cfg);
    pthread_t tid;
    if (pthread_create(&tid, nullptr, Worker, nullptr) == 0) pthread_detach(tid);
    else ESP_LOGE(TAG, "Failed to create app thread");
    cfg = esp_pthread_get_default_config();  // 别影响这个任务以后建的其他 pthread
    esp_pthread_set_cfg(&cfg);
}

Display* GetDisplay() { return s_display; }

void RunInWorker(void (*fn)(void*), void* arg) {
    if (!s_jobs) return;
    Job job = {nullptr, fn, arg};
    xQueueSend(s_jobs, &job, portMAX_DELAY);
}

void Open(const nomi_app_t* app, lv_obj_t* root) {
    auto* run = new AppRun();
    run->pkg = static_cast<const NomiPackage*>(app->user);
    run->root = root;
    run->queue = xQueueCreate(16, sizeof(Event));
    run->loading = lv_label_create(root);
    lv_obj_set_style_text_color(run->loading, lv_color_hex(C_MUTED), 0);
    lv_label_set_text(run->loading, "打开中…");
    lv_obj_center(run->loading);
    s_current = run;
    Job job = {run, nullptr, nullptr};
    if (!s_jobs || xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        run->closed = true;
        lv_label_set_text(run->loading, "应用运行时没起来，打不开");
    }
}

void Close(const nomi_app_t* app) {
    AppRun* run = s_current;
    if (!run || &run->pkg->app != app) return;
    run->closed = true;  // 之后它改界面的调用都不做了，root 由应用层删
    run->root = nullptr;
    s_current = nullptr;
    Event ev = {EV_CLOSE, 0, 0};
    xQueueSend(run->queue, &ev, 0);
}

bool Back(const nomi_app_t* app) {
    AppRun* run = s_current;
    if (!run || &run->pkg->app != app || !PopPage(run)) return false;
    Post(run, EV_BACK, 0, static_cast<int32_t>(run->pages.size()));
    return true;
}

}  // namespace NomiWasm
