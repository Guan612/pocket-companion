// 应用：全屏盖在卡片（表情、收听、时钟、历史这几页）上面，从桌面（表盘往上推）或语音打开。
// 桌面上只放装上去的应用，外加一个系统应用「应用管理」（装、删应用）；收听、时钟、对话记录、设置这些
// 系统功能不进应用层，还是左右滑的卡片和下拉设置。
//
// 交互照 Wear OS：应用里只上下走；从左边缘往右划是返回——手指拖着整个应用往右走，松手时拖过一段就
// 返回（应用自己能退一层的先退一层，退到头才关），没拖够就弹回去。不放返回按钮。
// 对话浮层、闹钟盖在应用上面，应用不关；说完原地继续，看到哪还在哪。睡着时应用藏起来，醒了原样露出来。
//
// 和 nomi_ui 一样只依赖 LVGL，设备和浏览器模拟器共用。所有函数都要在持有 LVGL 锁的上下文里调用。
#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nomi_app {
    const char *id;    // 语音、代码里用的名字："music" "novel" "photo" "map"
    const char *name;  // 应用列表里显示的名字
    // 应用列表里的线条图标：在 36×36 的 parent 里用 lv_line（线宽 2）和描边圆画，颜色由框架统一改
    void (*draw_icon)(lv_obj_t *parent);
    // 打开：在 root（全屏、黑底）里建界面，接着上次的进度
    void (*open)(lv_obj_t *root);
    // 关掉：存进度、放资源；root 由框架删，可以为 NULL
    void (*close)(void);
    // 左边缘右划：应用自己退了一层返回 true；返回 false（或为 NULL）就关掉应用回应用列表
    bool (*back)(void);
    // 可选：对话时告诉大模型用户在这个应用里看什么（比如小说的当前页）；没有返回 NULL
    const char *(*context)(void);
} nomi_app_t;

// launcher_tile：外层翻页里放桌面的那一格；user_activity：应用里有触摸时调（重置休眠计时）；
// go_home：桌面上左边缘右划时调，回表盘
void nomi_apps_init(lv_obj_t *screen, lv_obj_t *launcher_tile, const lv_font_t *font, void (*user_activity)(void),
                    void (*go_home)(void));
void nomi_apps_set_font(const lv_font_t *font);

// 打开 / 关掉应用（也给语音用）；id 不认识返回 false
bool nomi_app_open(const char *id);
void nomi_app_close(void);
const nomi_app_t *nomi_app_current(void);  // 开着的应用，没有为 NULL

// 睡着时藏起应用层（应用不关），醒了再露出来
void nomi_apps_set_hidden(bool hidden);
// 桌面那一格翻到眼前时调（现在没事要做，留着给以后刷新角标之类）
void nomi_launcher_shown(void);

// 装上的应用（桌面上的顺序，「应用管理」排在它们后面），以 NULL 结尾
extern const nomi_app_t *const nomi_apps[];

#ifdef __cplusplus
}
#endif
