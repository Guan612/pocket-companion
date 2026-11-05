// 相机：取景器（系统画实时画面）、拍照存卡、翻看拍过的、「问它」（照片交给大模型，开口问或者让它讲讲画面）。
//
// 页面：1 取景（画面 + 右边一列按钮） → 2 看照片（系统全屏看图，左右滑翻） → 3 这张照片的菜单（删除）。
// 照片存在卡上 DCIM/IMG_0001.jpg 起，编号接着卡上最大的往下排；按名字排就是拍的先后。
#include "nomi_sdk.h"
#include "nomi_text.h"

#define DIR "sd:/DCIM"
#define MAX_PHOTOS 300

static int32_t g_pages;                 // 现在几页（看 NOMI_EV_BACK 的 value）
static int32_t g_shot, g_ask, g_view;   // 取景页的按钮
static int32_t g_viewer;                // 看照片页的看图控件
static int32_t g_del, g_back;           // 菜单页
static int32_t g_keep_on;

static char g_list[MAX_PHOTOS * 16];    // fs_list 的结果，名字一行一个（原地切成字符串）
static const char *g_names[MAX_PHOTOS]; // 照片名，按名字排
static int32_t g_count;
static int32_t g_index;                 // 正在看第几张

static int name_less(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a < (unsigned char)*b;
}

static int is_jpg(const char *s)
{
    int32_t n = nomi_len(s);
    return n > 4 && (nomi_equal(s + n - 4, ".jpg") || nomi_equal(s + n - 4, ".JPG") || nomi_equal(s + n - 5, ".jpeg"));
}

// 读卡上 DCIM 里的照片（只认 .jpg / .jpeg）。返回张数，没插卡返回 NOMI_E_NO_SD
static int32_t scan(void)
{
    g_count = 0;
    int32_t n = fs_list(DIR, g_list, sizeof(g_list));
    if (n == NOMI_E_NO_SD) return n;
    if (n <= 0) return 0;
    char *s = g_list;
    while (*s && g_count < MAX_PHOTOS) {
        char *line = s;
        while (*s && *s != '\n') s++;
        if (*s) *s++ = '\0';
        if (!is_jpg(line)) continue;  // 文件夹（带 '/'）和别的文件不要
        int32_t i = g_count++;
        while (i > 0 && name_less(line, g_names[i - 1])) {  // 插进去，保持按名字排
            g_names[i] = g_names[i - 1];
            i--;
        }
        g_names[i] = line;
    }
    return g_count;
}

static void path_of(int32_t i, char *out, int32_t size) { nomi_fmt(out, size, "%s/%s", DIR, g_names[i]); }

// 下一张的名字：接着卡上最大的 IMG_ 编号往下排
static void next_name(char *out, int32_t size)
{
    int32_t max = 0;
    for (int32_t i = 0; i < g_count; i++) {
        if (!nomi_starts(g_names[i], "IMG_")) continue;
        int32_t v = nomi_atoi(g_names[i] + 4);
        if (v > max) max = v;
    }
    int32_t v = max + 1;
    nomi_fmt(out, size, "%s/IMG_%s%s%s%d.jpg", DIR, v < 1000 ? "0" : "", v < 100 ? "0" : "", v < 10 ? "0" : "", v);
}

static void keep_on(int32_t on)
{
    if (on == g_keep_on) return;
    g_keep_on = on;
    screen_keep_on(on);
}

// 拍一张存卡；out 给出提示（显示或念给主人）。返回 1 成功
static int shoot(char *out, int32_t size)
{
    if (scan() == NOMI_E_NO_SD) {
        nomi_copy_n(out, size, "没插 SD 卡，照片没地方存", 0);
        return 0;
    }
    char path[64];
    next_name(path, sizeof(path));
    int32_t n = camera_shot(path);
    if (n == NOMI_E_NO_SD) {
        nomi_copy_n(out, size, "没插 SD 卡，照片没地方存", 0);
        return 0;
    }
    if (n < 0) {
        nomi_copy_n(out, size, "没拍成，再试一次", 0);
        return 0;
    }
    nomi_fmt(out, size, "拍好了，存在 %s", path + 4);  // 去掉 "sd:/"
    return 1;
}

/* ---------- 页面 ---------- */

