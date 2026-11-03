// Pocket Companion 应用 SDK（接口版本 1）。规范见 docs/app-sdk.md。
//
// 应用用 C 写，编译成 WebAssembly（python tools/app-sdk/build.py 应用目录）。应用要定义两个函数：
//   void    nomi_main(void);                                       进程起来时调一次：读设置、建卡片
//   int32_t nomi_event(int32_t type, int32_t id, int32_t value);  其他所有事情，处理了返回 1
// 下面 NOMI_IMPORT 的函数都由系统提供。没有完整的 C 库：用这里的 nomi_* 小工具。
//
// 进程和界面：从桌面打开时起进程、挂上界面，nomi_main 之后来一个 NOMI_EV_SHOW，在这时建第一页
// （只在前台用的简单应用也可以直接在 nomi_main 里建，SHOW 不理就行）。右划退出时界面拿下（NOMI_EV_HIDE），
// 有理由的进程留在后台：manifest 里 "tile": true（有首页卡片）、或者它放的歌还在放。没理由的过一会儿结束，
// 结束前来 NOMI_EV_CLOSE。在后台时 ui_page 等返回 NOMI_E_NO_UI；再打开时又来 SHOW，重建页面。
#pragma once

#include <stddef.h>
#include <stdint.h>

#define NOMI_API 1

#define NOMI_IMPORT(name) __attribute__((import_module("env"), import_name(#name)))
#define NOMI_EXPORT(name) __attribute__((export_name(#name)))

/* ---------- 应用要定义的两个函数 ---------- */

NOMI_EXPORT(nomi_main) void nomi_main(void);
NOMI_EXPORT(nomi_event) int32_t nomi_event(int32_t type, int32_t id, int32_t value);

enum {
    NOMI_EV_CLICK = 1,  // 点了按钮、列表项：id 是控件
    NOMI_EV_CHANGE,     // 开关、滑杆变了：id 是控件，value 是新值（预留）
    NOMI_EV_BACK,       // 左边缘右划退了一页：value 是剩几页（系统已经关掉了那一页）
    NOMI_EV_PAUSE,      // 预留
    NOMI_EV_RESUME,     // 预留
    NOMI_EV_CLOSE,      // 进程要结束了：趁这时候把进度存好（界面已经没了）
    NOMI_EV_TIMER,      // 定时器到点：id 是定时器
    NOMI_EV_VOICE,      // 大模型调了 manifest 里的语音指令：id 是第几个（从 1 起），参数用 voice_arg 取
    NOMI_EV_READER,     // 预留（阅读器翻页）
    NOMI_EV_MEDIA,      // 播放：value 是 NOMI_MEDIA_FINISHED 等
    NOMI_EV_SHOW,       // 界面挂上了：建页面
    NOMI_EV_HIDE,       // 界面拿下了：进程留在后台
};

enum { NOMI_TEXT_TITLE = 0, NOMI_TEXT_BODY = 1, NOMI_TEXT_MUTED = 2 };

// 错误码（接口返回负数时）
#define NOMI_E_FAIL (-1)
#define NOMI_E_DENIED (-2)   // 没在 manifest 里申请这个权限
#define NOMI_E_NO_SD (-3)    // 没插 SD 卡
#define NOMI_E_NOT_FOUND (-4)
#define NOMI_E_TOO_BIG (-5)
#define NOMI_E_NO_UI (-6)    // 界面没挂着（在后台）
#define NOMI_E_NO_NET (-7)   // 没联网

/* ---------- 界面：页面一层层往里压，应用里只上下滚，左边缘右划退一层 ---------- */

