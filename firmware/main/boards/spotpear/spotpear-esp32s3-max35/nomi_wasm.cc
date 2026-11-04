#include "nomi_wasm.h"

#include "display/display.h"
#include "nomi_book.h"
#include "nomi_flash_io.h"
#include "nomi_music.h"
#include "nomi_photo.h"
#include "nomi_pkg.h"
#include "nomi_radio.h"
#include "nomi_reader.h"
#include "nomi_sdcard.h"
#include "nomi_style.h"
#include "nomi_ui.h"
#include "nomi_util.h"

#include <cJSON.h>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_pthread.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <wasm_export.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define TAG "NomiWasm"

LV_FONT_DECLARE(nomi_font_icon_36);  // 放在全局：写进匿名命名空间就成了另一个符号，链接时找不到

namespace {

constexpr uint32_t kWasmStack = 16 * 1024;      // 解释器给应用开的栈（运行时从 PSRAM 分）
constexpr uint32_t kThreadStack = 32 * 1024;    // 每个进程的 pthread 栈（PSRAM）
constexpr int kMaxRead = 32 * 1024;             // 一次读文件最多多少
constexpr size_t kMaxNet = 512 * 1024;          // 一次 net_get 最多多少
constexpr size_t kMaxWasm = 1024 * 1024;
constexpr size_t kMaxProcs = 4;                 // 同时跑着的应用最多几个
constexpr int64_t kIdleExitUs = 30 * 1000000LL;  // 在后台、没理由留着多久后结束
constexpr int kVoiceWaitMs = 25000;              // 语音指令等应用回话最多多久（在线点歌要搜、取歌曲页，慢的网站十几秒）
constexpr int kWebWaitMs = 60000;                // 网页请求等应用回话最多多久（应用可能要联网搜一下）

// 和 tools/app-sdk/nomi_sdk.h 一致
enum {
    EV_CLICK = 1, EV_CHANGE, EV_BACK, EV_PAUSE, EV_RESUME, EV_CLOSE, EV_TIMER, EV_VOICE, EV_READER, EV_MEDIA,
    EV_SHOW, EV_HIDE, EV_WEB, EV_PHOTO
};
enum { TEXT_TITLE = 0, TEXT_BODY, TEXT_MUTED };
enum { MEDIA_FINISHED = 1, MEDIA_NEXT = 2, MEDIA_PREV = 3, MEDIA_PLAY = 4 };
constexpr int32_t E_FAIL = -1, E_DENIED = -2, E_NO_SD = -3, E_NOT_FOUND = -4, E_NO_UI = -6, E_NO_NET = -7;

struct Event {
    int32_t type, id, value;
};

struct Proc;
struct TimerSlot {
    Proc* proc;
    int32_t id;
    esp_timer_handle_t handle;
};

// 一个跑着的应用。线程、界面回调、语音指令都可能拿着它，所以用 shared_ptr 管
struct Proc : std::enable_shared_from_this<Proc> {
    const NomiPackage* pkg = nullptr;
    // 界面（显示锁里读写）
    lv_obj_t* root = nullptr;     // 界面挂着时：应用层给的全屏容器
    lv_obj_t* tile = nullptr;     // 首页卡片（占着收听卡片时）
    lv_obj_t* target = nullptr;   // 接下来建的控件放哪：当前页，或卡片
    lv_obj_t* loading = nullptr;  // "打开中…"
    std::vector<lv_obj_t*> objs;  // 控件 id - 1 → 控件（删掉的为空）
    std::vector<lv_obj_t*> pages;
    std::atomic<bool> shown{false};
    // 线程
    QueueHandle_t queue = nullptr;
    std::atomic<bool> exiting{false};
    bool show_after_main = false;
    int64_t idle_since = 0;  // s_procs_mutex 里读写
    cJSON* kv = nullptr;
    std::map<int32_t, TimerSlot*> timers;  // 只在进程自己的线程里动
    // 大模型、网页发来的请求（text_mutex）：一次一个（request_mutex），应用处理完 voice_done
    std::mutex text_mutex, request_mutex;
    std::string context;
    SemaphoreHandle_t voice_done = nullptr;
    std::string req_arg, req_reply;
    bool req_end = false;

    ~Proc() {
        if (queue) vQueueDelete(queue);
        if (voice_done) vSemaphoreDelete(voice_done);
        cJSON_Delete(kv);
    }
};
using ProcPtr = std::shared_ptr<Proc>;

std::mutex s_procs_mutex;  // 拿锁顺序：显示锁在前
std::vector<ProcPtr> s_procs;
Proc* s_tile_owner;  // 占着收听卡片的（显示锁里读写）
Display* s_display;
QueueHandle_t s_jobs;
esp_timer_handle_t s_housekeeping;

Display* display() { return s_display; }

// 还在跑（没在退）的那个
ProcPtr Find(const std::string& id) {
    std::lock_guard<std::mutex> lock(s_procs_mutex);
    for (auto& p : s_procs) {
        if (p->pkg->id == id && !p->exiting) return p;
    }
    return nullptr;
}

void Post(Proc* p, int32_t type, int32_t id, int32_t value) {
    if (!p || p->exiting) return;
    Event ev = {type, id, value};
    xQueueSend(p->queue, &ev, 0);
}

// 让它退：最后收一个 CLOSE（趁这时候存进度），之后的事件都不送了
void Exit(Proc* p) {
    if (p->exiting.exchange(true)) return;
    Event ev = {EV_CLOSE, 0, 0};
    xQueueSend(p->queue, &ev, 0);
}

Proc* Ctx(wasm_exec_env_t env) {
    return static_cast<Proc*>(wasm_runtime_get_custom_data(wasm_runtime_get_module_inst(env)));
}

// 放的东西归这个应用、而且主人还要它放着（对话时让出来的也算）
bool OwnsMedia(const Proc* p) {
    auto& radio = NomiRadio::GetInstance();
    return radio.Wanted() && radio.Current().owner == p->pkg->id;
}

bool KeepResident(const Proc* p) { return p->shown || p->pkg->tile || OwnsMedia(p); }

// 改界面：拿显示锁；进程在退就什么都不做
class UiScope {
public:
    explicit UiScope(Proc* p) : lock_(display()), p_(p) {}
    bool ok() const { return !p_->exiting; }
    lv_obj_t* target() const { return ok() ? p_->target : nullptr; }

private:
    DisplayLockGuard lock_;
    Proc* p_;
};

bool IsInside(lv_obj_t* obj, lv_obj_t* ancestor) {
    if (!ancestor) return false;
    for (; obj; obj = lv_obj_get_parent(obj)) {
        if (obj == ancestor) return true;
    }
    return false;
}

void ForgetInside(Proc* p, lv_obj_t* container) {
    for (auto& o : p->objs) {
        if (o && IsInside(o, container)) o = nullptr;
    }
    if (IsInside(p->target, container)) p->target = nullptr;
}

// 显示锁里、root 删掉之前：界面拿下，里面的控件 id 作废（root 由应用层删）
void DetachUi(Proc* p) {
    if (p->root) ForgetInside(p, p->root);
    p->pages.clear();
    p->root = p->loading = nullptr;
    p->shown = false;
}

// 显示锁里：卡片让出来（换了别的应用占、或者自己退了）
void DropTile(Proc* p) {
    if (!p->tile) return;
    ForgetInside(p, p->tile);
    p->tile = nullptr;
}

void TakeTile(Proc* p) {
    if (s_tile_owner && s_tile_owner != p) DropTile(s_tile_owner);
    s_tile_owner = p;
    p->tile = nomi_ui_listen_slot();
    lv_obj_clean(p->tile);
}

lv_obj_t* Obj(Proc* p, int32_t id) {
    return id >= 1 && id <= static_cast<int32_t>(p->objs.size()) ? p->objs[id - 1] : nullptr;
}

int32_t AddObj(Proc* p, lv_obj_t* o) {
    p->objs.push_back(o);
    return p->objs.size();
}

// 关掉最上面一页（只剩一页时不关）：上面的控件 id 都作废，露出下面那页
bool PopPage(Proc* p) {
    if (p->pages.size() <= 1) return false;
    lv_obj_t* top = p->pages.back();
    p->pages.pop_back();
    bool was_target = IsInside(p->target, top);
    ForgetInside(p, top);
    if (was_target) p->target = p->pages.back();
    lv_obj_delete(top);
    lv_obj_remove_flag(p->pages.back(), LV_OBJ_FLAG_HIDDEN);
    return true;
}

// 可点的控件：记着是哪个进程的哪个 id。进程没了点了也不出事（weak_ptr）
struct ClickTag {
    std::weak_ptr<Proc> proc;
    int32_t id;
};
void BindClick(lv_obj_t* o, Proc* p, int32_t id) {
    auto* tag = new ClickTag{p->weak_from_this(), id};
    lv_obj_add_event_cb(
        o,
        [](lv_event_t* e) {
            auto* t = static_cast<ClickTag*>(lv_event_get_user_data(e));
            if (ProcPtr proc = t->proc.lock()) Post(proc.get(), EV_CLICK, t->id, 0);
        },
        LV_EVENT_CLICKED, tag);
    lv_obj_add_event_cb(
        o, [](lv_event_t* e) { delete static_cast<ClickTag*>(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, tag);
}

lv_obj_t* FirstLabel(lv_obj_t* obj) {
    if (!obj) return nullptr;
    if (lv_obj_check_type(obj, &lv_label_class)) return obj;
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) {
        if (lv_obj_t* l = FirstLabel(lv_obj_get_child(obj, i))) return l;
    }
    return nullptr;
}

void SetColor(lv_obj_t* obj, uint32_t c) { lv_obj_set_style_text_color(obj, lv_color_hex(c), 0); }

// 一页：竖着排、只上下滚
lv_obj_t* MakeColumn(lv_obj_t* parent) {
    lv_obj_t* col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(col, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(col, 36, 0);
    lv_obj_set_style_pad_ver(col, 20, 0);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_AUTO);
    return col;
}

// 应用建的容器（页面、卡片、卡片顶上一排）：ui_target 能指它们，不能藏、不能删
constexpr lv_obj_flag_t kContainer = LV_OBJ_FLAG_USER_1;
constexpr lv_obj_flag_t kTabBar = LV_OBJ_FLAG_USER_2;  // 「正在放」顶上那排：按钮画成小标签

lv_obj_t* AddLoading(lv_obj_t* root) {
    lv_obj_t* l = lv_label_create(root);
    lv_obj_set_style_text_color(l, lv_color_hex(C_MUTED), 0);
    lv_label_set_text(l, "打开中…");
    lv_obj_center(l);
    return l;
}

void CopyOut(const std::string& s, char* buf, uint32_t size, int32_t* n) {
    if (size == 0) {
        *n = E_FAIL;
        return;
    }
    size_t len = std::min<size_t>(s.size(), size - 1);
    memcpy(buf, s.data(), len);
    buf[len] = '\0';
    *n = len;
}

/* ---------- 界面 ---------- */

int32_t ui_page(wasm_exec_env_t env, const char* title) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    if (!ui.ok() || !p->root) return E_NO_UI;  // 在后台：没有界面
    if (p->loading) {
        lv_obj_delete(p->loading);
        p->loading = nullptr;
    }
    lv_obj_t* page = MakeColumn(p->root);
    lv_obj_add_flag(page, kContainer);
    // 放到左边缘拖动条（应用层加在 root 最后）下面
    lv_obj_move_to_index(page, static_cast<int32_t>(lv_obj_get_child_count(p->root)) - 2);
    if (!p->pages.empty()) lv_obj_add_flag(p->pages.back(), LV_OBJ_FLAG_HIDDEN);
    p->pages.push_back(page);
    p->target = page;
    if (title && *title) lv_label_set_text(lv_label_create(page), title);
    return AddObj(p, page);
}

void ui_page_close(wasm_exec_env_t env) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    if (ui.ok()) PopPage(p);
}

