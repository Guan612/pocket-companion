#include "nomi_sheet.h"

#include "nomi_font.h"
#include "nomi_style.h"

namespace {

lv_obj_t* s_box;
std::function<void()> s_on_close;

}  // namespace

namespace NomiSheet {

lv_obj_t* Open(const std::string& title, const std::string& sub, std::function<void()> on_close) {
    Close();
    s_on_close = std::move(on_close);
    lv_obj_t* box = s_box = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_60, 0);
    lv_obj_set_style_text_font(box, NomiFont::Base(), 0);  // 最上层不跟着屏幕继承字体
    lv_obj_set_style_text_color(box, lv_color_hex(C_TEXT), 0);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(
        box,
        [](lv_event_t* e) {
            if (lv_event_get_target(e) == lv_event_get_current_target(e)) Close();  // 点了外面
        },
        LV_EVENT_CLICKED, nullptr);

    lv_obj_t* panel = lv_obj_create(box);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, 400);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(panel, 300, 0);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_pad_all(panel, 14, 0);
    lv_obj_set_style_pad_row(panel, 6, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);  // 点面板空白处不算点外面
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);

    lv_obj_t* t = lv_label_create(panel);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(t, title.c_str());
    if (!sub.empty()) {
        lv_obj_t* s = lv_label_create(panel);
        lv_obj_set_width(s, LV_PCT(100));
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_color(s, lv_color_hex(C_MUTED), 0);
        lv_label_set_text(s, sub.c_str());
    }
    return panel;
}

lv_obj_t* AddRow(lv_obj_t* panel) {
    lv_obj_t* row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), 44);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

lv_obj_t* AddButton(lv_obj_t* panel, const std::string& text, uint32_t color, lv_event_cb_t cb, void* user) {
    lv_obj_t* row = AddRow(panel);
    lv_obj_set_height(row, 48);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_CONTROL), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_DIMMED), LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_hor(row, 14, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* label = lv_label_create(row);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text.c_str());
    if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user);
    return row;
}

void Close() {
    if (!s_box) return;
    lv_obj_delete_async(s_box);  // 往往是它自己里面的按钮触发的
    s_box = nullptr;
    auto cb = std::move(s_on_close);
    s_on_close = nullptr;
    if (cb) cb();
}

}  // namespace NomiSheet
