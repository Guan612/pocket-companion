// 相机：取景器（系统画实时画面）、拍照存卡、「问它」（照片交给大模型，开口问或者让它讲讲画面）。
//
// 只有一页：取景（画面 + 右边一列按钮）。看照片不自己做：「照片」把最新那张交给系统打开（open_file），
// 图片应用打开 DCIM 那个相册、翻到这一张；从图片应用划回去回到相机。
// 照片存在卡上 DCIM/IMG_0001.jpg 起，编号接着卡上最大的往下排。
#include "nomi_sdk.h"
#include "nomi_text.h"

#define DIR "sd:/DCIM"

static int32_t g_shot, g_ask, g_view;  // 取景页的按钮
static int32_t g_keep_on;

static char g_list[4096];  // fs_list 的结果，名字一行一个

// 卡上 DCIM 里 IMG_ 编号最大的那张：返回编号（没有为 0），没插卡返回 NOMI_E_NO_SD
static int32_t latest(void)
{
    int32_t n = fs_list(DIR, g_list, sizeof(g_list));
    if (n == NOMI_E_NO_SD) return n;
    if (n <= 0) return 0;
    int32_t max = 0;
    const char *s = g_list;
    while (*s) {
        if (nomi_starts(s, "IMG_")) {
            int32_t v = nomi_atoi(s + 4);
            if (v > max) max = v;
        }
        while (*s && *s != '\n') s++;
        if (*s) s++;
    }
    return max;
}

static void path_of(int32_t v, char *out, int32_t size)
{
    nomi_fmt(out, size, "%s/IMG_%s%s%s%d.jpg", DIR, v < 1000 ? "0" : "", v < 100 ? "0" : "", v < 10 ? "0" : "", v);
}

static void keep_on(int32_t on)
{
    if (on == g_keep_on) return;
    g_keep_on = on;
    screen_keep_on(on);
}

// 拍一张存卡；out 给出提示（显示或念给主人）
static void shoot(char *out, int32_t size)
{
    int32_t last = latest();
    if (last == NOMI_E_NO_SD) {
        nomi_copy_n(out, size, "没插 SD 卡，照片没地方存", 0);
        return;
    }
    char path[64];
    path_of(last + 1, path, sizeof(path));
    int32_t n = camera_shot(path);
    if (n == NOMI_E_NO_SD) nomi_copy_n(out, size, "没插 SD 卡，照片没地方存", 0);
    else if (n < 0) nomi_copy_n(out, size, "没拍成，再试一次", 0);
    else nomi_fmt(out, size, "拍好了，存在 %s", path + 4);  // 去掉 "sd:/"
}

// 最新那张交给图片应用看
static void open_photos(void)
{
    int32_t last = latest();
    if (last == NOMI_E_NO_SD) {
        ui_toast("没插 SD 卡");
        return;
    }
    if (last == 0) {
        ui_toast("还没拍过照片");
        return;
    }
    char path[64];
    path_of(last, path, sizeof(path));
    if (open_file(path) < 0) ui_toast("打不开照片");
}

static void show_viewfinder(void)
{
    if (ui_page("") < 0) return;
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

void nomi_main(void) {}

int32_t nomi_event(int32_t type, int32_t id, int32_t value)
{
    (void)value;
    char msg[96];
    switch (type) {
    case NOMI_EV_SHOW:
        show_viewfinder();
        return 1;
    case NOMI_EV_HIDE:
    case NOMI_EV_CLOSE:
        keep_on(0);
        return 1;
    case NOMI_EV_CLICK:
        if (id == g_shot) {
            shoot(msg, sizeof(msg));
            ui_toast(msg);
        } else if (id == g_ask) {
            ui_toast(camera_ask("") >= 0 ? "拍好了，问吧（不说话就讲讲画面）" : "没拍成，再试一次");
        } else if (id == g_view) {
            open_photos();
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