// 首页卡片（manifest 里 "tile": true）：清空重画，接下来建的控件放进卡片。返回卡片的 id（ui_target 用）
int32_t ui_tile(wasm_exec_env_t env) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    if (!ui.ok() || !p->tile) return E_DENIED;
    ForgetInside(p, p->tile);
    lv_obj_clean(p->tile);
    lv_obj_t* col = MakeColumn(p->tile);
    lv_obj_set_style_pad_top(col, 16, 0);
    lv_obj_add_flag(col, kContainer);
    p->target = col;
    return AddObj(p, col);
}

// 首页卡片里的「正在放」：和系统收听页一个样（封面、标题、上一首 / 放停 / 下一首、进度、右边歌词），系统刷新。
// 清空卡片重画；返回顶上一排（系统收听页放标签的地方）的 id，接下来建的按钮放进那排，画成小标签
int32_t ui_now_playing(wasm_exec_env_t env, const char* hint) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    if (!ui.ok() || !p->tile) return E_DENIED;
    ForgetInside(p, p->tile);
    lv_obj_clean(p->tile);
    lv_obj_t* bar = nomi_ui_now_playing(p->tile, hint);
    if (!bar) return E_FAIL;
    lv_obj_add_flag(bar, static_cast<lv_obj_flag_t>(kContainer | kTabBar));
    p->target = bar;
    return AddObj(p, bar);
}

// 接下来建的控件放到哪：一页（ui_page 的返回值）或卡片（ui_tile 的返回值）
int32_t ui_target(wasm_exec_env_t env, int32_t id) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    lv_obj_t* o = ui.ok() ? Obj(p, id) : nullptr;
    if (!o || !lv_obj_has_flag(o, kContainer)) return E_NOT_FOUND;
    p->target = o;
    return 0;
}

int32_t ui_text(wasm_exec_env_t env, const char* text, int32_t style) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    lv_obj_t* parent = ui.target();
    if (!parent) return E_NO_UI;
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    SetColor(l, style == TEXT_MUTED ? C_MUTED : C_TEXT);
    lv_label_set_text(l, text);
    return AddObj(p, l);
}

int32_t ui_button(wasm_exec_env_t env, const char* text, int32_t primary) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    lv_obj_t* parent = ui.target();
    if (!parent) return E_NO_UI;
    lv_obj_t* b = lv_button_create(parent);
    bool tab = lv_obj_has_flag(parent, kTabBar);  // 「正在放」顶上那排：和系统收听页的标签一个样
    lv_obj_set_style_radius(b, tab ? 15 : 18, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, tab ? 14 : 22, 0);
    lv_obj_set_style_pad_ver(b, tab ? 0 : 10, 0);
    if (tab) lv_obj_set_size(b, LV_SIZE_CONTENT, 30);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? C_ACCENT : C_CONTROL), 0);
    lv_obj_t* l = lv_label_create(b);
    SetColor(l, primary ? 0x0b1220 : tab ? C_MUTED : C_TEXT);
    lv_label_set_text(l, text);
    if (tab) lv_obj_center(l);
    int32_t id = AddObj(p, b);
    BindClick(b, p, id);
    return id;
}

int32_t ui_item(wasm_exec_env_t env, const char* title, const char* sub, const char* right) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    lv_obj_t* parent = ui.target();
    if (!parent) return E_NO_UI;
    lv_obj_t* row = lv_obj_create(parent);
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
    lv_label_set_text(lv_label_create(col), title);
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
    int32_t id = AddObj(p, row);
    BindClick(row, p, id);
    return id;
}

// 拖条：label 里的 {} 换成当前值，拖的时候跟着变；松手时发 NOMI_EV_CHANGE（value 是新值）
struct SliderTag {
    std::weak_ptr<Proc> proc;
    int32_t id;
    std::string label;
    lv_obj_t* text;
};

std::string SliderText(const std::string& label, int32_t v) {
    size_t at = label.find("{}");
    std::string n = std::to_string(static_cast<long>(v));
    return at == std::string::npos ? label + "  " + n : label.substr(0, at) + n + label.substr(at + 2);
}

int32_t ui_slider(wasm_exec_env_t env, const char* label, int32_t min, int32_t max, int32_t value) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    lv_obj_t* parent = ui.target();
    if (!parent) return E_NO_UI;
    if (max <= min) max = min + 1;
    value = std::min(std::max(value, min), max);
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 16, 0);
    lv_obj_set_style_pad_ver(box, 8, 0);
    lv_obj_set_style_pad_hor(box, 12, 0);  // 留出拖块的半径
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    std::string text = label ? label : "";
    lv_obj_t* l = lv_label_create(box);
    lv_label_set_text(l, SliderText(text, value).c_str());
    lv_obj_t* sl = lv_slider_create(box);
    lv_obj_set_size(sl, LV_PCT(100), 8);
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 8, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, 16);  // 细条好按
    int32_t id = AddObj(p, box);
    auto* tag = new SliderTag{p->weak_from_this(), id, text, l};
    lv_obj_set_user_data(sl, tag);
    lv_obj_add_event_cb(
        sl,
        [](lv_event_t* e) {
            auto* t = static_cast<SliderTag*>(lv_event_get_user_data(e));
            auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
            lv_event_code_t code = lv_event_get_code(e);
            if (code == LV_EVENT_DELETE) {
                delete t;
            } else if (code == LV_EVENT_VALUE_CHANGED) {
                lv_label_set_text(t->text, SliderText(t->label, lv_slider_get_value(target)).c_str());
            } else if (code == LV_EVENT_RELEASED) {
                if (ProcPtr proc = t->proc.lock()) Post(proc.get(), EV_CHANGE, t->id, lv_slider_get_value(target));
            }
        },
        LV_EVENT_ALL, tag);
    return id;
}

// 改拖条的值（不发 NOMI_EV_CHANGE）
void ui_set_value(wasm_exec_env_t env, int32_t id, int32_t value) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    lv_obj_t* box = ui.ok() ? Obj(p, id) : nullptr;
    for (uint32_t i = 0; box && i < lv_obj_get_child_count(box); i++) {
        lv_obj_t* c = lv_obj_get_child(box, i);
        if (!lv_obj_check_type(c, &lv_slider_class)) continue;
        lv_slider_set_value(c, value, LV_ANIM_OFF);
        auto* t = static_cast<SliderTag*>(lv_obj_get_user_data(c));
        if (t) lv_label_set_text(t->text, SliderText(t->label, lv_slider_get_value(c)).c_str());
    }
}

void ui_set_text(wasm_exec_env_t env, int32_t id, const char* text) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    if (!ui.ok()) return;
    if (lv_obj_t* l = FirstLabel(Obj(p, id))) {
        if (strcmp(lv_label_get_text(l), text) != 0) lv_label_set_text(l, text);
    }
}

// 页面、卡片本身不能藏、不能删（页面用 ui_page_close，卡片用 ui_tile 重画）
bool IsContainer(Proc* p, lv_obj_t* o) {
    (void)p;
    return lv_obj_has_flag(o, kContainer);
}

void ui_set_hidden(wasm_exec_env_t env, int32_t id, int32_t hidden) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    lv_obj_t* o = ui.ok() ? Obj(p, id) : nullptr;
    if (o && !IsContainer(p, o)) lv_obj_set_flag(o, LV_OBJ_FLAG_HIDDEN, hidden != 0);
}

void ui_remove(wasm_exec_env_t env, int32_t id) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    lv_obj_t* o = ui.ok() ? Obj(p, id) : nullptr;
    if (!o || IsContainer(p, o)) return;
    ForgetInside(p, o);
    lv_obj_delete(o);
}

void ui_toast(wasm_exec_env_t env, const char* text) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    if (ui.ok()) nomi_ui_notify(text, 2500);
}

// 打开自己的界面（卡片上点了要进应用时用）；界面已经开着什么都不做。之后会来 SHOW
void ui_open(wasm_exec_env_t env) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    if (ui.ok() && !p->root) nomi_app_open(p->pkg->id.c_str());
}

/* ---------- ui_player：系统画的「正在放」（封面、歌名、进度、上一首 / 放停 / 下一首） ---------- */