NOMI_IMPORT(ui_page) int32_t ui_page(const char *title);  // 压一页新页面（title 为空就不显示标题）
NOMI_IMPORT(ui_page_close) void ui_page_close(void);      // 关掉当前页
NOMI_IMPORT(ui_text) int32_t ui_text(const char *text, int32_t style);
NOMI_IMPORT(ui_item) int32_t ui_item(const char *title, const char *sub, const char *right);  // 可点的一行
NOMI_IMPORT(ui_button) int32_t ui_button(const char *text, int32_t primary);
NOMI_IMPORT(ui_set_text) void ui_set_text(int32_t id, const char *text);
NOMI_IMPORT(ui_set_hidden) void ui_set_hidden(int32_t id, int32_t hidden);
NOMI_IMPORT(ui_remove) void ui_remove(int32_t id);
NOMI_IMPORT(ui_toast) void ui_toast(const char *text);
// 首页卡片（manifest 里 "tile": true，占首页的收听卡片，像 Wear OS 的 tile）：清空重画，
// 接下来 ui_text 等建的控件都放进卡片，返回卡片的 id
NOMI_IMPORT(ui_tile) int32_t ui_tile(void);
// 首页卡片里的「正在放」：和系统收听页一个样（封面、标题、上一首 / 放停 / 下一首、进度或音量、右边歌词），
// 系统自己刷新。会清空卡片重画；hint 是没歌词可显示时右边中间那行（比如"跟它说「放周杰伦」"）。
// 返回顶上一排的 id（系统收听页放标签的地方）：接下来 ui_button 建的按钮放进那排、画成小标签，放应用的入口
NOMI_IMPORT(ui_now_playing) int32_t ui_now_playing(const char *hint);
// 接下来建的控件放到哪：一页（ui_page）、卡片（ui_tile）或「正在放」顶上那排（ui_now_playing）的返回值
NOMI_IMPORT(ui_target) int32_t ui_target(int32_t id);
// 打开自己的界面（卡片上点了"打开"时用），之后来 SHOW；已经开着就什么都不做
NOMI_IMPORT(ui_open) void ui_open(void);

/* ---------- 存储：data:/ 应用自己的数据，pkg:/ 应用包里的 assets，sd:/ SD 卡（只读，要 sd.read） ---------- */

NOMI_IMPORT(fs_list) int32_t fs_list(const char *dir, char *buf, int32_t size);  // 一行一个，目录带 '/'
NOMI_IMPORT(fs_size) int32_t fs_size(const char *path);
NOMI_IMPORT(fs_read) int32_t fs_read(const char *path, int32_t offset, void *buf, int32_t size);
NOMI_IMPORT(kv_get) int32_t kv_get(const char *key, char *buf, int32_t size);  // 没有返回 NOMI_E_NOT_FOUND
NOMI_IMPORT(kv_set) int32_t kv_set(const char *key, const char *value);

/* ---------- 播放（权限 media）：系统只管放"这一首"，放哪首、下一首放什么（队列）是应用的事 ---------- */

enum { NOMI_MEDIA_PAUSE = 1, NOMI_MEDIA_RESUME = 2, NOMI_MEDIA_STOP = 5, NOMI_MEDIA_TOGGLE = 6 };  // media_control
#define NOMI_PLAY_LIVE 1  // media_play 的 flags：直播（电台，没有进度、断了自动重连）

// NOMI_EV_MEDIA 的 value（只发给放这一首的应用；应用没在跑时系统先在后台把它拉起来）
enum {
    NOMI_MEDIA_FINISHED = 1,  // 这一首放完了：接着放什么你定（不放就停着）
    NOMI_MEDIA_NEXT = 2,      // 主人在系统的播放控件（ui_player、收听卡片）上点了下一首
    NOMI_MEDIA_PREV = 3,      // 上一首
    NOMI_MEDIA_PLAY = 4,      // 主人点了你的 ui_player / 首页卡片上的"放"，但停着的不是你的：你决定放什么（比如接着放上次的）
};

