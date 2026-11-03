// 最小示例：点一下加一；进度用 kv 存着，退出再进来还记得。
// 再点「下一页」压一页新的，试试左边缘右划退回来。
#include "nomi_sdk.h"

static int32_t g_count, g_label, g_add, g_next;

static void show_count(void)
{
    char text[32];
    nomi_fmt(text, sizeof(text), "点了 %d 次", g_count);
    ui_set_text(g_label, text);
}

void nomi_main(void)
{
    g_count = kv_get_int("count", 0);
    ui_page("计数器");
    g_label = ui_text("", NOMI_TEXT_TITLE);
    g_add = ui_button("点我加一", 1);
    g_next = ui_item("下一页", "试试从左边缘往右划退回来", ">");
    ui_text("这是一个应用包：WebAssembly 写的，装在 SD 卡上，能在应用管理里删掉。", NOMI_TEXT_MUTED);
    show_count();
}

int32_t nomi_event(int32_t type, int32_t id, int32_t value)
{
    (void)value;
    if (type == NOMI_EV_CLICK && id == g_add) {
        g_count++;
        kv_set_int("count", g_count);
        show_count();
        return 1;
    }
    if (type == NOMI_EV_CLICK && id == g_next) {
        ui_page("第二页");
        ui_text("从屏幕左边缘往右划，回到上一页。", NOMI_TEXT_BODY);
        return 1;
    }
    return 0;
}