#define ICON_PLAY "\xEF\x81\x8B"
#define ICON_PAUSE "\xEF\x81\x8C"
#define ICON_PREV "\xEF\x81\x88"
#define ICON_NEXT "\xEF\x81\x91"
constexpr int kCover = NomiMusic::kCoverSize;

struct Player {
    lv_obj_t *cover, *title, *artist, *time, *slider, *play;
    lv_image_dsc_t dsc;
    int cover_version = -1;
    lv_timer_t* timer;
    std::weak_ptr<Proc> owner;  // 画这个控件的应用
    std::string owner_id;
};

// 上一首 / 下一首：交给放这个的应用（队列、台单都是它的）
void PlayerStep(bool next) { NomiWasm::MediaCommand(next ? MEDIA_NEXT : MEDIA_PREV); }

// 放 / 停：在放就停；停着的是这个应用自己的就接着放；别的（或什么都没有）交给应用决定放什么
void PlayerToggle(Player* p) {
    auto& radio = NomiRadio::GetInstance();
    if (radio.Wanted() || radio.Current().owner == p->owner_id) {
        radio.Toggle();
    } else if (ProcPtr proc = p->owner.lock()) {
        Post(proc.get(), EV_MEDIA, 0, MEDIA_PLAY);
    }
}

void SetTextIfChanged(lv_obj_t* label, const std::string& text) {
    if (strcmp(lv_label_get_text(label), text.c_str()) != 0) lv_label_set_text(label, text.c_str());
}

std::string Clock(int s) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
    return buf;
}

// 0 没在放、1 连接中、2 在放、3 暂停（自己停的、或对话时让出来的）
int32_t MediaState() {
    auto& radio = NomiRadio::GetInstance();
    if (!radio.Wanted()) return radio.Current().episode && radio.Position() > 0 ? 3 : 0;
    switch (radio.GetState()) {
    case NomiRadio::kPlaying: return 2;
    case NomiRadio::kHeld: return 3;
    case NomiRadio::kOff: return 0;
    default: return 1;
    }
}

void PlayerRefresh(Player* p) {
    auto& radio = NomiRadio::GetInstance();
    NomiRadio::Station st = radio.Current();
    bool any = !st.url.empty();
    SetTextIfChanged(p->title, any ? st.name : "没在放");
    SetTextIfChanged(p->artist, any ? st.podcast : "");
    int pos = st.episode ? radio.Position() : 0, dur = st.episode ? st.duration : 0;
    SetTextIfChanged(p->time, st.episode ? Clock(pos) + (dur > 0 ? " / " + Clock(dur) : "") : "");
    lv_obj_set_flag(p->slider, LV_OBJ_FLAG_HIDDEN, !st.episode);  // 电台直播没有进度
    if (st.episode && !lv_obj_has_state(p->slider, LV_STATE_PRESSED)) {
        lv_slider_set_range(p->slider, 0, dur > 0 ? dur : 1);
        lv_slider_set_value(p->slider, dur > 0 ? std::min(pos, dur) : 0, LV_ANIM_OFF);
    }
    int state = MediaState();
    SetTextIfChanged(lv_obj_get_child(p->play, 0), state == 1 || state == 2 ? ICON_PAUSE : ICON_PLAY);
    int version;
    const uint16_t* px = NomiMusic::GetInstance().Cover(version);
    bool show = st.song && px;
    if (show && (version != p->cover_version || p->dsc.data != reinterpret_cast<const uint8_t*>(px))) {
        p->cover_version = version;
        memset(&p->dsc, 0, sizeof(p->dsc));
        p->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        p->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        p->dsc.header.w = kCover;
        p->dsc.header.h = kCover;
        p->dsc.header.stride = kCover * 2;
        p->dsc.data = reinterpret_cast<const uint8_t*>(px);
        p->dsc.data_size = kCover * kCover * 2;
        lv_image_set_src(p->cover, &p->dsc);
        lv_obj_invalidate(p->cover);
    }
    lv_obj_set_style_image_opa(p->cover, show ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

lv_obj_t* PlayerButton(lv_obj_t* parent, const char* icon, int size, lv_event_cb_t cb, Player* p) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, size / 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CONTROL), 0);
    lv_obj_t* l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &nomi_font_icon_36, 0);
    lv_label_set_text(l, icon);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, p);
    return b;
}

