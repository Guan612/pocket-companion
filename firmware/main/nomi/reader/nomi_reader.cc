#include "nomi_reader.h"

#include "nomi_book.h"
#include "nomi_font.h"
#include "nomi_sdcard.h"
#include "nomi_style.h"
#include "nomi_ui.h"
#include "src/misc/lv_text_private.h"

#include <esp_log.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>

#define TAG "NomiReader"

namespace {

constexpr int kPadX = 32;      // 左右留白（左边 30 像素是返回手势的边缘条，字可以在它下面）
constexpr int kPadTop = 14;
constexpr int kFooterH = 30;   // 底下一行：左边章名（应用给），右边百分比
constexpr int kLineSpace = 6;
constexpr int kSwipeMin = 40;  // 上下划过这么多算翻页
constexpr int kTapMax = 16;    // 动得比这少算点
constexpr uint32_t kWindow = 4096;  // 排一页先读这么多（一页一两百个字），不够再加
constexpr size_t kMaxBack = 64;     // 往回翻能原样退几页

struct Reader {
    std::unique_ptr<NomiSdCard::Use> sd;
    NomiBook book;
    const lv_font_t* font = nullptr;
    lv_obj_t *box = nullptr, *status = nullptr, *page = nullptr, *percent = nullptr;
    int flags = NomiReader::kSwipe | NomiReader::kTap;
    NomiReader::Callback cb;
    uint32_t start = 0, end = 0;  // 这一页在文件里的范围
    std::vector<uint32_t> back;   // 往后翻时记下的上一页开头
    std::string text;             // 这一页的字（行之间是 \n）
    int32_t width = 0;
    int lines = 1;
    lv_point_t press = {0, 0};
    bool long_pressed = false;
};

std::vector<lv_obj_t*> s_readers;

Reader* Get(lv_obj_t* obj) {
    return NomiReader::Is(obj) ? static_cast<Reader*>(lv_obj_get_user_data(obj)) : nullptr;
}

lv_text_attributes_t Attr(const Reader* r) {
    lv_text_attributes_t a;
    lv_text_attributes_init(&a);
    a.max_width = r->width;
    a.line_space = kLineSpace;
    a.letter_space = 0;
    a.text_flags = LV_TEXT_FLAG_NONE;
    return a;
}

// 从 start 往后排一页：page 是排好的字，next 是下一页开头
bool LayoutForward(Reader* r, uint32_t start, std::string& page, uint32_t& next) {
    std::string t;
    std::vector<uint32_t> src;
    const uint32_t size = r->book.size();
    for (uint32_t window = kWindow;; window *= 4) {
        uint32_t to = size - start > window ? start + window : size;
        if (!r->book.Decode(start, to, t, src)) return false;
        bool eof = to >= size;
        lv_text_attributes_t attr = Attr(r);
        page.clear();
        size_t pos = 0;
        int n_lines = 0;
        while (n_lines < r->lines && pos < t.size()) {
            uint32_t n = lv_text_get_next_line(t.c_str() + pos, t.size() - pos, r->font, nullptr, &attr);
            if (n == 0) break;
            size_t vis = pos + n;
            if (vis > pos && t[vis - 1] == '\n') vis--;
            if (n_lines) page += '\n';
            page.append(t, pos, vis - pos);
            pos += n;
            n_lines++;
        }
        // 一页排满、而且最后一行不是被读的窗口截断的；或者到了书的结尾
        bool done = eof || (n_lines >= r->lines && pos < t.size());
        if (done || window >= 256 * 1024) {
            next = pos < t.size() ? src[pos] : (eof ? size : src.back());
            if (next <= start) next = std::min(size, start + 1);  // 怎么也要往前走
            return true;
        }
    }
}

// 上一页：从前面的段落开头排到 end（这页开头），取最后一整页
bool LayoutBackward(Reader* r, uint32_t end, uint32_t& start) {
    std::string t;
    std::vector<uint32_t> src;
    for (uint32_t back = 3072;; back *= 2) {
        uint32_t from = r->book.ParagraphStartBefore(end, back);
        if (!r->book.Decode(from, end, t, src)) return false;
        lv_text_attributes_t attr = Attr(r);
        std::vector<size_t> starts;
        size_t pos = 0;
        while (pos < t.size()) {
            starts.push_back(pos);
            uint32_t n = lv_text_get_next_line(t.c_str() + pos, t.size() - pos, r->font, nullptr, &attr);
            if (n == 0) break;
            pos += n;
        }
        if (starts.size() >= static_cast<size_t>(r->lines) || from <= r->book.body() || back >= 128 * 1024) {
            if (starts.empty()) {
                start = from;
            } else {
                size_t k = starts.size() > static_cast<size_t>(r->lines) ? starts.size() - r->lines : 0;
                start = src[starts[k]];
            }
            return true;
        }
    }
}

void Show(Reader* r, uint32_t start) {
    std::string page;
    uint32_t next;
    if (!LayoutForward(r, start, page, next)) {
        nomi_ui_notify("读不了这本书（SD 卡拔了？）", 2500);
        return;
    }
    r->start = start;
    r->end = next;
    r->text = page;
    lv_label_set_text(r->page, page.c_str());
    uint32_t total = r->book.size() - r->book.body();
    int pct = next >= r->book.size() || total == 0
                  ? 100
                  : static_cast<int>(static_cast<uint64_t>(start - r->book.body()) * 100 / total);
    lv_label_set_text_fmt(r->percent, "%d%%", pct);
    if (r->cb) r->cb(NomiReader::kPage, start);
}

void Next(Reader* r) {
    if (r->end >= r->book.size()) {
        nomi_ui_notify("已经是最后一页", 1500);
        if (r->cb) r->cb(NomiReader::kEnd, r->start);
        return;
    }
    if (r->back.size() >= kMaxBack) r->back.erase(r->back.begin());
    r->back.push_back(r->start);
    Show(r, r->end);
}

void Prev(Reader* r) {
    if (r->start <= r->book.body()) {
        nomi_ui_notify("已经是第一页", 1500);
        return;
    }
    uint32_t s;
    if (!r->back.empty()) {
        s = r->back.back();
        r->back.pop_back();
    } else if (!LayoutBackward(r, r->start, s)) {
        return;
    }
    Show(r, s);
}

// 书签、目录给的位置：对齐到字的开头；超出结尾就显示最后一页
void ShowAt(Reader* r, uint32_t offset) {
    if (offset >= r->book.size()) {
        uint32_t s;
        if (!LayoutBackward(r, r->book.size(), s)) s = r->book.body();
        Show(r, s);
        return;
    }
    Show(r, r->book.AlignChar(std::max(offset, r->book.body())));
}

void OnEvent(lv_event_t* e) {
    auto* r = static_cast<Reader*>(lv_event_get_user_data(e));
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        s_readers.erase(std::remove(s_readers.begin(), s_readers.end(), r->box), s_readers.end());
        NomiFont::ReleaseReading();
        delete r;
        return;
    }
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_LONG_PRESSED && code != LV_EVENT_RELEASED) return;
    lv_point_t p = r->press;
    if (lv_indev_t* indev = lv_indev_active()) lv_indev_get_point(indev, &p);
    int dx = p.x - r->press.x, dy = p.y - r->press.y;
    switch (code) {
    case LV_EVENT_PRESSED:
        r->press = p;
        r->long_pressed = false;
        break;
    case LV_EVENT_LONG_PRESSED:
        if (std::abs(dx) < kTapMax && std::abs(dy) < kTapMax) {  // 慢慢划的不算长按
            r->long_pressed = true;
            if (r->cb) r->cb(NomiReader::kMenu, r->start);
        }
        break;
    case LV_EVENT_RELEASED:
        if (r->long_pressed) break;
        if ((r->flags & NomiReader::kSwipe) && std::abs(dy) >= kSwipeMin && std::abs(dy) > std::abs(dx)) {
            if (dy < 0) Next(r);
            else Prev(r);
        } else if (std::abs(dx) < kTapMax && std::abs(dy) < kTapMax) {
            // 点：分三段，中间那段要菜单；两头按设置翻页（上下或左右）
            lv_area_t a;
            lv_obj_get_coords(r->box, &a);
            bool lr = r->flags & NomiReader::kTapLr;
            int32_t len = lr ? a.x2 - a.x1 + 1 : a.y2 - a.y1 + 1, at = lr ? p.x - a.x1 : p.y - a.y1;
            if (at >= len / 3 && at < len * 2 / 3) {
                if (r->cb) r->cb(NomiReader::kMenu, r->start);
            } else if (r->flags & (NomiReader::kTap | NomiReader::kTapLr)) {
                if (at < len / 3) Prev(r);
                else Next(r);
            }
        }
        break;
    default:
        break;
    }
}

