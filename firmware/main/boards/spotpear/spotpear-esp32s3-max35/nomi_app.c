#include "nomi_app.h"

#include <stdio.h>
#include <string.h>

#include "face.h"
#include "nomi_style.h"

// 左边缘多宽算"从边上划"（照 Wear OS：能拖动的界面只认边上开始的右划）。触摸 30ms 才读一次，
// 手指一落下就往右划时，第一次读到的点可能已经离边二三十像素，太窄了接不住
#define EDGE_W 30
#define BACK_DISTANCE 100  // 拖过这么远松手就返回
#define SLIDE_MS 180
// 桌面：一行四格。图标是深色圆角方块里的白色细线图，下面写名字
#define GRID_COLS 4
#define CELL_W 108
#define CELL_H 124
#define TILE 84     // 图标方块边长
#define ICON_PX 48  // 方块里线条图标的大小（图标按 36 的格子画，放大到这么大）
#define ICON_UNIT 36
#define MAX_APPS 24

static lv_obj_t *s_layer;  // 应用层：全屏，没开应用（或睡着）时藏着
static lv_obj_t *s_root;   // 开着的应用的界面
static const nomi_app_t *s_app;
static bool s_hidden;
static lv_obj_t *s_grid;  // 桌面
static const nomi_app_t *s_apps[MAX_APPS + 1];
static int s_app_count;
static const lv_font_t *s_font;
static void (*s_activity)(void);
static void (*s_go_home)(void);
static void (*s_toast)(const char *text);
static nomi_appmgr_hooks_t s_hooks;
static int32_t s_drag_x0, s_drag_dx;
static bool s_indev_hooked;

static void activity(void)
{
    if (s_activity) s_activity();
}

static void toast(const char *text)
{
    if (s_toast) s_toast(text);
}

/* ---------- 线条图标 ---------- */

static void free_points(lv_event_t *e) { lv_free(lv_event_get_user_data(e)); }

