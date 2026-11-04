// 应用：全屏盖在卡片（表情、收听、时钟、历史这几页）上面，从桌面（表盘往上推）或语音打开。
// 桌面上只放装上的应用，外加一个系统应用「应用管理」（装、删应用）；收听、时钟、对话记录、设置这些
// 系统功能不进应用层，还是左右滑的卡片和下拉设置。
//
// 交互照 Wear OS：应用里只上下走；从左边缘往右划是返回——手指拖着整个应用往右走，松手时拖过一段就
// 返回（应用自己能退一层的先退一层，退到头才关），没拖够就弹回去。不放返回按钮。
// 对话浮层、闹钟盖在应用上面，应用不关；说完原地继续，看到哪还在哪。睡着时应用藏起来，醒了原样露出来。
//
// 应用有两种：编进固件的（应用管理、现在的几个占位），和装上的应用包（WebAssembly，见 docs/app-sdk.md，
// 由 nomi_pkg / nomi_wasm 管，它们用 nomi_apps_add 把包登记到桌面上）。
//
// 和 nomi_ui 一样只依赖 LVGL，设备和浏览器模拟器共用。所有函数都要在持有 LVGL 锁的上下文里调用。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// 线条图标：在 36×36 的格子里写折线和描边圆，系统统一画成白色 2 像素细线（应用包的 manifest.json 里也是这么写）
typedef struct {
    const lv_point_precise_t *points;
    uint16_t count;
} nomi_icon_line_t;
typedef struct {
    int16_t x, y, d;  // 外接方块左上角和直径
} nomi_icon_circle_t;
typedef struct {
    const nomi_icon_line_t *lines;
    uint8_t line_count;
    const nomi_icon_circle_t *circles;
    uint8_t circle_count;
} nomi_icon_t;

typedef struct nomi_app nomi_app_t;
struct nomi_app {
    const char *id;    // 唯一的名字：编进固件的是 "appmgr" 这种，应用包是 manifest 里的 id（"作者.应用"）
    const char *name;  // 桌面上显示的名字
    const nomi_icon_t *icon;
    // 打开：在 root（全屏、黑底）里建界面，接着上次的进度
    void (*open)(const nomi_app_t *app, lv_obj_t *root);
    // 关掉：存进度、放资源；root 由框架删，可以为 NULL
    void (*close)(const nomi_app_t *app);
    // 左边缘右划：应用自己退了一层返回 true；返回 false（或为 NULL）就关掉应用回桌面
    bool (*back)(const nomi_app_t *app);
    // 可选：对话时告诉大模型用户在这个应用里看什么（比如小说的当前页）；没有返回 NULL
    const char *(*context)(const nomi_app_t *app);
    void *user;               // 应用自己的数据（应用包：它的安装信息）
    bool dimmed;              // 桌面上灰着、点了只弹提示（比如装在 SD 卡上但卡没插）；改了要 nomi_apps_refresh
    const char *dimmed_hint;  // 灰着时点了弹什么
};

// launcher_tile：外层翻页里放桌面的那一格；user_activity：应用里有触摸时调（重置休眠计时）；
// go_home：桌面上左边缘右划时调，回表盘；toast：弹一句提示
void nomi_apps_init(lv_obj_t *screen, lv_obj_t *launcher_tile, const lv_font_t *font, void (*user_activity)(void),
                    void (*go_home)(void), void (*toast)(const char *text));
void nomi_apps_set_font(const lv_font_t *font);

// 桌面上加一个应用（排在已有的后面、应用管理前面）/ 去掉一个（开着的话先关掉）。app 要一直有效直到去掉
void nomi_apps_add(const nomi_app_t *app);
void nomi_apps_remove(const nomi_app_t *app);
// 应用的名字、图标、灰不灰变了：重画桌面
void nomi_apps_refresh(void);
// 桌面上的应用（不含应用管理），以 NULL 结尾；给应用管理列表用
const nomi_app_t *const *nomi_apps_list(void);

// 打开 / 关掉应用（也给语音用）；id 不认识返回 false
bool nomi_app_open(const char *id);
void nomi_app_close(void);
const nomi_app_t *nomi_app_current(void);  // 开着的应用，没有为 NULL

// 睡着时藏起应用层（应用不关），醒了再露出来
void nomi_apps_set_hidden(bool hidden);
// 桌面那一格翻到眼前时调（现在没事要做，留着给以后刷新角标之类）
void nomi_launcher_shown(void);

// 画一个线条图标（36×36 的格子放大到 px 见方），桌面、应用管理列表都用它
void nomi_icon_draw(lv_obj_t *parent, const nomi_icon_t *icon, int px);

// 应用管理里删应用、显示说明要做的事由设备那边给（模拟器里没有就只列出来）
typedef struct {
    bool (*can_uninstall)(const nomi_app_t *app);
    void (*uninstall)(const nomi_app_t *app);  // 已经确认过
    const char *(*summary)(void);              // 顶上那段说明：怎么装、存储用了多少
    // 下面两个可以不给。桌面翻到眼前时（设备去看一眼 SD 卡还在不在，卡上的应用跟着灰掉 / 恢复）
    void (*launcher_shown)(void);
    // 点了灰着的应用，dimmed_hint 已经弹过了（设备再去看一眼卡，插回来了就恢复并打开它）
    void (*dimmed_tap)(const nomi_app_t *app);
} nomi_appmgr_hooks_t;
void nomi_appmgr_set_hooks(const nomi_appmgr_hooks_t *hooks);

#ifdef __cplusplus
}
#endif
