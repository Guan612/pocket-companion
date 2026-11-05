#include "nomi_filelist.h"

#include "display/display.h"
#include "nomi_sdcard.h"
#include "nomi_style.h"
#include "nomi_display.h"

#include <esp_log.h>
#include <esp_pthread.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <pthread.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>

#define TAG "NomiFileList"

namespace {

constexpr size_t kChunk = 60;  // 一次画几行（每行四个控件，几百个一起画太占内存）

struct State {
    NomiFileList::Options opt;
    lv_obj_t* list = nullptr;  // 删了就是 nullptr（后台列完回来先看它）
    lv_obj_t* body = nullptr;  // 这一层的内容：换一层整个换掉
    std::string dir;
    uint32_t gen = 0;  // 第几次列：列得慢的旧结果不要
    std::vector<NomiFiles::Entry> entries;
    size_t shown = 0;
};
using StatePtr = std::shared_ptr<State>;

StatePtr Get(lv_obj_t* list) {
    auto* holder = static_cast<StatePtr*>(lv_obj_get_user_data(list));
    return holder ? *holder : nullptr;
}

std::string Join(const std::string& dir, const std::string& name) { return dir.empty() ? name : dir + "/" + name; }

std::string Parent(const std::string& dir) {
    size_t slash = dir.rfind('/');
    return slash == std::string::npos ? "" : dir.substr(0, slash);
}

bool Matches(const std::string& name, const std::vector<std::string>& exts) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) {
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    }
    return std::find(exts.begin(), exts.end(), ext) != exts.end();
}

/* ---------- 后台：一个线程，一件件做 ---------- */

std::mutex s_jobs_mutex;
std::deque<std::function<void()>> s_jobs;
SemaphoreHandle_t s_wake;

void* JobThread(void*) {
    for (;;) {
        xSemaphoreTake(s_wake, portMAX_DELAY);
        for (;;) {
            std::function<void()> job;
            {
                std::lock_guard<std::mutex> lock(s_jobs_mutex);
                if (s_jobs.empty()) break;
                job = std::move(s_jobs.front());
                s_jobs.pop_front();
            }
            job();
        }
    }
    return nullptr;
}

/* ---------- 画 ---------- */

void Load(const StatePtr& st, const std::string& dir);
void AddChunk(const StatePtr& st);

lv_obj_t* NewBody(const StatePtr& st) {
    if (st->body) {  // 往往是它里面的一行点了触发的：先藏起来，回头再删
        lv_obj_add_flag(st->body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(st->body);
    }
    lv_obj_t* body = st->body = lv_obj_create(st->list);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_remove_flag(body, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return body;
}

lv_obj_t* Note(lv_obj_t* parent, const std::string& text, uint32_t color = C_MUTED) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text.c_str());
    return l;
}

// 一行（和应用里的列表项一个样子，矮一点）
lv_obj_t* Row(lv_obj_t* parent, int height) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CONTROL), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t* RowLabel(lv_obj_t* row, const std::string& text, uint32_t color) {
    lv_obj_t* l = lv_label_create(row);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text.c_str());
    return l;
}