void nomi_icon_draw(lv_obj_t *parent, const nomi_icon_t *icon, int px)
{
    if (!icon) return;
    for (int i = 0; i < icon->line_count; i++) {
        const nomi_icon_line_t *l = &icon->lines[i];
        lv_point_precise_t *pts = lv_malloc(l->count * sizeof(*pts));  // 线要一直引用这块点，删线时一起放
        for (uint16_t k = 0; k < l->count; k++) {
            pts[k].x = l->points[k].x * px / ICON_UNIT;
            pts[k].y = l->points[k].y * px / ICON_UNIT;
        }
        lv_obj_t *line = lv_line_create(parent);
        lv_line_set_points_mutable(line, pts, l->count);
        lv_obj_add_event_cb(line, free_points, LV_EVENT_DELETE, pts);
        lv_obj_set_style_line_width(line, 2, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_set_style_line_color(line, lv_color_hex(C_TEXT), 0);
        lv_obj_set_pos(line, 0, 0);
        lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }
    for (int i = 0; i < icon->circle_count; i++) {
        const nomi_icon_circle_t *c = &icon->circles[i];
        lv_obj_t *o = lv_obj_create(parent);
        lv_obj_remove_style_all(o);
        lv_obj_set_size(o, c->d * px / ICON_UNIT, c->d * px / ICON_UNIT);
        lv_obj_set_pos(o, c->x * px / ICON_UNIT, c->y * px / ICON_UNIT);
        lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(o, 2, 0);
        lv_obj_set_style_border_color(o, lv_color_hex(C_TEXT), 0);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void icon_set_color(lv_obj_t *icon, uint32_t color)
{
    for (uint32_t i = 0; i < lv_obj_get_child_count(icon); i++) {
        lv_obj_t *c = lv_obj_get_child(icon, i);
        if (lv_obj_check_type(c, &lv_line_class)) lv_obj_set_style_line_color(c, lv_color_hex(color), 0);
        else lv_obj_set_style_border_color(c, lv_color_hex(color), 0);
    }
}


static const lv_point_precise_t kMapTip[] = {{11, 16}, {18, 32}, {25, 16}};
static const nomi_icon_line_t kMapLines[] = {{kMapTip, 3}};
static const nomi_icon_circle_t kMapCircles[] = {{9, 3, 18}, {15, 9, 6}};
static const nomi_icon_t kMapIcon = {kMapLines, 1, kMapCircles, 2};

// 应用管理：托盘上一个往下的箭头（下载安装）
static const lv_point_precise_t kMgrShaft[] = {{18, 4}, {18, 22}};
static const lv_point_precise_t kMgrHead[] = {{11, 15}, {18, 22}, {25, 15}};
static const lv_point_precise_t kMgrTray[] = {{5, 22}, {5, 31}, {31, 31}, {31, 22}};
static const nomi_icon_line_t kMgrLines[] = {{kMgrShaft, 2}, {kMgrHead, 3}, {kMgrTray, 4}};
static const nomi_icon_t kAppMgrIcon = {kMgrLines, 3, NULL, 0};

/* ---------- 占位应用：框架先跑通，内容后面一个个做成应用包 ---------- */

static void placeholder_open(const nomi_app_t *app, lv_obj_t *root)
{
    lv_obj_t *list = lv_obj_create(root);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(list, 32, 0);
    lv_obj_set_style_pad_ver(list, 20, 0);
    lv_obj_set_style_pad_row(list, 12, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *title = lv_label_create(list);
    lv_label_set_text(title, app->name);
    lv_obj_t *hint = lv_label_create(list);
    lv_obj_set_width(hint, LV_PCT(100));
    lv_obj_set_style_text_color(hint, lv_color_hex(C_MUTED), 0);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "%s还在做。先拿这页试交互：往下滚一点，喊「糯米糯米」说句话，说完应该还停在这里；"
             "从屏幕左边缘往右划，回到桌面。",
             (const char *)app->user);
    lv_label_set_text(hint, buf);
    for (int i = 1; i <= 30; i++) {
        lv_obj_t *row = lv_label_create(list);
        snprintf(buf, sizeof(buf), "第 %d 行", i);
        lv_label_set_text(row, buf);
    }
}

static const nomi_app_t kMap = {"map", "地图", &kMapIcon, placeholder_open, NULL, NULL, NULL, "地图"};

/* ---------- 系统应用「应用管理」：看装了哪些、删掉不要的 ---------- */

static lv_obj_t *s_mgr_root, *s_mgr_list;
static lv_obj_t *s_armed;  // 点过一次、等再点一次确认的删除按钮
static void appmgr_build(void);

static void on_uninstall(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const nomi_app_t *app = (const nomi_app_t *)lv_event_get_user_data(e);
    activity();
    if (s_armed != btn) {  // 第一下：变成"确认删除"，再点一下才删
        if (s_armed) lv_label_set_text(lv_obj_get_child(s_armed, 0), "删除");
        s_armed = btn;
        lv_label_set_text(lv_obj_get_child(btn, 0), "确认删除");
        return;
    }
    s_armed = NULL;
    if (s_hooks.uninstall) s_hooks.uninstall(app);  // 里面会 nomi_apps_remove
    appmgr_build();
}

static void on_clear_defaults(lv_event_t *e)
{
    activity();
    if (s_hooks.clear_defaults) s_hooks.clear_defaults((const nomi_app_t *)lv_event_get_user_data(e));
    appmgr_build();
}

static lv_obj_t *appmgr_button(lv_obj_t *row, const char *text, uint32_t color)
{
    lv_obj_t *btn = lv_button_create(row);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_CONTROL), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return btn;
}

// 建（重建）应用管理的列表。旧列表异步删：重建往往是它自己里面的按钮触发的
static void appmgr_build(void)
{
    if (!s_mgr_root) return;
    s_armed = NULL;
    if (s_mgr_list) lv_obj_delete_async(s_mgr_list);
    lv_obj_t *list = s_mgr_list = lv_obj_create(s_mgr_root);
    lv_obj_move_to_index(list, 0);  // 压在左边缘拖动条下面
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(list, 40, 0);
    lv_obj_set_style_pad_ver(list, 20, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *title = lv_label_create(list);
    lv_label_set_text(title, "应用管理");
    if (s_hooks.summary) {
        lv_obj_t *sum = lv_label_create(list);
        lv_obj_set_width(sum, LV_PCT(100));
        lv_obj_set_style_text_color(sum, lv_color_hex(C_MUTED), 0);
        lv_label_set_text(sum, s_hooks.summary());
    }
    for (int i = 0; i < s_app_count; i++) {
        const nomi_app_t *app = s_apps[i];
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 64);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 16, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *icon = lv_obj_create(row);
        lv_obj_remove_style_all(icon);
        lv_obj_set_size(icon, ICON_PX, ICON_PX);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        nomi_icon_draw(icon, app->icon, ICON_PX);
        // 名字，下面一行小字：是哪些文件的默认打开方式
        lv_obj_t *info = lv_obj_create(row);
        lv_obj_remove_style_all(info);
        lv_obj_set_height(info, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(info, 1);
        lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(info, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *name = lv_label_create(info);
        lv_obj_set_width(name, LV_PCT(100));
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(name, app->name);
        const char *defaults = s_hooks.defaults ? s_hooks.defaults(app) : NULL;
        if (defaults) {
            lv_obj_t *sub = lv_label_create(info);
            lv_obj_set_width(sub, LV_PCT(100));
            lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_style_text_color(sub, lv_color_hex(C_MUTED), 0);
            lv_label_set_text(sub, defaults);
        }
        if (app->dimmed) {  // 和桌面上一样灰着
            icon_set_color(icon, C_DIMMED);
            lv_obj_set_style_text_color(name, lv_color_hex(C_DIMMED), 0);
        }
        if (defaults && s_hooks.clear_defaults) {
            lv_obj_add_event_cb(appmgr_button(row, "清除默认", C_TEXT), on_clear_defaults, LV_EVENT_CLICKED,
                                (void *)app);
        }
        if (s_hooks.can_uninstall && s_hooks.can_uninstall(app)) {
            lv_obj_add_event_cb(appmgr_button(row, "删除", C_DANGER), on_uninstall, LV_EVENT_CLICKED, (void *)app);
        } else {
            lv_obj_t *tag = lv_label_create(row);
            lv_obj_set_style_text_color(tag, lv_color_hex(C_MUTED), 0);
            lv_label_set_text(tag, "内置");
        }
    }
}

static void appmgr_open(const nomi_app_t *app, lv_obj_t *root)
{
    (void)app;
    s_mgr_root = root;
    s_mgr_list = NULL;
    appmgr_build();
}

static void appmgr_close(const nomi_app_t *app)
{
    (void)app;
    s_mgr_root = s_mgr_list = s_armed = NULL;
}

static const nomi_app_t kAppMgr = {"appmgr", "应用管理", &kAppMgrIcon, appmgr_open, appmgr_close, NULL, NULL, NULL};

void nomi_appmgr_set_hooks(const nomi_appmgr_hooks_t *hooks)
{
    if (hooks) s_hooks = *hooks;
}

/* ---------- 打开、关掉、左边缘右划返回 ---------- */

static void set_root_x(void *obj, int32_t x) { lv_obj_set_x((lv_obj_t *)obj, x); }

static void slide_root(int32_t to, lv_anim_completed_cb_t done)
{
    lv_anim_delete(s_root, set_root_x);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_root);
    lv_anim_set_exec_cb(&a, set_root_x);
    lv_anim_set_values(&a, lv_obj_get_x(s_root), to);
    lv_anim_set_duration(&a, SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

// 关掉当前应用（界面删掉、藏起应用层）。async：在这个应用自己的事件里关时要等事件处理完再删
static void close_app(bool async)
{
    if (!s_app) return;
    const nomi_app_t *app = s_app;
    s_app = NULL;
    if (app->close) app->close(app);
    if (s_root) {
        lv_anim_delete(s_root, set_root_x);
        if (async) lv_obj_delete_async(s_root);
        else lv_obj_delete(s_root);
        s_root = NULL;
    }
    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
}

static void on_slid_out(lv_anim_t *a)
{
    (void)a;
    close_app(true);
}

// 应用里的左边缘：按住往右拖，整个应用跟着手走；松手时拖够了就返回，不够就弹回去
static void on_edge(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (!indev || !s_root) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_PRESSED) {
        s_drag_x0 = p.x;
        s_drag_dx = 0;
        lv_anim_delete(s_root, set_root_x);
        activity();
    } else if (code == LV_EVENT_PRESSING) {
        s_drag_dx = p.x > s_drag_x0 ? p.x - s_drag_x0 : 0;
        lv_obj_set_x(s_root, s_drag_dx);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_drag_dx < BACK_DISTANCE) {
            slide_root(0, NULL);
        } else if (s_app->back && s_app->back(s_app)) {
            slide_root(0, NULL);  // 应用自己退了一层（比如回上一级文件夹）
        } else {
            slide_root(FACE_W, on_slid_out);
        }
        s_drag_dx = 0;
    }
}

// 左边缘的拖动条：不让它把拖动交给外层滚动，按住后手指离开它也一直算按着
static lv_obj_t *add_edge(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *edge = lv_obj_create(parent);
    lv_obj_remove_style_all(edge);
    lv_obj_set_size(edge, EDGE_W, FACE_H);
    lv_obj_set_pos(edge, 0, 0);
    lv_obj_add_flag(edge, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(edge, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(edge, cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(edge, cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(edge, cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(edge, cb, LV_EVENT_PRESS_LOST, NULL);
    return edge;
}

// 应用里任何地方有触摸都算有人在用（应用里的控件各管各的事件，挂在触摸设备上最省事）
static void on_indev_pressed(lv_event_t *e)
{
    (void)e;
    if (s_app && !s_hidden) activity();
}

static const nomi_app_t *find_app(const char *id)
{
    if (strcmp(id, kAppMgr.id) == 0) return &kAppMgr;
    for (int i = 0; i < s_app_count; i++) {
        if (strcmp(s_apps[i]->id, id) == 0) return s_apps[i];
    }
    return NULL;
}

bool nomi_app_open(const char *id)
{
    const nomi_app_t *app = find_app(id);
    if (!app) return false;
    if (app == s_app) return true;
    if (app->dimmed) {
        toast(app->dimmed_hint ? app->dimmed_hint : "现在打不开");
        if (s_hooks.dimmed_tap) s_hooks.dimmed_tap(app);
        return false;
    }
    close_app(false);
    if (!s_indev_hooked) {
        lv_indev_t *indev = lv_indev_get_next(NULL);  // 触摸在界面建好之后才注册，这里第一次开应用时再挂
        if (indev) {
            lv_indev_add_event_cb(indev, on_indev_pressed, LV_EVENT_PRESSED, NULL);
            s_indev_hooked = true;
        }
    }

    s_root = lv_obj_create(s_layer);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, FACE_W, FACE_H);
    lv_obj_set_style_bg_color(s_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_root, lv_color_hex(C_TEXT), 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);  // 挡住下面的卡片
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    s_app = app;
    app->open(app, s_root);
    // 左边缘的拖动条最后建，压在应用内容上面；应用以后再加控件要用 lv_obj_move_to_index 放到它下面
    add_edge(s_root, on_edge);

    lv_obj_set_flag(s_layer, LV_OBJ_FLAG_HIDDEN, s_hidden);
    lv_obj_set_x(s_root, FACE_W / 5);  // 从右边一点滑进来
    slide_root(0, NULL);
    return true;
}

void nomi_app_close(void) { close_app(true); }

const nomi_app_t *nomi_app_current(void) { return s_app; }

void nomi_apps_set_hidden(bool hidden)
{
    s_hidden = hidden;
    if (s_layer) lv_obj_set_flag(s_layer, LV_OBJ_FLAG_HIDDEN, hidden || !s_app);
}

/* ---------- 桌面（表盘往上推出来）：装上的应用 + 应用管理，一行四格。点开的都是全屏应用，右划回桌面 ---------- */

static void on_cell_event(lv_event_t *e)
{
    const nomi_app_t *app = (const nomi_app_t *)lv_event_get_user_data(e);
    lv_obj_t *tile = lv_obj_get_child(lv_event_get_target(e), 0);
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        lv_obj_set_style_bg_color(tile, lv_color_hex(C_CONTROL), 0);
        activity();
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        lv_obj_set_style_bg_color(tile, lv_color_hex(C_PANEL), 0);
        break;
    case LV_EVENT_CLICKED:
        nomi_app_open(app->id);
        break;
    default: break;
    }
}

static void add_cell(const nomi_app_t *app)
{
    lv_obj_t *cell = lv_obj_create(s_grid);
    lv_obj_remove_style_all(cell);
    lv_obj_set_size(cell, CELL_W, CELL_H);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, 8, 0);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(cell, on_cell_event, LV_EVENT_ALL, (void *)app);

    lv_obj_t *tile = lv_obj_create(cell);
    lv_obj_remove_style_all(tile);
    lv_obj_set_size(tile, TILE, TILE);
    lv_obj_set_style_radius(tile, 22, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *icon = lv_obj_create(tile);
    lv_obj_remove_style_all(icon);
    lv_obj_set_size(icon, ICON_PX, ICON_PX);
    lv_obj_center(icon);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    nomi_icon_draw(icon, app->icon, ICON_PX);

    lv_obj_t *name = lv_label_create(cell);
    lv_label_set_text(name, app->name);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(name, CELL_W - 8);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    if (app->dimmed) {  // 比如装在 SD 卡上但卡没插：底块淡下去、图标和名字都灰掉，一眼看得出用不了
        lv_obj_set_style_bg_opa(tile, LV_OPA_40, 0);
        icon_set_color(icon, C_DIMMED);
        lv_obj_set_style_text_color(name, lv_color_hex(C_DIMMED), 0);
    }
}

static void rebuild_grid(void)
{
    if (!s_grid) return;
    lv_obj_clean(s_grid);
    for (int i = 0; i < s_app_count; i++) add_cell(s_apps[i]);
    add_cell(&kAppMgr);
}

void nomi_apps_add(const nomi_app_t *app)
{
    for (int i = 0; i < s_app_count; i++) {
        if (s_apps[i] == app) return;
    }
    if (s_app_count >= MAX_APPS) return;
    int at = s_app_count;
    while (!app->system && at > 0 && s_apps[at - 1]->system) at--;  // 系统应用一直在最后
    memmove(&s_apps[at + 1], &s_apps[at], (s_app_count - at) * sizeof(s_apps[0]));
    s_apps[at] = app;
    s_app_count++;
    s_apps[s_app_count] = NULL;
    rebuild_grid();
}

void nomi_apps_remove(const nomi_app_t *app)
{
    if (s_app == app) close_app(true);
    for (int i = 0; i < s_app_count; i++) {
        if (s_apps[i] != app) continue;
        memmove(&s_apps[i], &s_apps[i + 1], (s_app_count - i) * sizeof(s_apps[0]));
        s_app_count--;
        break;
    }
    rebuild_grid();
}

void nomi_apps_refresh(void) { rebuild_grid(); }

const nomi_app_t *const *nomi_apps_list(void) { return s_apps; }

// 桌面上的左边缘：和应用里一样，从边上往右划过一段就返回——回表盘
static void on_launcher_edge(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_drag_x0 = p.x;
        activity();
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && p.x - s_drag_x0 > BACK_DISTANCE / 2) {
        if (s_go_home) s_go_home();
    }
}

static void build_launcher(lv_obj_t *tile)
{
    s_grid = lv_obj_create(tile);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_hor(s_grid, (FACE_W - GRID_COLS * CELL_W) / 2, 0);
    lv_obj_set_style_pad_top(s_grid, 30, 0);
    lv_obj_set_style_pad_bottom(s_grid, 20, 0);
    lv_obj_set_style_pad_row(s_grid, 14, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);  // 应用多了往下滚
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_OFF);
    // 音乐、小说、图片已经是应用包（apps/music、apps/novel、apps/photo，预装），装上后自己登记
    nomi_apps_add(&kMap);
    add_edge(tile, on_launcher_edge);
}

void nomi_launcher_shown(void)
{
    if (s_hooks.launcher_shown) s_hooks.launcher_shown();
}

void nomi_apps_set_font(const lv_font_t *font)
{
    if (!font) return;
    s_font = font;
    if (s_layer) lv_obj_set_style_text_font(s_layer, font, 0);
    if (s_grid) lv_obj_set_style_text_font(s_grid, font, 0);
}

void nomi_apps_init(lv_obj_t *screen, lv_obj_t *launcher_tile, const lv_font_t *font, void (*user_activity)(void),
                    void (*go_home)(void), void (*toast_cb)(const char *text))
{
    s_activity = user_activity;
    s_go_home = go_home;
    s_toast = toast_cb;
    s_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(s_layer);
    lv_obj_set_size(s_layer, FACE_W, FACE_H);
    lv_obj_remove_flag(s_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_layer, LV_OBJ_FLAG_HIDDEN);
    build_launcher(launcher_tile);
    nomi_apps_set_font(font);
}