// 放一首：src 是 sd:/ 的文件（还要 sd.read）或 http(s) 地址；title、artist 显示用（SD 卡上的歌系统会读标签换掉）。
// 放的东西记在你名下：退出界面也接着放，放完来 NOMI_MEDIA_FINISHED
NOMI_IMPORT(media_play) int32_t media_play(const char *src, const char *title, const char *artist, int32_t flags);
// 网上的歌：给正在放的这首配封面（JPEG 地址）和 LRC 歌词，都可以空。SD 卡上的歌系统自己读内嵌封面和同名 .lrc
NOMI_IMPORT(media_meta) void media_meta(const char *cover_url, const char *lrc);
NOMI_IMPORT(media_control) void media_control(int32_t cmd);  // NOMI_MEDIA_PAUSE 等
NOMI_IMPORT(media_seek) void media_seek(int32_t second);
NOMI_IMPORT(media_state) int32_t media_state(void);  // 0 没在放、1 连接中、2 在放、3 暂停
NOMI_IMPORT(media_position) int32_t media_position(void);  // 放到第几秒
NOMI_IMPORT(media_duration) int32_t media_duration(void);  // 总长（秒），不知道为 0
NOMI_IMPORT(media_mine) int32_t media_mine(void);          // 现在放的（或停着的）是不是你放的：1 是
// 一首歌的信息："标题\n歌手\n时长秒"（标签读不到时标题是文件名）
NOMI_IMPORT(media_info) int32_t media_info(const char *path, char *buf, int32_t size);
// 系统画的「正在放」：封面、歌名、进度（能拖）、上一首 / 放停 / 下一首，系统自己刷新。页面、卡片里都能放
NOMI_IMPORT(ui_player) int32_t ui_player(void);

/* ---------- 联网（权限 net） ---------- */

// GET 一个 http(s) 地址（跟着跳转），正文最多 size 字节（多的截掉）；返回字节数。会卡住几秒，别在意界面卡
NOMI_IMPORT(net_get) int32_t net_get(const char *url, char *buf, int32_t size);

/* ---------- 大模型（权限 assistant） ---------- */

// 前台开着你的界面时，每轮对话的系统提示里带上这段（比如"正在看《xx》第 3 章"），最多 600 字节
NOMI_IMPORT(assistant_context) void assistant_context(const char *text);
// 收到 NOMI_EV_VOICE 时：取参数、回话（回话要在这次 nomi_event 里给；end = 1 时这句说完就回待机，
// 比如开始放歌了——不然对话还开着，声音出不来）
NOMI_IMPORT(voice_arg) int32_t voice_arg(char *buf, int32_t size);
NOMI_IMPORT(voice_reply) void voice_reply(const char *text, int32_t end);

/* ---------- 其他 ---------- */

NOMI_IMPORT(time_now) int64_t time_now(void);  // Unix 毫秒，没对上时间为 0
NOMI_IMPORT(timer_start) void timer_start(int32_t id, int32_t ms, int32_t repeat);  // 最多 8 个
NOMI_IMPORT(timer_stop) void timer_stop(int32_t id);
NOMI_IMPORT(nomi_log) void nomi_log(const char *text);  // 打到串口日志

/* ---------- 小工具（没有 C 库，自带几个常用的） ---------- */

static inline int32_t nomi_strlen(const char *s)
{
    int32_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline int32_t nomi_atoi(const char *s)
{
    int32_t sign = 1, v = 0;
    if (*s == '-') sign = -1, s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return sign * v;
}

// 只认 %d、%s、%%；结果一定以 '\0' 结尾，放不下的截掉
static inline int32_t nomi_fmt(char *out, int32_t size, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int32_t n = 0;
#define NOMI_PUT(c)                    \
    do {                               \
        if (n < size - 1) out[n] = (c); \
        n++;                           \
    } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            NOMI_PUT(*fmt);
            continue;
        }
        fmt++;
        if (*fmt == 'd') {
            int32_t v = __builtin_va_arg(ap, int32_t);
            char tmp[12];
            int32_t k = 0;
            uint32_t u = v < 0 ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
            if (v < 0) NOMI_PUT('-');
            do tmp[k++] = (char)('0' + u % 10); while ((u /= 10) && k < 11);
            while (k) NOMI_PUT(tmp[--k]);
        } else if (*fmt == 's') {
            const char *s = __builtin_va_arg(ap, const char *);
            while (s && *s) NOMI_PUT(*s++);
        } else if (*fmt) {
            NOMI_PUT(*fmt);
        } else {
            break;
        }
    }
#undef NOMI_PUT
    out[n < size ? n : size - 1] = '\0';
    __builtin_va_end(ap);
    return n;
}

static inline int32_t kv_get_int(const char *key, int32_t def)
{
    char buf[16];
    return kv_get(key, buf, sizeof(buf)) > 0 ? nomi_atoi(buf) : def;
}

static inline int32_t kv_set_int(const char *key, int32_t value)
{
    char buf[16];
    nomi_fmt(buf, sizeof(buf), "%d", value);
    return kv_set(key, buf);
}