// 细线框画的小图标：文件夹（扁、蓝）、文件（竖、灰）
void TypeIcon(lv_obj_t* row, bool dir) {
    lv_obj_t* box = lv_obj_create(row);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 24, 24);
    lv_obj_remove_flag(box, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_t* shape = lv_obj_create(box);
    lv_obj_remove_style_all(shape);
    lv_obj_set_size(shape, dir ? 22 : 16, dir ? 16 : 20);
    lv_obj_center(shape);
    lv_obj_set_style_border_width(shape, 2, 0);
    lv_obj_set_style_border_color(shape, lv_color_hex(dir ? C_ACCENT : C_MUTED), 0);
    lv_obj_set_style_radius(shape, dir ? 3 : 2, 0);
    lv_obj_remove_flag(shape, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
}

// 拿到一行所在列表的状态（行在 body 里，body 在列表里）
StatePtr FromRow(lv_obj_t* row) {
    lv_obj_t* body = lv_obj_get_parent(row);
    return body ? Get(lv_obj_get_parent(body)) : nullptr;
}

void OnEntryClick(lv_event_t* e) {
    StatePtr st = FromRow(lv_event_get_current_target_obj(e));
    size_t i = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    if (!st || i >= st->entries.size()) return;
    NomiFiles::Entry entry = st->entries[i];
    std::string rel = Join(st->dir, entry.name);
    if (entry.dir) {
        Load(st, rel);
    } else if (st->opt.on_pick) {
        auto pick = st->opt.on_pick;  // 回调里可能把列表删了
        pick(rel, false);
    }
}

void OnEntryLong(lv_event_t* e) {
    StatePtr st = FromRow(lv_event_get_current_target_obj(e));
    size_t i = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    if (!st || i >= st->entries.size() || !st->opt.on_long) return;
    NomiFiles::Entry entry = st->entries[i];
    auto on_long = st->opt.on_long;
    on_long(Join(st->dir, entry.name), entry);
}

void AddChunk(const StatePtr& st) {
    size_t end = std::min(st->entries.size(), st->shown + kChunk);
    for (size_t i = st->shown; i < end; i++) {
        const auto& en = st->entries[i];
        lv_obj_t* row = Row(st->body, 52);
        TypeIcon(row, en.dir);
        RowLabel(row, en.name, C_TEXT);
        if (!en.dir) {
            lv_obj_t* size = lv_label_create(row);
            lv_obj_set_style_text_color(size, lv_color_hex(C_MUTED), 0);
            lv_label_set_text(size, NomiFileList::SizeText(en.size).c_str());
        }
        void* index = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
        // 长按之后松手不算点（SHORT_CLICKED 只在短按时来）
        lv_obj_add_event_cb(row, OnEntryClick, st->opt.on_long ? LV_EVENT_SHORT_CLICKED : LV_EVENT_CLICKED, index);
        if (st->opt.on_long) lv_obj_add_event_cb(row, OnEntryLong, LV_EVENT_LONG_PRESSED, index);
    }
    st->shown = end;
    if (end >= st->entries.size()) return;
    size_t left = st->entries.size() - end;
    lv_obj_t* more = Row(st->body, 44);
    RowLabel(more, "再列 " + std::to_string(std::min(left, kChunk)) + " 个（还有 " + std::to_string(left) + " 个）",
             C_ACCENT);
    lv_obj_add_event_cb(
        more,
        [](lv_event_t* e) {
            lv_obj_t* row = lv_event_get_current_target_obj(e);
            StatePtr st = FromRow(row);
            if (!st) return;
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            lv_obj_delete_async(row);
            AddChunk(st);
        },
        LV_EVENT_CLICKED, nullptr);
}

// 顶上：路径，不在最上面时「上一级」，选文件夹时「选这个文件夹」
void AddHeader(const StatePtr& st) {
    std::string path = "SD 卡";
    size_t pos = 0;
    while (pos < st->dir.size()) {
        size_t slash = st->dir.find('/', pos);
        if (slash == std::string::npos) slash = st->dir.size();
        path += " / " + st->dir.substr(pos, slash - pos);
        pos = slash + 1;
    }
    Note(st->body, path);
    if (st->dir != st->opt.top) {
        lv_obj_t* up = Row(st->body, 44);
        RowLabel(up, "上一级", C_MUTED);
        lv_obj_add_event_cb(
            up,
            [](lv_event_t* e) {
                if (StatePtr st = FromRow(lv_event_get_current_target_obj(e))) Load(st, Parent(st->dir));
            },
            LV_EVENT_CLICKED, nullptr);
    }
    if (st->opt.pick_dir) {
        lv_obj_t* pick = Row(st->body, 44);
        RowLabel(pick, "选这个文件夹", C_ACCENT);
        lv_obj_add_event_cb(
            pick,
            [](lv_event_t* e) {
                StatePtr st = FromRow(lv_event_get_current_target_obj(e));
                if (!st || !st->opt.on_pick) return;
                auto pick = st->opt.on_pick;
                pick(st->dir, true);
            },
            LV_EVENT_CLICKED, nullptr);
    }
}

void Load(const StatePtr& st, const std::string& dir) {
    st->dir = dir;
    uint32_t gen = ++st->gen;
    st->entries.clear();
    st->shown = 0;
    NewBody(st);
    AddHeader(st);
    Note(st->body, "正在读…");
    lv_obj_t* scroller = lv_obj_get_parent(st->list);  // 换了一层回到顶上
    if (scroller) lv_obj_scroll_to_y(scroller, 0, LV_ANIM_OFF);
    Display* display = NomiDisplay::Get();
    NomiFileList::RunInBackground([st, dir, gen, display] {
        std::vector<NomiFiles::Entry> entries;
        bool truncated = false;
        std::string err;
        {
            NomiSdCard::Use sd;
            err = sd ? NomiFiles::List(dir, entries, truncated) : "SD 卡没插";
        }
        if (err.empty() && !st->opt.exts.empty()) {
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                                         [&](const NomiFiles::Entry& e) { return !e.dir && !Matches(e.name, st->opt.exts); }),
                          entries.end());
        }
        DisplayLockGuard lock(display);
        if (!st->list || st->gen != gen) return;  // 列表没了，或者已经换到别的层
        st->entries = std::move(entries);
        NewBody(st);
        AddHeader(st);
        if (!err.empty()) {
            Note(st->body, err);
        } else if (st->entries.empty()) {
            Note(st->body, st->opt.exts.empty() ? "空的" : "这里没有能打开的文件");
        } else {
            if (truncated) Note(st->body, "太多了，只列了前 1000 个");
            AddChunk(st);
        }
    });
}

}  // namespace