static void show_viewfinder(void)
{
    if (ui_page("") < 0) return;
    g_pages = 1;
    int32_t side = ui_camera();
    if (side < 0) {
        ui_text("这台设备没有摄像头", NOMI_TEXT_MUTED);
        return;
    }
    ui_target(side);
    g_shot = ui_button("拍照", 1);
    g_ask = ui_button("问它", 0);
    g_view = ui_button("照片", 0);
    keep_on(1);
}

static void show_photo(int32_t i)
{
    char path[64];
    path_of(i, path, sizeof(path));
    g_index = i;
    if (g_pages == 2 && g_viewer > 0) {
        photo_show(g_viewer, path);
        return;
    }
    if (ui_page("") < 0) return;
    g_pages = 2;
    g_viewer = ui_photo(path, NOMI_PHOTO_SWIPE_LR);
    keep_on(0);  // 看照片照常睡
}

static void open_photos(void)
{
    int32_t n = scan();
    if (n == NOMI_E_NO_SD) {
        ui_toast("没插 SD 卡");
        return;
    }
    if (n == 0) {
        ui_toast("还没拍过照片");
        return;
    }
    show_photo(n - 1);  // 从最新的看起
}

static void show_menu(void)
{
    if (ui_page("这张照片") < 0) return;
    g_pages = 3;
    char s[64];
    nomi_fmt(s, sizeof(s), "第 %d 张，共 %d 张", g_index + 1, g_count);
    ui_text(g_names[g_index], NOMI_TEXT_BODY);
    ui_text(s, NOMI_TEXT_MUTED);
    g_del = ui_item("删除这张", "从卡上删掉，删了找不回来", "");
    g_back = ui_item("回去拍照", "", "");
}

static void delete_current(void)
{
    char path[64];
    path_of(g_index, path, sizeof(path));
    int32_t i = g_index;
    if (fs_remove(path) < 0) {
        ui_toast("没删掉");
        return;
    }
    ui_toast("删了");
    ui_page_close();  // 回到看照片
    g_pages = 2;
    if (scan() <= 0) {
        ui_page_close();  // 一张都没了：回取景
        g_pages = 1;
        keep_on(1);
        return;
    }
    show_photo(i < g_count ? i : g_count - 1);
}

static void reset(void)
{
    keep_on(0);
    g_pages = 0;
    g_viewer = 0;
}

void nomi_main(void) {}

int32_t nomi_event(int32_t type, int32_t id, int32_t value)
{
    char msg[96];
    switch (type) {
    case NOMI_EV_SHOW:
        show_viewfinder();
        return 1;
    case NOMI_EV_HIDE:
    case NOMI_EV_CLOSE:
        reset();
        return 1;
    case NOMI_EV_BACK:
        g_pages = value;
        if (value < 2) g_viewer = 0;
        keep_on(value == 1);  // 回到取景：屏幕别睡
        return 1;
    case NOMI_EV_CLICK:
        if (id == g_shot && g_pages == 1) {
            shoot(msg, sizeof(msg));
            ui_toast(msg);
        } else if (id == g_ask && g_pages == 1) {
            int32_t n = camera_ask("");
            ui_toast(n >= 0 ? "拍好了，问吧（不说话就讲讲画面）" : "没拍成，再试一次");
        } else if (id == g_view && g_pages == 1) {
            open_photos();
        } else if (id == g_del && g_pages == 3) {
            delete_current();
        } else if (id == g_back && g_pages == 3) {
            ui_page_close();
            ui_page_close();
            g_pages = 1;
            g_viewer = 0;
            keep_on(1);
        }
        return 1;
    case NOMI_EV_PHOTO:
        if (g_pages != 2 || g_count == 0) return 1;
        if (value == NOMI_PHOTO_NEXT) {
            if (g_index + 1 < g_count) show_photo(g_index + 1);
            else ui_toast("这是最新的一张");
        } else if (value == NOMI_PHOTO_PREV) {
            if (g_index > 0) show_photo(g_index - 1);
            else ui_toast("这是第一张");
        } else {
            show_menu();
        }
        return 1;
    case NOMI_EV_VOICE:
        // 不在前台也能拍（系统在后台把应用拉起来）
        shoot(msg, sizeof(msg));
        voice_reply(msg, 0);
        return 1;
    default:
        return 0;
    }
}