int32_t ui_player(wasm_exec_env_t env) {
    Proc* proc = Ctx(env);
    UiScope ui(proc);
    lv_obj_t* parent = ui.target();
    if (!parent) return E_NO_UI;
    auto* p = new Player();
    p->owner = proc->weak_from_this();
    p->owner_id = proc->pkg->id;
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 10, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* top = lv_obj_create(box);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 16, 0);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    p->cover = lv_image_create(top);
    lv_obj_set_size(p->cover, kCover, kCover);
    lv_obj_set_style_radius(p->cover, 10, 0);
    lv_obj_set_style_clip_corner(p->cover, true, 0);
    lv_obj_set_style_bg_color(p->cover, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(p->cover, LV_OPA_COVER, 0);
    lv_obj_t* info = lv_obj_create(top);
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(info, 1);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(info, 4, 0);
    lv_obj_remove_flag(info, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    p->title = lv_label_create(info);
    lv_obj_set_width(p->title, LV_PCT(100));
    lv_label_set_long_mode(p->title, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    p->artist = lv_label_create(info);
    lv_obj_set_width(p->artist, LV_PCT(100));
    lv_label_set_long_mode(p->artist, LV_LABEL_LONG_MODE_DOTS);
    SetColor(p->artist, C_MUTED);
    p->time = lv_label_create(info);
    SetColor(p->time, C_MUTED);

    p->slider = lv_slider_create(box);
    lv_obj_set_size(p->slider, LV_PCT(100), 6);
    lv_obj_set_style_bg_color(p->slider, lv_color_hex(C_CONTROL), LV_PART_MAIN);
    lv_obj_set_style_bg_color(p->slider, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(p->slider, lv_color_hex(C_TEXT), LV_PART_KNOB);
    lv_obj_set_style_pad_all(p->slider, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(
        p->slider,
        [](lv_event_t* e) { NomiRadio::GetInstance().SeekTo(lv_slider_get_value(lv_event_get_target_obj(e))); },
        LV_EVENT_RELEASED, nullptr);

    lv_obj_t* row = lv_obj_create(box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    // 上一首 / 下一首交给放这个的应用（队列是它的）
    PlayerButton(row, ICON_PREV, 56, [](lv_event_t*) { PlayerStep(false); }, p);
    p->play = PlayerButton(row, ICON_PLAY, 68,
                           [](lv_event_t* e) {
                               auto* pl = static_cast<Player*>(lv_event_get_user_data(e));
                               PlayerToggle(pl);
                               PlayerRefresh(pl);
                           },
                           p);
    lv_obj_set_style_bg_color(p->play, lv_color_hex(C_ACCENT), 0);
    SetColor(lv_obj_get_child(p->play, 0), 0x0b1220);
    PlayerButton(row, ICON_NEXT, 56, [](lv_event_t*) { PlayerStep(true); }, p);

    p->timer = lv_timer_create([](lv_timer_t* t) { PlayerRefresh(static_cast<Player*>(lv_timer_get_user_data(t))); },
                               500, p);
    lv_obj_add_event_cb(
        box,
        [](lv_event_t* e) {
            auto* pl = static_cast<Player*>(lv_event_get_user_data(e));
            lv_timer_delete(pl->timer);
            delete pl;
        },
        LV_EVENT_DELETE, p);
    PlayerRefresh(p);
    return AddObj(proc, box);
}

/* ---------- 存储 ---------- */

// SD 卡上的路径：先挂上卡（已经挂着就只是计个数）；别的路径什么都不做（没插卡时别白等一次挂载）
struct SdIfNeeded {
    std::unique_ptr<NomiSdCard::Use> use;
    explicit SdIfNeeded(const std::string& path) {
        if (path.rfind("/sd/", 0) == 0) use = std::make_unique<NomiSdCard::Use>();
    }
};

// 应用给的路径换成设备上的；不许 ..；权限不够、写只读的地方返回空，err 里是原因
std::string Resolve(Proc* p, const char* path, bool write, int32_t* err) {
    std::string s = path ? path : "";
    *err = E_FAIL;
    if (s.find("..") != std::string::npos) return "";
    if (s.rfind("data:/", 0) == 0) return p->pkg->dir + "/data/" + s.substr(6);
    if (s.rfind("pkg:/", 0) == 0) return write ? "" : p->pkg->dir + "/assets/" + s.substr(5);
    if (s.rfind("sd:/", 0) == 0) {
        if (write || !p->pkg->Has("sd.read")) {
            *err = E_DENIED;
            return "";
        }
        return std::string("/sd/") + s.substr(4);
    }
    return "";
}

int32_t fs_list(wasm_exec_env_t env, const char* dir, char* buf, uint32_t size) {
    int32_t err;
    std::string path = Resolve(Ctx(env), dir, false, &err);
    if (path.empty()) return err;
    if (path.back() == '/') path.pop_back();
    SdIfNeeded sd(path);
    int32_t result = 0;
    NomiFlashIo::ForPath(path, [&] {
        DIR* d = opendir(path.c_str());
        if (!d) {
            result = sd.use && !*sd.use ? E_NO_SD : E_NOT_FOUND;  // 卡上的路径、卡又没挂上：没插卡
            return;
        }
        uint32_t n = 0;
        while (dirent* e = readdir(d)) {
            std::string line = std::string(e->d_name) + (e->d_type == DT_DIR ? "/" : "") + "\n";
            if (n + line.size() + 1 > size) break;
            memcpy(buf + n, line.data(), line.size());
            n += line.size();
        }
        closedir(d);
        if (size) buf[n < size ? n : size - 1] = '\0';
        result = n;
    });
    return result;
}

int32_t fs_size(wasm_exec_env_t env, const char* path) {
    int32_t err;
    std::string s = Resolve(Ctx(env), path, false, &err);
    if (s.empty()) return err;
    SdIfNeeded sd(s);
    int32_t result = E_NOT_FOUND;
    NomiFlashIo::ForPath(s, [&] {
        struct stat st;
        if (stat(s.c_str(), &st) == 0) result = static_cast<int32_t>(st.st_size);
    });
    return result;
}

int32_t fs_read(wasm_exec_env_t env, const char* path, int32_t offset, void* buf, uint32_t size) {
    int32_t err;
    std::string s = Resolve(Ctx(env), path, false, &err);
    if (s.empty()) return err;
    SdIfNeeded sd(s);
    if (size > kMaxRead) size = kMaxRead;
    int32_t result = E_NOT_FOUND;
    NomiFlashIo::ForPath(s, [&] {
        FILE* f = fopen(s.c_str(), "rb");
        if (!f) return;
        result = fseek(f, offset, SEEK_SET) == 0 ? static_cast<int32_t>(fread(buf, 1, size, f)) : 0;
        fclose(f);
    });
    return result;
}

/* ---------- 阅读器和书：ui_reader、text_chapters、fs_find ---------- */

constexpr int32_t READER_MENU = -1, READER_END = -2;  // NOMI_EV_READER 的 value（翻页时是位置，≥ 0）
constexpr const char* kChapterCache = "/sd/nomi/cache/chapters";  // 章节缓存（第一次要把整本书读一遍）

// 阅读器占满当前页（这页不留边、不滚：上下划是翻页）。翻到新的一页、长按、翻过最后一页都发 NOMI_EV_READER
int32_t ui_reader(wasm_exec_env_t env, const char* path, int32_t offset, int32_t flags) {
    Proc* p = Ctx(env);
    int32_t err;
    std::string dev = Resolve(p, path, false, &err);
    if (dev.empty()) return err;
    SdIfNeeded sd(dev);
    if (sd.use && !*sd.use) return E_NO_SD;
    UiScope ui(p);
    lv_obj_t* parent = ui.target();
    if (!parent) return E_NO_UI;
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    int32_t id = static_cast<int32_t>(p->objs.size()) + 1;  // AddObj 给的就是这个
    std::weak_ptr<Proc> weak = p->weak_from_this();
    std::string error;
    lv_obj_t* r = NomiReader::Create(
        parent, dev, offset < 0 ? 0 : static_cast<uint32_t>(offset), flags,
        [weak, id](NomiReader::Event ev, uint32_t at) {
            ProcPtr proc = weak.lock();
            if (!proc) return;
            int32_t value = ev == NomiReader::kPage ? static_cast<int32_t>(std::min<uint32_t>(at, INT32_MAX))
                            : ev == NomiReader::kMenu ? READER_MENU
                                                      : READER_END;
            Post(proc.get(), EV_READER, id, value);
        },
        error);
    if (!r) {
        nomi_ui_notify(error.c_str(), 3000);
        return E_NOT_FOUND;
    }
    return AddObj(p, r);
}

void reader_goto(wasm_exec_env_t env, int32_t id, int32_t offset) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    if (ui.ok()) NomiReader::Goto(Obj(p, id), offset < 0 ? 0 : static_cast<uint32_t>(offset));
}

void reader_set_flags(wasm_exec_env_t env, int32_t id, int32_t flags) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    if (ui.ok()) NomiReader::SetFlags(Obj(p, id), flags);
}

// 这一页的字（给大模型当上下文）：放不下就截掉，不截半个字
int32_t reader_text(wasm_exec_env_t env, int32_t id, char* buf, uint32_t size) {
    Proc* p = Ctx(env);
    std::string text;
    {
        UiScope ui(p);
        lv_obj_t* o = ui.ok() ? Obj(p, id) : nullptr;
        if (!NomiReader::Is(o)) return E_NOT_FOUND;
        text = NomiReader::PageText(o);
    }
    if (size == 0) return E_FAIL;
    size_t n = std::min<size_t>(text.size(), size - 1);
    while (n > 0 && n < text.size() && (static_cast<uint8_t>(text[n]) & 0xC0) == 0x80) n--;
    memcpy(buf, text.data(), n);
    buf[n] = '\0';
    return static_cast<int32_t>(n);
}

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

struct ChapterIndex {
    std::string path;
    int64_t size = -1, mtime = 0;
    std::vector<NomiBook::Chapter> list;
};
std::mutex s_chapter_mutex;
ChapterIndex s_chapters;  // 最近一本书的章节（换书时换掉）

uint32_t Fnv(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

// 拿着 s_chapter_mutex 调。卡上的书在 kChapterCache 留缓存：文件大小、修改时间对得上就直接用
const ChapterIndex* LoadChapters(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return nullptr;
    ChapterIndex& ix = s_chapters;
    if (ix.path == path && ix.size == st.st_size && ix.mtime == static_cast<int64_t>(st.st_mtime)) return &ix;
    ix = ChapterIndex();
    char header[64], name[16];
    snprintf(header, sizeof(header), "v1 %lld %lld\n", static_cast<long long>(st.st_size),
             static_cast<long long>(st.st_mtime));
    snprintf(name, sizeof(name), "%08lx.txt", static_cast<unsigned long>(Fnv(path)));
    std::string cache = std::string(kChapterCache) + "/" + name;
    bool on_sd = path.rfind("/sd/", 0) == 0, loaded = false;
    if (on_sd) {
        if (FILE* f = fopen(cache.c_str(), "rb")) {
            char line[512];
            if (fgets(line, sizeof(line), f) && strcmp(line, header) == 0 && fgets(line, sizeof(line), f) &&
                path + "\n" == line) {  // 第二行是书的路径：防哈希撞上
                loaded = true;
                while (fgets(line, sizeof(line), f)) {
                    char* tab = strchr(line, '\t');
                    if (!tab) continue;
                    *tab = '\0';
                    std::string title = tab + 1;
                    if (!title.empty() && title.back() == '\n') title.pop_back();
                    ix.list.push_back({static_cast<uint32_t>(strtoul(line, nullptr, 10)), title});
                }
            }
            fclose(f);
        }
    }
    if (!loaded) {
        NomiBook book;
        int64_t t0 = esp_timer_get_time();
        if (!book.Open(path) || !book.ScanChapters(ix.list)) {
            ix = ChapterIndex();
            return nullptr;
        }
        ESP_LOGI(TAG, "Chapters of %s: %u (%lld ms)", path.c_str(), static_cast<unsigned>(ix.list.size()),
                 (esp_timer_get_time() - t0) / 1000);
        if (on_sd && MakeDirs(kChapterCache)) {
            if (FILE* f = fopen(cache.c_str(), "wb")) {
                fputs(header, f);
                fprintf(f, "%s\n", path.c_str());
                for (const auto& c : ix.list) fprintf(f, "%u\t%s\n", static_cast<unsigned>(c.offset), c.title.c_str());
                fclose(f);
            }
        }
    }
    ix.path = path;
    ix.size = st.st_size;
    ix.mtime = st.st_mtime;
    return &ix;
}

// 章节：返回一共几章；buf 里从第 first 章起一行一个「位置\t章名」，放不下的不放
int32_t text_chapters(wasm_exec_env_t env, const char* path, int32_t first, char* buf, uint32_t size) {
    int32_t err;
    std::string s = Resolve(Ctx(env), path, false, &err);
    if (s.empty()) return err;
    SdIfNeeded sd(s);
    if (sd.use && !*sd.use) return E_NO_SD;
    std::lock_guard<std::mutex> lock(s_chapter_mutex);
    const ChapterIndex* ix = nullptr;
    NomiFlashIo::ForPath(s, [&] { ix = LoadChapters(s); });
    if (!ix) return E_NOT_FOUND;
    uint32_t n = 0;
    for (size_t i = first < 0 ? 0 : first; i < ix->list.size(); i++) {
        std::string line = std::to_string(ix->list[i].offset) + "\t" + ix->list[i].title + "\n";
        if (n + line.size() + 1 > size) break;
        memcpy(buf + n, line.data(), line.size());
        n += line.size();
    }
    if (size) buf[n] = '\0';
    return static_cast<int32_t>(ix->list.size());
}

// offset 在第几章（从 0 起）；在第一章之前、或者这本书没有章节，返回 -1
int32_t text_chapter_at(wasm_exec_env_t env, const char* path, int32_t offset) {
    int32_t err;
    std::string s = Resolve(Ctx(env), path, false, &err);
    if (s.empty()) return err;
    SdIfNeeded sd(s);
    if (sd.use && !*sd.use) return E_NO_SD;
    std::lock_guard<std::mutex> lock(s_chapter_mutex);
    const ChapterIndex* ix = nullptr;
    NomiFlashIo::ForPath(s, [&] { ix = LoadChapters(s); });
    if (!ix) return E_NOT_FOUND;
    auto it = std::upper_bound(ix->list.begin(), ix->list.end(), static_cast<uint32_t>(std::max<int32_t>(offset, 0)),
                               [](uint32_t v, const NomiBook::Chapter& c) { return v < c.offset; });
    return static_cast<int32_t>(it - ix->list.begin()) - 1;
}

bool SkipDir(const std::string& path, const char* name) {
    return name[0] == '.' || strcasecmp(name, "System Volume Information") == 0 || strcasecmp(name, "$RECYCLE.BIN") == 0 ||
           path == "/sd/nomi";  // 系统自己的（应用、缓存）
}

void FindIn(const std::string& dir, int depth, const std::vector<std::string>& exts, int64_t min_size, int64_t deadline,
            std::string& out, size_t cap, int32_t& count) {
    if (depth > 8 || esp_timer_get_time() > deadline || out.size() >= cap) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> subdirs;
    while (dirent* e = readdir(d)) {
        std::string child = dir + "/" + e->d_name;
        if (e->d_type == DT_DIR) {
            if (!SkipDir(child, e->d_name)) subdirs.push_back(child);
            continue;
        }
        const char* dot = strrchr(e->d_name, '.');
        if (e->d_name[0] == '.' || !dot) continue;  // . 开头的（macOS 的 ._ 文件）不要
        if (std::find(exts.begin(), exts.end(), nomi::Lower(dot + 1)) == exts.end()) continue;
        struct stat st;
        if (stat(child.c_str(), &st) != 0 || st.st_size < min_size) continue;
        std::string line = "sd:/" + child.substr(4) + "\t" + std::to_string(static_cast<long long>(st.st_size)) + "\n";
        if (out.size() + line.size() + 1 > cap) break;
        out += line;
        count++;
    }
    closedir(d);
    std::sort(subdirs.begin(), subdirs.end());
    for (const auto& sub : subdirs) FindIn(sub, depth + 1, exts, min_size, deadline, out, cap, count);
}

// 在卡上 dir 下面（含子文件夹）找扩展名是 exts（逗号分开，不带点）之一、不小于 min_size 字节的文件
int32_t fs_find(wasm_exec_env_t env, const char* dir, const char* exts, int32_t min_size, char* buf, uint32_t size) {
    int32_t err;
    std::string root = Resolve(Ctx(env), dir, false, &err);
    if (root.empty()) return err;
    while (root.size() > 3 && root.back() == '/') root.pop_back();
    if (root != "/sd" && root.rfind("/sd/", 0) != 0) return E_DENIED;  // 只找 SD 卡
    NomiSdCard::Use sd;
    if (!sd) return E_NO_SD;
    std::vector<std::string> list;
    std::string e = exts ? exts : "";
    for (size_t i = 0; i <= e.size();) {
        size_t j = e.find(',', i);
        if (j == std::string::npos) j = e.size();
        std::string one = nomi::Lower(e.substr(i, j - i));
        if (!one.empty() && one[0] == '.') one.erase(0, 1);
        if (!one.empty()) list.push_back(one);
        i = j + 1;
    }
    if (list.empty() || size == 0) return E_FAIL;
    std::string out;
    int32_t count = 0;
    int64_t t0 = esp_timer_get_time();
    FindIn(root, 0, list, min_size, t0 + 20 * 1000000LL, out, size, count);
    ESP_LOGI(TAG, "fs_find %s (%s): %d files in %lld ms", root.c_str(), e.c_str(), static_cast<int>(count),
             (esp_timer_get_time() - t0) / 1000);
    memcpy(buf, out.data(), out.size());
    buf[out.size()] = '\0';
    return count;
}

/* ---------- 图片：ui_grid、ui_thumb、ui_photo、photo_frame ---------- */

constexpr int kThumbSize = 88;  // 一行 4 个（页面去掉两边留白 408 像素宽）
enum { PHOTO_NEXT = 1, PHOTO_PREV = 2, PHOTO_MENU = 3 };  // NOMI_EV_PHOTO 的 value

// 网格：控件一排排往右放、放满换行（缩略图墙）；ui_target 能指它
int32_t ui_grid(wasm_exec_env_t env) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    lv_obj_t* parent = ui.target();
    if (!parent) return E_NO_UI;
    lv_obj_t* g = lv_obj_create(parent);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(g, 10, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g, kContainer);
    return AddObj(p, g);
}

// 一张缩略图（方块、铺满裁切），点了来 NOMI_EV_CLICK。系统在后台解，解好的存在卡上，下次秒开
int32_t ui_thumb(wasm_exec_env_t env, const char* path) {
    Proc* p = Ctx(env);
    int32_t err;
    std::string dev = Resolve(p, path, false, &err);
    if (dev.empty()) return err;
    UiScope ui(p);
    lv_obj_t* parent = ui.target();
    if (!parent) return E_NO_UI;
    lv_obj_t* t = NomiPhoto::CreateThumb(parent, dev, kThumbSize);
    int32_t id = AddObj(p, t);
    BindClick(t, p, id);
    return id;
}

// 全屏看图：占满当前页；翻图手势、点中间要菜单都发 NOMI_EV_PHOTO（换哪张由应用定，photo_show）
int32_t ui_photo(wasm_exec_env_t env, const char* path, int32_t flags) {
    Proc* p = Ctx(env);
    int32_t err;
    std::string dev = Resolve(p, path, false, &err);
    if (dev.empty()) return err;
    UiScope ui(p);
    lv_obj_t* parent = ui.target();
    if (!parent) return E_NO_UI;
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    int32_t id = static_cast<int32_t>(p->objs.size()) + 1;  // AddObj 给的就是这个
    std::weak_ptr<Proc> weak = p->weak_from_this();
    lv_obj_t* v = NomiPhoto::CreateViewer(parent, dev, flags, [weak, id](NomiPhoto::Event ev) {
        if (ProcPtr proc = weak.lock()) {
            Post(proc.get(), EV_PHOTO, id,
                 ev == NomiPhoto::kNext ? PHOTO_NEXT : ev == NomiPhoto::kPrev ? PHOTO_PREV : PHOTO_MENU);
        }
    });
    return AddObj(p, v);
}

void photo_show(wasm_exec_env_t env, int32_t id, const char* path) {
    Proc* p = Ctx(env);
    int32_t err;
    std::string dev = Resolve(p, path, false, &err);
    if (dev.empty()) return;
    UiScope ui(p);
    if (ui.ok()) NomiPhoto::ViewerShow(Obj(p, id), dev);
}

// 预读：后台把这张解成全屏图存进缓存（排在别的后面），翻过去就快
void photo_preload(wasm_exec_env_t env, const char* path) {
    int32_t err;
    std::string dev = Resolve(Ctx(env), path, false, &err);
    if (!dev.empty()) NomiPhoto::Preload(dev);
}

void photo_set_flags(wasm_exec_env_t env, int32_t id, int32_t flags) {
    Proc* p = Ctx(env);
    UiScope ui(p);
    if (ui.ok()) NomiPhoto::ViewerSetFlags(Obj(p, id), flags);
}

// 电子相框：睡着时轮播 dir 这个文件夹里的图，每 seconds 秒换一张；dir 空 = 不设了
int32_t photo_frame(wasm_exec_env_t env, const char* dir, int32_t seconds) {
    if (!dir || !*dir) {
        NomiPhoto::SetFrame("", 0);
        return 0;
    }
    int32_t err;
    std::string dev = Resolve(Ctx(env), dir, false, &err);
    if (dev.empty()) return err;
    while (dev.size() > 4 && dev.back() == '/') dev.pop_back();
    if (dev.rfind("/sd/", 0) != 0) return E_DENIED;
    NomiPhoto::SetFrame(dev, seconds);
    return 0;
}

// 小设置和进度：data/kv.json，一个 JSON 对象，改一次写一次（不占设备那 16KB 的 NVS）
void LoadKv(Proc* p) {
    std::string text, path = p->pkg->dir + "/data/kv.json";
    NomiFlashIo::ForPath(path, [&] {
        if (FILE* f = fopen(path.c_str(), "rb")) {
            char tmp[512];
            size_t n;
            while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0 && text.size() < 64 * 1024) text.append(tmp, n);
            fclose(f);
        }
    });
    p->kv = cJSON_Parse(text.c_str());
    if (!cJSON_IsObject(p->kv)) {
        cJSON_Delete(p->kv);
        p->kv = cJSON_CreateObject();
    }
}

int32_t kv_get(wasm_exec_env_t env, const char* key, char* buf, uint32_t size) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(Ctx(env)->kv, key);
    if (!cJSON_IsString(item)) return E_NOT_FOUND;
    int32_t n;
    CopyOut(item->valuestring, buf, size, &n);
    return n;
}

int32_t kv_set(wasm_exec_env_t env, const char* key, const char* value) {
    Proc* p = Ctx(env);
    if (!key || !*key) return E_FAIL;
    cJSON* item = cJSON_CreateString(value ? value : "");
    if (cJSON_HasObjectItem(p->kv, key)) cJSON_ReplaceItemInObjectCaseSensitive(p->kv, key, item);
    else cJSON_AddItemToObject(p->kv, key, item);
    std::string dir = p->pkg->dir + "/data";
    char* json = cJSON_PrintUnformatted(p->kv);
    bool ok = false;
    NomiFlashIo::ForPath(dir, [&] {
        mkdir(dir.c_str(), 0775);
        FILE* f = fopen((dir + "/kv.json").c_str(), "wb");
        ok = f && json && fwrite(json, 1, strlen(json), f) == strlen(json);
        if (f) fclose(f);
    });
    cJSON_free(json);
    return ok ? 0 : E_FAIL;
}

/* ---------- 播放（权限 media）：交给系统播放器，放的东西记在这个应用名下 ---------- */

// 网上的一首 / 一期先探一下：https 的先试 http（不占 TLS 的内部 RAM）；按文件头认格式（扩展名、响应头
// 常对不上，m4a 写成 .aac 之类）。探到了返回能用的地址；都不是音频（下架了跳到网页之类）返回空
std::string ProbeRemote(const std::string& url, NomiRadio::Station::Format* format) {
    std::vector<std::string> tries;
    if (url.rfind("https://", 0) == 0) tries.push_back("http://" + url.substr(8));
    tries.push_back(url);
    for (const auto& u : tries) {
        auto& radio = NomiRadio::GetInstance();
        bool held = u.rfind("https://", 0) == 0 && radio.HoldForHttps();
        std::string head;
        bool ok = nomi::HttpGet(u, head, 16);
        if (held) radio.ReleaseHttpsHold();
        if (ok && (*format = NomiRadio::SniffFormat(head)) != NomiRadio::Station::kAuto) return u;
    }
    return "";
}

// flags：1 直播（电台：没有进度，断了自动重连）；2 节目（播客：按节目显示，左右键是倒回 15 秒 / 快进 30 秒）；
// 都没有是一首歌。src：sd:/ 的文件（还要 sd.read）或 http(s) 地址。start 从第几秒放；duration、bytes 是
// 总长（秒）和文件大小（字节，网上的 MP3 按时间跳转要用），不知道填 0
int32_t media_play(wasm_exec_env_t env, const char* src, const char* title, const char* artist, int32_t flags,
                   int32_t start, int32_t duration, int32_t bytes) {
    Proc* p = Ctx(env);
    if (!p->pkg->Has("media")) return E_DENIED;
    std::string s = src ? src : "", url;
    bool remote = s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
    bool live = (flags & 1) && remote, show = (flags & 2) && !live;
    auto format = NomiRadio::Station::kAuto;
    if (remote) {
        url = s;
        if (!live) {
            if (!nomi::NetworkUp()) return E_NO_NET;
            std::string ok = ProbeRemote(s, &format);
            if (!ok.empty()) url = ok;
            else if (!show) return E_NOT_FOUND;  // 歌：不是音频就算放不了（应用好换下一首）；节目探不到也试着放
        }
    } else {
        int32_t err;
        url = Resolve(p, src, false, &err);
        if (url.empty() || url.rfind("/sd/", 0) != 0) return url.empty() ? err : E_FAIL;
        NomiSdCard::Use sd;
        struct stat st;
        if (stat(url.c_str(), &st) != 0) return E_NOT_FOUND;
    }
    NomiRadio::Station item;
    item.url = url;
    item.name = title && *title ? title : url.substr(url.rfind('/') + 1);
    item.podcast = artist ? artist : "";
    item.local = !remote;
    item.episode = !live;
    item.song = !live && !show;
    item.start = std::max(0, static_cast<int>(start));
    item.duration = std::max(0, static_cast<int>(duration));
    item.bytes_per_sec = bytes > 0 && duration > 0 ? bytes / duration : 0;
    item.format = format;
    item.owner = p->pkg->id;
    NomiRadio::GetInstance().PlayEpisode(item);
    if (item.local) NomiMusic::GetInstance().ShowLocal(url);
    else if (!live) NomiMusic::GetInstance().ShowRemote(url, "", "");
    return 0;
}

// 正在放的这首（网上的）给封面地址（JPEG）和 LRC 歌词；SD 卡上的歌系统自己读
void media_meta(wasm_exec_env_t env, const char* cover_url, const char* lrc) {
    Proc* p = Ctx(env);
    NomiRadio::Station st = NomiRadio::GetInstance().Current();
    if (!p->pkg->Has("media") || st.owner != p->pkg->id || st.local || !st.episode) return;
    NomiMusic::GetInstance().ShowRemote(st.url, cover_url ? cover_url : "", lrc ? lrc : "");
}

// 和 nomi_sdk.h 一致：1 暂停、2 接着放、5 停、6 暂停 / 接着放（上一首下一首是应用自己的队列）
void media_control(wasm_exec_env_t env, int32_t cmd) {
    if (!Ctx(env)->pkg->Has("media")) return;
    auto& radio = NomiRadio::GetInstance();
    if ((cmd == 1 && radio.Wanted()) || (cmd == 2 && !radio.Wanted()) || cmd == 6) radio.Toggle();
    else if (cmd == 5) radio.Stop();
}

void media_seek(wasm_exec_env_t env, int32_t second) {
    if (Ctx(env)->pkg->Has("media")) NomiRadio::GetInstance().SeekTo(second);
}

int32_t media_state(wasm_exec_env_t env) {
    (void)env;
    return MediaState();
}

int32_t media_position(wasm_exec_env_t env) {
    (void)env;
    return NomiRadio::GetInstance().Position();
}

int32_t media_duration(wasm_exec_env_t env) {
    (void)env;
    return NomiRadio::GetInstance().Current().duration;
}

// 现在放的是不是自己放的：1 是
int32_t media_mine(wasm_exec_env_t env) { return NomiRadio::GetInstance().Current().owner == Ctx(env)->pkg->id; }

// "标题\n歌手\n时长秒"：标签读不到时标题是文件名
int32_t media_info(wasm_exec_env_t env, const char* path, char* buf, uint32_t size) {
    int32_t err;
    std::string s = Resolve(Ctx(env), path, false, &err);
    if (s.empty()) return err;
    SdIfNeeded sd(s);
    NomiMusic::Track t;
    int duration = 0;
    NomiMusic::ReadInfo(s, t, duration);
    int32_t n;
    CopyOut(t.title + "\n" + t.artist + "\n" + std::to_string(duration), buf, size, &n);
    return n;
}

/* ---------- 联网（权限 net） ---------- */

// GET 一个地址（跟着跳转），正文最多 size 字节（够了就断开）。until 不空时数到它出现 count 次就不往下收了
// （RSS 只要最新几期：until = "</item>"）；最多等 20 秒，收到多少算多少。成功（HTTP 200）返回字节数
int32_t NetGet(Proc* p, const char* url, char* buf, uint32_t size, const char* until, int32_t count) {
    if (!p->pkg->Has("net")) return E_DENIED;
    std::string u = url ? url : "";
    if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0) return E_FAIL;
    if (!nomi::NetworkUp()) return E_NO_NET;
    std::string mark = until ? until : "";
    int found = 0;
    size_t scanned = 0;
    int64_t deadline = esp_timer_get_time() + 20 * 1000000LL;
    auto enough = [&](const std::string& b) {
        if (esp_timer_get_time() > deadline) return true;
        if (mark.empty() || count <= 0) return false;
        for (size_t at; (at = b.find(mark, scanned)) != std::string::npos;) {
            found++;
            scanned = at + mark.size();
        }
        if (b.size() > scanned + mark.size()) scanned = b.size() - mark.size();
        return found >= count;
    };
    // 内部 RAM 只够同时一条 TLS：正在放的若是 https，先让出来，取完接着放
    auto& radio = NomiRadio::GetInstance();
    bool held = u.rfind("https://", 0) == 0 && radio.HoldForHttps();
    std::string body;
    bool ok = nomi::HttpGet(u, body, std::min<size_t>(size, kMaxNet), nullptr, enough);
    if (held) radio.ReleaseHttpsHold();
    if (!ok) return E_FAIL;
    size_t n = std::min<size_t>(body.size(), size);
    memcpy(buf, body.data(), n);
    return n;
}