namespace NomiFileList {

lv_obj_t* Create(lv_obj_t* parent, const std::string& dir, Options opt) {
    auto st = std::make_shared<State>();
    st->opt = std::move(opt);
    lv_obj_t* list = st->list = lv_obj_create(parent);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_remove_flag(list, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_user_data(list, new StatePtr(st));
    lv_obj_add_event_cb(
        list,
        [](lv_event_t* e) {
            auto* holder = static_cast<StatePtr*>(lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
            if (!holder) return;
            (*holder)->list = nullptr;
            (*holder)->body = nullptr;
            delete holder;
        },
        LV_EVENT_DELETE, nullptr);
    // 起点不在最上面以内就从最上面开始
    std::string start = dir;
    const std::string& top = st->opt.top;
    if (!top.empty() && start != top && start.rfind(top + "/", 0) != 0) start = top;
    Load(st, start);
    return list;
}

bool Up(lv_obj_t* list) {
    StatePtr st = Get(list);
    if (!st || st->dir == st->opt.top) return false;
    Load(st, Parent(st->dir));
    return true;
}

void Refresh(lv_obj_t* list) {
    if (StatePtr st = Get(list)) Load(st, st->dir);
}

std::string Dir(lv_obj_t* list) {
    StatePtr st = Get(list);
    return st ? st->dir : "";
}

void RunInBackground(std::function<void()> job) {
    static std::once_flag once;
    std::call_once(once, [] {
        s_wake = xSemaphoreCreateBinary();
        // 栈放 PSRAM：只碰 SD 卡，不碰内部闪存和 NVS
        esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
        cfg.stack_size = 6144;
        cfg.prio = 3;
        cfg.thread_name = "nomi_files";
        cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        esp_pthread_set_cfg(&cfg);
        pthread_t tid;
        if (pthread_create(&tid, nullptr, JobThread, nullptr) == 0) pthread_detach(tid);
        else ESP_LOGE(TAG, "Failed to create file thread");
        cfg = esp_pthread_get_default_config();
        esp_pthread_set_cfg(&cfg);
    });
    {
        std::lock_guard<std::mutex> lock(s_jobs_mutex);
        s_jobs.push_back(std::move(job));
    }
    xSemaphoreGive(s_wake);
}

std::string SizeText(uint64_t b) {
    char buf[24];
    if (b < 1024) snprintf(buf, sizeof(buf), "%u B", static_cast<unsigned>(b));
    else if (b < 1024 * 1024) snprintf(buf, sizeof(buf), "%u KB", static_cast<unsigned>((b + 512) / 1024));
    else if (b < 1024ull * 1024 * 1024) snprintf(buf, sizeof(buf), "%.1f MB", b / 1048576.0);
    else snprintf(buf, sizeof(buf), "%.2f GB", b / 1073741824.0);
    return buf;
}

}  // namespace NomiFileList