lv_obj_t* Label(lv_obj_t* parent, const lv_font_t* font, uint32_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

}  // namespace

namespace NomiReader {

lv_obj_t* Create(lv_obj_t* parent, const std::string& path, uint32_t offset, int flags, Callback cb,
                 std::string& error) {
    auto r = std::make_unique<Reader>();
    if (path.rfind("/sd/", 0) == 0) {
        r->sd = std::make_unique<NomiSdCard::Use>();
        if (!*r->sd) {
            error = "SD 卡没插";
            return nullptr;
        }
    }
    if (!r->book.Open(path)) {
        error = "打不开这本书（文件没了，或者是不认识的编码）";
        return nullptr;
    }
    r->font = NomiFont::AcquireReading();
    r->flags = flags & (kSwipe | kTap | kTapLr) ? flags & (kSwipe | kTap | kTapLr) : kSwipe | kTap;
    r->cb = std::move(cb);

    lv_obj_update_layout(parent);
    int32_t w = lv_obj_get_content_width(parent), h = lv_obj_get_content_height(parent);
    if (w < 200 || h < 120) {
        w = 480;
        h = 320;
    }
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_add_flag(box, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(box, 0, 0);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    r->box = box;
    r->width = w - 2 * kPadX;
    r->lines = std::max(1, static_cast<int>((h - kPadTop - kFooterH + kLineSpace) / (r->font->line_height + kLineSpace)));

    // 左下角那行最先建：ui_set_text(阅读器) 改的是它（运行时找第一个标签）
    r->status = Label(box, r->font, C_MUTED);
    lv_label_set_long_mode(r->status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(r->status, r->width - 90);
    lv_obj_set_pos(r->status, kPadX, h - kFooterH + 2);
    r->page = Label(box, r->font, C_TEXT);
    lv_obj_set_style_text_line_space(r->page, kLineSpace, 0);
    lv_label_set_long_mode(r->page, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_width(r->page, r->width + 4);  // 行都排好了，宽一点免得标签自己再断一次
    lv_obj_set_pos(r->page, kPadX, kPadTop);
    r->percent = Label(box, r->font, C_MUTED);
    lv_obj_set_width(r->percent, 80);
    lv_obj_set_style_text_align(r->percent, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(r->percent, w - kPadX - 80, h - kFooterH + 2);

    Reader* raw = r.release();
    lv_obj_set_user_data(box, raw);
    lv_obj_add_event_cb(box, OnEvent, LV_EVENT_ALL, raw);
    s_readers.push_back(box);
    ESP_LOGI(TAG, "Open %s: %u bytes, %s, %d lines x %d px", path.c_str(), static_cast<unsigned>(raw->book.size()),
             raw->book.encoding() == NomiBook::kUtf8 ? "UTF-8" : "GBK", raw->lines, static_cast<int>(raw->width));
    ShowAt(raw, offset);
    return box;
}

bool Is(lv_obj_t* obj) { return obj && std::find(s_readers.begin(), s_readers.end(), obj) != s_readers.end(); }

void Goto(lv_obj_t* reader, uint32_t offset) {
    if (Reader* r = Get(reader)) {
        r->back.clear();
        ShowAt(r, offset < r->book.size() ? r->book.ParagraphStartBefore(offset, 0) : offset);
    }
}

void SetFlags(lv_obj_t* reader, int flags) {
    if (Reader* r = Get(reader)) {
        r->flags = flags & (kSwipe | kTap | kTapLr) ? flags & (kSwipe | kTap | kTapLr) : kSwipe | kTap;
    }
}

std::string PageText(lv_obj_t* reader) {
    Reader* r = Get(reader);
    return r ? r->text : "";
}

}  // namespace NomiReader
