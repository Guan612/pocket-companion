#include "nomi_canvas.h"

#include <algorithm>
#include <set>
#include <vector>

LV_FONT_DECLARE(nomi_font_clock_120);  // 时钟的大数字（只有 0-9 和冒号）；别放进匿名命名空间，链接找不到

namespace {

constexpr size_t kMaxPrims = 800;    // 一份清单最多几样（防应用画个没完；城里一屏的路几百条）
constexpr size_t kMaxPoints = 1000;  // 一条折线、一个多边形最多几个点

enum Kind { kLine, kFill, kCircle, kText };

struct Prim {
    Kind kind;
    uint32_t color;
    int width = 0;  // 线宽；圆：0 = 实心
    int size = 0, align = 0;
    std::vector<lv_point_precise_t> points;  // 圆：points[0] 是圆心
    int r = 0;
    std::string text;
};

struct Canvas {
    std::vector<Prim> pending, shown;
};

std::set<lv_obj_t*> s_canvases;

Canvas* Get(lv_obj_t* obj) {
    if (!obj || !s_canvases.count(obj)) return nullptr;
    return static_cast<Canvas*>(lv_obj_get_user_data(obj));
}

lv_color_t Color(uint32_t rgb) { return lv_color_hex(rgb & 0xffffff); }

const lv_font_t* FontOf(lv_obj_t* obj, int size) {
    return size == NomiCanvas::kBig ? &nomi_font_clock_120 : lv_obj_get_style_text_font(obj, LV_PART_MAIN);
}

void Draw(lv_event_t* e) {
    lv_obj_t* obj = lv_event_get_current_target_obj(e);
    Canvas* c = Get(obj);
    if (!c) return;
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t area;
    lv_obj_get_content_coords(obj, &area);
    const int ox = area.x1, oy = area.y1;
    for (const Prim& p : c->shown) {
        switch (p.kind) {
        case kLine: {
            lv_draw_line_dsc_t dsc;
            lv_draw_line_dsc_init(&dsc);
            dsc.color = Color(p.color);
            dsc.width = p.width;
            dsc.round_start = 1;
            dsc.round_end = 1;
            for (size_t i = 1; i < p.points.size(); i++) {
                dsc.p1 = {p.points[i - 1].x + ox, p.points[i - 1].y + oy};
                dsc.p2 = {p.points[i].x + ox, p.points[i].y + oy};
                lv_draw_line(layer, &dsc);
            }
            break;
        }
        case kFill: {
            // 凸多边形：从第一个点扇形切成三角形
            lv_draw_triangle_dsc_t dsc;
            lv_draw_triangle_dsc_init(&dsc);
            dsc.color = Color(p.color);
            dsc.opa = LV_OPA_COVER;
            for (size_t i = 2; i < p.points.size(); i++) {
                dsc.p[0] = {p.points[0].x + ox, p.points[0].y + oy};
                dsc.p[1] = {p.points[i - 1].x + ox, p.points[i - 1].y + oy};
                dsc.p[2] = {p.points[i].x + ox, p.points[i].y + oy};
                lv_draw_triangle(layer, &dsc);
            }
            break;
        }
        case kCircle: {
            const int cx = p.points[0].x + ox, cy = p.points[0].y + oy;
            if (p.width <= 0) {
                lv_draw_rect_dsc_t dsc;
                lv_draw_rect_dsc_init(&dsc);
                dsc.radius = LV_RADIUS_CIRCLE;
                dsc.bg_color = Color(p.color);
                dsc.bg_opa = LV_OPA_COVER;
                lv_area_t a = {cx - p.r, cy - p.r, cx + p.r, cy + p.r};
                lv_draw_rect(layer, &dsc, &a);
            } else {
                lv_draw_arc_dsc_t dsc;
                lv_draw_arc_dsc_init(&dsc);
                dsc.color = Color(p.color);
                dsc.width = p.width;
                dsc.center = {cx, cy};
                dsc.radius = static_cast<uint16_t>(p.r);
                dsc.start_angle = 0;
                dsc.end_angle = 360;
                lv_draw_arc(layer, &dsc);
            }
            break;
        }
        case kText: {
            const lv_font_t* font = FontOf(obj, p.size);
            lv_point_t size;
            lv_text_get_size(&size, p.text.c_str(), font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            int x = p.points[0].x + ox;
            if (p.align == NomiCanvas::kCenter) x -= size.x / 2;
            else if (p.align == NomiCanvas::kRight) x -= size.x;
            const int y = p.points[0].y + oy;
            lv_draw_label_dsc_t dsc;
            lv_draw_label_dsc_init(&dsc);
            dsc.text = p.text.c_str();
            dsc.font = font;
            dsc.color = Color(p.color);
            lv_area_t a = {x, y, x + size.x, y + size.y};
            lv_draw_label(layer, &dsc, &a);
            break;
        }
        }
    }
}

Prim* Add(lv_obj_t* obj, Kind kind, uint32_t color) {
    Canvas* c = Get(obj);
    if (!c || c->pending.size() >= kMaxPrims) return nullptr;
    c->pending.push_back(Prim{});
    Prim* p = &c->pending.back();
    p->kind = kind;
    p->color = color;
    return p;
}

void Points(Prim* p, const int32_t* xy, int n) {
    n = std::clamp(n, 0, static_cast<int>(kMaxPoints));
    p->points.reserve(n);
    for (int i = 0; i < n; i++) p->points.push_back({xy[i * 2], xy[i * 2 + 1]});
}

}  // namespace

namespace NomiCanvas {

lv_obj_t* Create(lv_obj_t* parent, int height) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    if (height <= 0) {  // 占满：父容器（一页）的留白去掉、不让滚
        lv_obj_set_style_pad_all(parent, 0, 0);
        lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(obj, LV_PCT(100), LV_PCT(100));
    } else {
        lv_obj_set_size(obj, LV_PCT(100), height);
    }
    lv_obj_remove_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_user_data(obj, new Canvas());
    s_canvases.insert(obj);
    lv_obj_add_event_cb(obj, Draw, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_event_cb(
        obj,
        [](lv_event_t* e) {
            lv_obj_t* obj = lv_event_get_current_target_obj(e);
            delete static_cast<Canvas*>(lv_obj_get_user_data(obj));
            lv_obj_set_user_data(obj, nullptr);
            s_canvases.erase(obj);
        },
        LV_EVENT_DELETE, nullptr);
    return obj;
}

bool Is(lv_obj_t* obj) { return Get(obj) != nullptr; }

void Clear(lv_obj_t* canvas) {
    if (Canvas* c = Get(canvas)) c->pending.clear();
}

void Line(lv_obj_t* canvas, const int32_t* xy, int points, int width, uint32_t color) {
    if (points < 2) return;
    if (Prim* p = Add(canvas, kLine, color)) {
        p->width = std::clamp(width, 1, 64);
        Points(p, xy, points);
    }
}

void Fill(lv_obj_t* canvas, const int32_t* xy, int points, uint32_t color) {
    if (points < 3) return;
    if (Prim* p = Add(canvas, kFill, color)) Points(p, xy, points);
}

void Circle(lv_obj_t* canvas, int x, int y, int r, int width, uint32_t color) {
    if (r <= 0) return;
    if (Prim* p = Add(canvas, kCircle, color)) {
        p->points.push_back({x, y});
        p->r = std::min(r, 2000);
        p->width = std::clamp(width, 0, 64);
    }
}

int Text(lv_obj_t* canvas, int x, int y, const std::string& text, int size, uint32_t color, int align) {
    Prim* p = Add(canvas, kText, color);
    if (!p) return 0;
    p->points.push_back({x, y});
    p->text = text;
    p->size = size == kBig ? kBig : kNormal;
    p->align = std::clamp(align, 0, 2);
    lv_point_t s;
    lv_text_get_size(&s, text.c_str(), FontOf(canvas, p->size), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return s.x;
}

void Show(lv_obj_t* canvas) {
    Canvas* c = Get(canvas);
    if (!c) return;
    c->shown.swap(c->pending);
    c->pending.clear();
    lv_obj_invalidate(canvas);
}

int Width(lv_obj_t* canvas) {
    lv_obj_update_layout(canvas);
    return lv_obj_get_content_width(canvas);
}

int Height(lv_obj_t* canvas) {
    lv_obj_update_layout(canvas);
    return lv_obj_get_content_height(canvas);
}

}  // namespace NomiCanvas
