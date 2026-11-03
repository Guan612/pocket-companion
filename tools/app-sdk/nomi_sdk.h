// Pocket Companion 应用 SDK（接口版本 1）。规范见 docs/app-sdk.md。
//
// 应用用 C 写，编译成 WebAssembly（python tools/app-sdk/build.py 应用目录）。应用要定义两个函数：
//   void    nomi_main(void);                                       打开时调一次：建第一页
//   int32_t nomi_event(int32_t type, int32_t id, int32_t value);  其他所有事情，处理了返回 1
// 下面 NOMI_IMPORT 的函数都由系统提供。没有完整的 C 库：用这里的 nomi_* 小工具。
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
    NOMI_EV_CHANGE,     // 开关、滑杆变了：id 是控件，value 是新值
    NOMI_EV_BACK,       // 左边缘右划退了一页：value 是剩几页（系统已经关掉了那一页）
    NOMI_EV_PAUSE,      // 对话浮层升起、睡着
    NOMI_EV_RESUME,     // 浮层收起、醒来
    NOMI_EV_CLOSE,      // 要关了：趁这时候把进度存好
    NOMI_EV_TIMER,      // 定时器到点：id 是定时器
    NOMI_EV_VOICE,      // 用户说了注册的语音指令：id 是指令
    NOMI_EV_READER,     // 阅读器翻页：id 是阅读器，value 是读到的字节位置
    NOMI_EV_MEDIA,      // 播放状态变了：value 是新状态
};

enum { NOMI_TEXT_TITLE = 0, NOMI_TEXT_BODY = 1, NOMI_TEXT_MUTED = 2 };

// 错误码（接口返回负数时）
#define NOMI_E_FAIL (-1)
#define NOMI_E_DENIED (-2)   // 没在 manifest 里申请这个权限
#define NOMI_E_NO_SD (-3)    // 没插 SD 卡
#define NOMI_E_NOT_FOUND (-4)
#define NOMI_E_TOO_BIG (-5)

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

/* ---------- 存储：data:/ 应用自己的数据，pkg:/ 应用包里的 assets，sd:/ SD 卡（只读，要 sd.read） ---------- */

NOMI_IMPORT(fs_list) int32_t fs_list(const char *dir, char *buf, int32_t size);  // 一行一个，目录带 '/'
NOMI_IMPORT(fs_size) int32_t fs_size(const char *path);
NOMI_IMPORT(fs_read) int32_t fs_read(const char *path, int32_t offset, void *buf, int32_t size);
NOMI_IMPORT(kv_get) int32_t kv_get(const char *key, char *buf, int32_t size);  // 没有返回 NOMI_E_NOT_FOUND
NOMI_IMPORT(kv_set) int32_t kv_set(const char *key, const char *value);

/* ---------- 其他 ---------- */

NOMI_IMPORT(time_now) int64_t time_now(void);  // Unix 毫秒，没对上时间为 0
NOMI_IMPORT(timer_start) void timer_start(int32_t id, int32_t ms, int32_t repeat);
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