int32_t net_get(wasm_exec_env_t env, const char* url, char* buf, uint32_t size) {
    return NetGet(Ctx(env), url, buf, size, nullptr, 0);
}

int32_t net_get_until(wasm_exec_env_t env, const char* url, char* buf, uint32_t size, const char* until,
                      int32_t count) {
    return NetGet(Ctx(env), url, buf, size, until, count);
}

/* ---------- 大模型（权限 assistant） ---------- */

// 每轮对话的系统提示里带上这段（应用开在前台时）：比如"正在看《xx》第 3 章"
void assistant_context(wasm_exec_env_t env, const char* text) {
    Proc* p = Ctx(env);
    if (!p->pkg->Has("assistant")) return;
    std::lock_guard<std::mutex> lock(p->text_mutex);
    p->context = text ? std::string(text).substr(0, 600) : "";
}

// 语音指令（NOMI_EV_VOICE）、网页请求（NOMI_EV_WEB）带的参数
int32_t voice_arg(wasm_exec_env_t env, char* buf, uint32_t size) {
    Proc* p = Ctx(env);
    std::lock_guard<std::mutex> lock(p->text_mutex);
    int32_t n;
    CopyOut(p->req_arg, buf, size, &n);
    return n;
}

// 回给大模型的话；end = 1 时这句说完就回待机（开始放歌了：不然对话还开着、声音出不来）
void voice_reply(wasm_exec_env_t env, const char* text, int32_t end) {
    Proc* p = Ctx(env);
    std::lock_guard<std::mutex> lock(p->text_mutex);
    p->req_reply = text ? std::string(text).substr(0, 600) : "";
    p->req_end = end != 0;
}

// 回给网页的（一般是 JSON），最多 32KB
void web_reply(wasm_exec_env_t env, const char* text) {
    Proc* p = Ctx(env);
    std::lock_guard<std::mutex> lock(p->text_mutex);
    p->req_reply = text ? std::string(text).substr(0, 32 * 1024) : "";
}

/* ---------- 其他 ---------- */

int64_t time_now(wasm_exec_env_t env) {
    (void)env;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec < 1700000000 ? 0 : static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

void StopTimer(Proc* p, int32_t id) {
    auto it = p->timers.find(id);
    if (it == p->timers.end()) return;
    esp_timer_stop(it->second->handle);
    esp_timer_delete(it->second->handle);
    delete it->second;
    p->timers.erase(it);
}

void timer_start(wasm_exec_env_t env, int32_t id, int32_t ms, int32_t repeat) {
    Proc* p = Ctx(env);
    StopTimer(p, id);
    if (p->timers.size() >= 8) return;
    auto* slot = new TimerSlot{p, id, nullptr};
    esp_timer_create_args_t args = {};
    args.callback = [](void* arg) {
        auto* s = static_cast<TimerSlot*>(arg);
        Post(s->proc, EV_TIMER, s->id, 0);
    };
    args.arg = slot;
    args.name = "app_timer";
    if (esp_timer_create(&args, &slot->handle) != ESP_OK) {
        delete slot;
        return;
    }
    p->timers[id] = slot;
    uint64_t us = static_cast<uint64_t>(ms < 50 ? 50 : ms) * 1000;
    if (repeat) esp_timer_start_periodic(slot->handle, us);
    else esp_timer_start_once(slot->handle, us);
}

void timer_stop(wasm_exec_env_t env, int32_t id) { StopTimer(Ctx(env), id); }

void nomi_log(wasm_exec_env_t env, const char* text) { ESP_LOGI("app", "[%s] %s", Ctx(env)->pkg->id.c_str(), text); }

// 签名：i 32 位整数，I 64 位，$ 应用里的字符串，*~ 应用里的缓冲区和它的长度（运行时检查越界）
NativeSymbol kNatives[] = {
    {"ui_page", (void*)ui_page, "($)i", nullptr},
    {"ui_page_close", (void*)ui_page_close, "()", nullptr},
    {"ui_tile", (void*)ui_tile, "()i", nullptr},
    {"ui_target", (void*)ui_target, "(i)i", nullptr},
    {"ui_now_playing", (void*)ui_now_playing, "($)i", nullptr},
    {"ui_text", (void*)ui_text, "($i)i", nullptr},
    {"ui_item", (void*)ui_item, "($$$)i", nullptr},
    {"ui_button", (void*)ui_button, "($i)i", nullptr},
    {"ui_set_text", (void*)ui_set_text, "(i$)", nullptr},
    {"ui_slider", (void*)ui_slider, "($iii)i", nullptr},
    {"ui_set_value", (void*)ui_set_value, "(ii)", nullptr},
    {"ui_set_hidden", (void*)ui_set_hidden, "(ii)", nullptr},
    {"ui_remove", (void*)ui_remove, "(i)", nullptr},
    {"ui_toast", (void*)ui_toast, "($)", nullptr},
    {"ui_open", (void*)ui_open, "()", nullptr},
    {"ui_player", (void*)ui_player, "()i", nullptr},
    {"fs_list", (void*)fs_list, "($*~)i", nullptr},
    {"fs_size", (void*)fs_size, "($)i", nullptr},
    {"fs_read", (void*)fs_read, "($i*~)i", nullptr},
    {"fs_find", (void*)fs_find, "($$i*~)i", nullptr},
    {"ui_reader", (void*)ui_reader, "($ii)i", nullptr},
    {"ui_grid", (void*)ui_grid, "()i", nullptr},
    {"ui_thumb", (void*)ui_thumb, "($)i", nullptr},
    {"ui_photo", (void*)ui_photo, "($i)i", nullptr},
    {"photo_show", (void*)photo_show, "(i$)", nullptr},
    {"photo_set_flags", (void*)photo_set_flags, "(ii)", nullptr},
    {"photo_preload", (void*)photo_preload, "($)", nullptr},
    {"photo_frame", (void*)photo_frame, "($i)i", nullptr},
    {"reader_goto", (void*)reader_goto, "(ii)", nullptr},
    {"reader_set_flags", (void*)reader_set_flags, "(ii)", nullptr},
    {"reader_text", (void*)reader_text, "(i*~)i", nullptr},
    {"text_chapters", (void*)text_chapters, "($i*~)i", nullptr},
    {"text_chapter_at", (void*)text_chapter_at, "($i)i", nullptr},
    {"kv_get", (void*)kv_get, "($*~)i", nullptr},
    {"kv_set", (void*)kv_set, "($$)i", nullptr},
    {"media_play", (void*)media_play, "($$$iiii)i", nullptr},
    {"media_meta", (void*)media_meta, "($$)", nullptr},
    {"media_control", (void*)media_control, "(i)", nullptr},
    {"media_seek", (void*)media_seek, "(i)", nullptr},
    {"media_state", (void*)media_state, "()i", nullptr},
    {"media_position", (void*)media_position, "()i", nullptr},
    {"media_duration", (void*)media_duration, "()i", nullptr},
    {"media_mine", (void*)media_mine, "()i", nullptr},
    {"media_info", (void*)media_info, "($*~)i", nullptr},
    {"net_get", (void*)net_get, "($*~)i", nullptr},
    {"net_get_until", (void*)net_get_until, "($*~$i)i", nullptr},
    {"assistant_context", (void*)assistant_context, "($)", nullptr},
    {"voice_arg", (void*)voice_arg, "(*~)i", nullptr},
    {"voice_reply", (void*)voice_reply, "($i)", nullptr},
    {"web_body", (void*)voice_arg, "(*~)i", nullptr},
    {"web_reply", (void*)web_reply, "($)", nullptr},
    {"time_now", (void*)time_now, "()I", nullptr},
    {"timer_start", (void*)timer_start, "(iii)", nullptr},
    {"timer_stop", (void*)timer_stop, "(i)", nullptr},
    {"nomi_log", (void*)nomi_log, "($)", nullptr},
};

/* ---------- 进程 ---------- */

// 出错：弹提示、界面开着的话关掉，进程结束
void Fail(Proc* p, const std::string& msg) {
    ESP_LOGW(TAG, "%s: %s", p->pkg->id.c_str(), msg.c_str());
    p->exiting = true;
    DisplayLockGuard lock(display());
    nomi_ui_notify(msg.c_str(), 3500);
    if (p->root && nomi_app_current() == &p->pkg->app) nomi_app_close();
}

void RunProc(Proc* p) {
    const NomiPackage* pkg = p->pkg;
    // 装在 SD 卡上的：程序和数据都在卡上，跑着的时候卡一直挂着。装在机器里的不占着卡（sd:/ 的读写每次自己挂）
    std::unique_ptr<NomiSdCard::Use> sd;
    if (!pkg->internal) {
        sd = std::make_unique<NomiSdCard::Use>();
        if (!*sd) {
            NomiPkg::GetInstance().CheckSd();  // 卡不在了：桌面上把卡上的应用灰掉
            return Fail(p, "插上 SD 卡才能打开「" + pkg->name + "」");
        }
    }

    std::string path = pkg->dir + "/app.wasm";
    struct stat st;
    bool found = false, read_ok = false;
    uint8_t* buf = nullptr;
    NomiFlashIo::ForPath(path, [&] {
        found = stat(path.c_str(), &st) == 0 && st.st_size > 0 && static_cast<size_t>(st.st_size) <= kMaxWasm;
        if (!found) return;
        // 运行时要一直引用这块（解释器会在里面改写），卸载后才能放
        buf = static_cast<uint8_t*>(heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM));
        FILE* f = fopen(path.c_str(), "rb");
        read_ok = buf && f && fread(buf, 1, st.st_size, f) == static_cast<size_t>(st.st_size);
        if (f) fclose(f);
    });
    if (!found) return Fail(p, "「" + pkg->name + "」的程序文件不见了，重新装一下");
    if (!read_ok) {
        heap_caps_free(buf);
        return Fail(p, "读不了「" + pkg->name + "」的程序文件");
    }

    char err[128] = "";
    wasm_module_t module = wasm_runtime_load(buf, st.st_size, err, sizeof(err));
    wasm_module_inst_t inst = module ? wasm_runtime_instantiate(module, kWasmStack, 0, err, sizeof(err)) : nullptr;
    wasm_exec_env_t env = inst ? wasm_runtime_create_exec_env(inst, kWasmStack) : nullptr;
    wasm_function_inst_t fmain = inst ? wasm_runtime_lookup_function(inst, "nomi_main") : nullptr;
    wasm_function_inst_t fevent = inst ? wasm_runtime_lookup_function(inst, "nomi_event") : nullptr;
    std::string problem;
    auto deliver = [&](const Event& ev) {
        uint32_t argv[3] = {static_cast<uint32_t>(ev.type), static_cast<uint32_t>(ev.id),
                            static_cast<uint32_t>(ev.value)};
        if (!wasm_runtime_call_wasm(env, fevent, 3, argv)) problem = wasm_runtime_get_exception(inst);
    };
    if (!env || !fmain || !fevent) {
        problem = err[0] ? err : "找不到 nomi_main / nomi_event";
    } else {
        wasm_runtime_set_custom_data(inst, p);
        LoadKv(p);
        int64_t t0 = esp_timer_get_time();
        if (!wasm_runtime_call_wasm(env, fmain, 0, nullptr)) problem = wasm_runtime_get_exception(inst);
        ESP_LOGI(TAG, "%s started in %lld ms%s, PSRAM free %u (largest %u)", pkg->id.c_str(),
                 (esp_timer_get_time() - t0) / 1000, p->show_after_main ? "" : " (background)",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        if (problem.empty() && p->show_after_main && p->shown) deliver({EV_SHOW, 0, 0});
        while (problem.empty()) {
            Event ev;
            xQueueReceive(p->queue, &ev, portMAX_DELAY);
            deliver(ev);
            if (ev.type == EV_VOICE || ev.type == EV_WEB) xSemaphoreGive(p->voice_done);
            if (ev.type == EV_CLOSE) break;
        }
    }
    if (!problem.empty() && !p->exiting) Fail(p, "「" + pkg->name + "」出错了：" + problem);

    if (env) wasm_runtime_destroy_exec_env(env);
    if (inst) wasm_runtime_deinstantiate(inst);
    if (module) wasm_runtime_unload(module);
    heap_caps_free(buf);
}

void* ProcThread(void* arg) {
    ProcPtr self = *static_cast<ProcPtr*>(arg);
    delete static_cast<ProcPtr*>(arg);
    Proc* p = self.get();
    wasm_runtime_init_thread_env();
    RunProc(p);
    wasm_runtime_destroy_thread_env();
    // 收尾：先停定时器，界面上留下的拿掉（卡片还给系统的收听页），再从列表里去掉
    p->exiting = true;
    xSemaphoreGive(p->voice_done);  // 等着语音回话的别干等
    while (!p->timers.empty()) StopTimer(p, p->timers.begin()->first);
    {
        DisplayLockGuard lock(display());
        if (p->root && nomi_app_current() == &p->pkg->app) nomi_app_close();
        DetachUi(p);
        if (s_tile_owner == p) {
            DropTile(p);
            nomi_ui_listen_slot_release();
            s_tile_owner = nullptr;
        }
        p->target = nullptr;
        std::lock_guard<std::mutex> procs(s_procs_mutex);
        s_procs.erase(std::remove(s_procs.begin(), s_procs.end(), self), s_procs.end());
    }
    ESP_LOGI(TAG, "%s exited, PSRAM free %u", p->pkg->id.c_str(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return nullptr;
}

// 起一个进程。root 不为空：界面挂着（显示锁里调）。有卡片的应用占首页收听卡片
ProcPtr Spawn(const NomiPackage* pkg, lv_obj_t* root) {
    size_t running;
    {
        std::lock_guard<std::mutex> lock(s_procs_mutex);
        running = s_procs.size();
    }
    if (running >= kMaxProcs) {
        DisplayLockGuard ui(display());
        nomi_ui_notify(("同时开着的应用太多，打不开「" + pkg->name + "」").c_str(), 3500);
        if (root && nomi_app_current() == &pkg->app) nomi_app_close();
        return nullptr;
    }
    auto p = std::make_shared<Proc>();
    p->pkg = pkg;
    p->queue = xQueueCreate(16, sizeof(Event));
    p->voice_done = xSemaphoreCreateBinary();
    {
        DisplayLockGuard lock(display());
        if (root) {
            p->root = root;
            p->shown = true;
            p->show_after_main = true;
            p->loading = AddLoading(root);
        }
        if (pkg->tile) TakeTile(p.get());
        std::lock_guard<std::mutex> procs(s_procs_mutex);
        s_procs.push_back(p);
    }
    // 运行时的 esp-idf 平台层假定自己跑在 pthread 里（加载应用时会调 pthread_self），栈放 PSRAM
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = kThreadStack;
    cfg.prio = 3;
    cfg.thread_name = "nomi_app";
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_pthread_set_cfg(&cfg);
    pthread_t tid;
    auto* arg = new ProcPtr(p);
    bool ok = pthread_create(&tid, nullptr, ProcThread, arg) == 0;
    cfg = esp_pthread_get_default_config();  // 别影响这个任务以后建的其他 pthread
    esp_pthread_set_cfg(&cfg);
    if (ok) {
        pthread_detach(tid);
        return p;
    }
    delete arg;
    ESP_LOGE(TAG, "Failed to start %s", pkg->id.c_str());
    Fail(p.get(), "内存不够，打不开「" + pkg->name + "」");
    DisplayLockGuard lock(display());
    DetachUi(p.get());
    if (s_tile_owner == p.get()) {
        DropTile(p.get());
        nomi_ui_listen_slot_release();
        s_tile_owner = nullptr;
    }
    std::lock_guard<std::mutex> procs(s_procs_mutex);
    s_procs.erase(std::remove(s_procs.begin(), s_procs.end(), p), s_procs.end());
    return nullptr;
}

// 还在跑的那个，没在跑就在后台拉起来
ProcPtr FindOrSpawn(const std::string& id) {
    if (ProcPtr p = Find(id)) return p;
    const NomiPackage* pkg = NomiPkg::GetInstance().Find(id);
    return pkg ? Spawn(pkg, nullptr) : nullptr;
}

// 每 5 秒：在后台、没理由留着（没卡片、放的东西不归它或主人停了）的进程，30 秒后结束
void Housekeeping(void*) {
    int64_t now = esp_timer_get_time();
    std::lock_guard<std::mutex> lock(s_procs_mutex);
    for (auto& p : s_procs) {
        if (p->exiting || KeepResident(p.get())) {
            p->idle_since = 0;
        } else if (p->idle_since == 0) {
            p->idle_since = now;
        } else if (now - p->idle_since > kIdleExitUs) {
            ESP_LOGI(TAG, "%s idle in background, exiting", p->pkg->id.c_str());
            Exit(p.get());
        }
    }
}

// 系统任务：开机扫描装好的包之类的活
struct Job {
    void (*fn)(void*);
    void* arg;
};

void* JobThread(void*) {
    for (;;) {
        Job job;
        xQueueReceive(s_jobs, &job, portMAX_DELAY);
        job.fn(job.arg);
    }
    return nullptr;
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
    // 应用放的歌 / 节目放完了（主循环里）：告诉那个应用，接着放什么由它定
    NomiRadio::GetInstance().on_finished = [](const NomiRadio::Station& item) {
        if (item.owner.empty()) return;
        if (ProcPtr p = FindOrSpawn(item.owner)) Post(p.get(), EV_MEDIA, 0, MEDIA_FINISHED);
    };

    // 系统任务也是 pthread：扫描时会登记卡片应用、起进程
    s_jobs = xQueueCreate(4, sizeof(Job));
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8192;
    cfg.prio = 3;
    cfg.thread_name = "nomi_jobs";
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_pthread_set_cfg(&cfg);
    pthread_t tid;
    if (pthread_create(&tid, nullptr, JobThread, nullptr) == 0) pthread_detach(tid);
    else ESP_LOGE(TAG, "Failed to create job thread");
    cfg = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&cfg);

    esp_timer_create_args_t targs = {};
    targs.callback = Housekeeping;
    targs.name = "app_house";
    esp_timer_create(&targs, &s_housekeeping);
    esp_timer_start_periodic(s_housekeeping, 5 * 1000000ULL);
}

Display* GetDisplay() { return s_display; }

void RunInWorker(void (*fn)(void*), void* arg) {
    if (!s_jobs) return;
    Job job = {fn, arg};
    xQueueSend(s_jobs, &job, portMAX_DELAY);
}

void Open(const nomi_app_t* app, lv_obj_t* root) {
    auto* pkg = static_cast<const NomiPackage*>(app->user);
    if (ProcPtr p = Find(pkg->id)) {  // 在后台跑着：把界面挂上去，它收到 SHOW 再建页面
        DetachUi(p.get());
        p->root = root;
        p->shown = true;
        p->loading = AddLoading(root);
        Post(p.get(), EV_SHOW, 0, 0);
        return;
    }
    Spawn(pkg, root);
}

void Close(const nomi_app_t* app) {
    ProcPtr live;
    {
        std::lock_guard<std::mutex> lock(s_procs_mutex);
        for (auto& p : s_procs) {
            if (p->pkg->id != app->id || !p->root) continue;
            DetachUi(p.get());  // 在退的也要拿下：root 马上要删了
            if (!p->exiting) live = p;
        }
    }
    if (!live) return;
    if (KeepResident(live.get())) Post(live.get(), EV_HIDE, 0, 0);  // 有卡片、或在放它的歌：留在后台
    else Exit(live.get());
}

bool Back(const nomi_app_t* app) {
    ProcPtr p = Find(app->id);
    if (!p || !p->root || !PopPage(p.get())) return false;
    Post(p.get(), EV_BACK, 0, static_cast<int32_t>(p->pages.size()));
    return true;
}

void TileToggle() {
    auto& radio = NomiRadio::GetInstance();
    Proc* owner = s_tile_owner;
    if (radio.Wanted() || !owner || radio.Current().owner == owner->pkg->id) radio.Toggle();
    else Post(owner, EV_MEDIA, 0, MEDIA_PLAY);  // 停着的不是它的（或什么都没放过）：让卡片的应用决定放什么
}

void StartBackground(const NomiPackage* pkg) {
    if (!Find(pkg->id)) Spawn(pkg, nullptr);
}

void Kill(const std::string& id) {
    DisplayLockGuard lock(display());
    std::vector<ProcPtr> victims;
    {
        std::lock_guard<std::mutex> procs(s_procs_mutex);
        for (auto& p : s_procs) {
            if (p->pkg->id == id) victims.push_back(p);
        }
    }
    for (auto& p : victims) {
        if (p->root && nomi_app_current() == &p->pkg->app) nomi_app_close();
        DetachUi(p.get());
        if (s_tile_owner == p.get()) {
            DropTile(p.get());
            nomi_ui_listen_slot_release();
            s_tile_owner = nullptr;
        }
        Exit(p.get());
    }
}

bool MediaCommand(int code) {
    std::string owner = NomiRadio::GetInstance().Current().owner;
    if (owner.empty()) return false;
    ProcPtr p = FindOrSpawn(owner);  // 应用不在跑了（在后台待太久被结束）：拉起来处理
    if (!p) return false;
    Post(p.get(), EV_MEDIA, 0, code);
    return true;
}

}  // namespace NomiWasm

namespace {

// 把一个请求（语音指令、网页）交给应用（没在跑就在后台拉起来），等它处理完。出错返回 "!原因"；
// 应用没回话返回空
std::string Request(const NomiPackage* pkg, int32_t type, int32_t id, const std::string& arg, int wait_ms,
                    bool* end) {
    ProcPtr p = FindOrSpawn(pkg->id);
    if (!p) return "!「" + pkg->name + "」起不来";
    std::lock_guard<std::mutex> one(p->request_mutex);  // 一次一个：回话放在同一个地方
    {
        std::lock_guard<std::mutex> lock(p->text_mutex);
        p->req_arg = arg;
        p->req_reply.clear();
        p->req_end = false;
    }
    xSemaphoreTake(p->voice_done, 0);  // 清掉上次没人收的
    Post(p.get(), type, id, 0);
    if (xSemaphoreTake(p->voice_done, pdMS_TO_TICKS(wait_ms)) != pdTRUE) return "!「" + pkg->name + "」没回应";
    std::lock_guard<std::mutex> lock(p->text_mutex);
    if (p->exiting && p->req_reply.empty()) return "!「" + pkg->name + "」出错退出了";
    if (end) *end = p->req_end;
    return p->req_reply;
}

}  // namespace

namespace NomiWasm {

std::string VoiceCommand(const std::string& app, const std::string& command, const std::string& arg,
                         bool* end_conversation) {
    const NomiPackage* pkg = NomiPkg::GetInstance().Find(app);
    if (!pkg) return "!没装这个应用：" + app;
    int index = -1;
    for (size_t i = 0; i < pkg->voice.size(); i++) {
        if (pkg->voice[i].name == command) index = i;
    }
    if (index < 0) return "!「" + pkg->name + "」没有这个指令：" + command;
    std::string reply = Request(pkg, EV_VOICE, index + 1, arg, kVoiceWaitMs, end_conversation);
    return reply.empty() ? "已交给「" + pkg->name + "」" : reply;
}

std::string WebCall(const std::string& app, const std::string& body) {
    const NomiPackage* pkg = NomiPkg::GetInstance().Find(app);
    if (!pkg) return "!没装这个应用：" + app;
    std::string reply = Request(pkg, EV_WEB, 0, body, kWebWaitMs, nullptr);
    return reply.empty() ? "{}" : reply;
}

std::string PromptContext() {
    std::lock_guard<std::mutex> lock(s_procs_mutex);
    for (auto& p : s_procs) {
        if (!p->shown || p->exiting) continue;
        std::lock_guard<std::mutex> text_lock(p->text_mutex);
        std::string out = "主人正开着应用「" + p->pkg->name + "」";
        return p->context.empty() ? out + "。" : out + "：" + p->context;
    }
    return "";
}

}  // namespace NomiWasm
