// Pocket Companion 应用 SDK（接口版本见 NOMI_API）。规范见 docs/app-sdk.md。
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

#define NOMI_API 3  // 用到的最高版本写进 manifest 的 "api"。
// 2：阅读器（ui_reader 等）、text_chapters、fs_find。
// 3：图片（ui_photo 等）、写卡（fs_write 等，权限 sd.write）、打开文件（manifest 的 opens、NOMI_EV_OPEN）、
//    选文件（ui_files）

#define NOMI_IMPORT(name) __attribute__((import_module("env"), import_name(#name)))
#define NOMI_EXPORT(name) __attribute__((export_name(#name)))

/* ---------- 应用要定义的两个函数 ---------- */

NOMI_EXPORT(nomi_main) void nomi_main(void);
NOMI_EXPORT(nomi_event) int32_t nomi_event(int32_t type, int32_t id, int32_t value);

enum {
    NOMI_EV_CLICK = 1,  // 点了按钮、列表项：id 是控件
    NOMI_EV_CHANGE,     // 拖条松手了：id 是控件，value 是新值
    NOMI_EV_BACK,       // 左边缘右划退了一页：value 是剩几页（系统已经关掉了那一页）
    NOMI_EV_PAUSE,      // 预留
    NOMI_EV_RESUME,     // 预留
    NOMI_EV_CLOSE,      // 进程要结束了：趁这时候把进度存好（界面已经没了）
    NOMI_EV_TIMER,      // 定时器到点：id 是定时器
    NOMI_EV_VOICE,      // 大模型调了 manifest 里的语音指令：id 是第几个（从 1 起），参数用 voice_arg 取
    NOMI_EV_READER,     // 阅读器：id 是阅读器，value 是这页开头的位置（≥ 0，存下来就是书签）或 NOMI_READER_MENU / END
    NOMI_EV_MEDIA,      // 播放：value 是 NOMI_MEDIA_FINISHED 等
    NOMI_EV_SHOW,       // 界面挂上了：建页面
    NOMI_EV_HIDE,       // 界面拿下了：进程留在后台
    NOMI_EV_WEB,        // 应用自带的网页发来请求：用 web_body 取请求体，web_reply 回话（见「网页」）
    NOMI_EV_PHOTO,      // （api 3）全屏看图：id 是看图控件，value 是 NOMI_PHOTO_NEXT / PREV / MENU
    NOMI_EV_OPEN,       // （api 3）打开一个文件：用 open_path 取路径（见「打开文件」）
    NOMI_EV_FILE,       // （api 3）文件列表里选了：id 是 ui_files 的控件，value 0 文件、1 文件夹，files_picked 取路径
};

enum { NOMI_TEXT_TITLE = 0, NOMI_TEXT_BODY = 1, NOMI_TEXT_MUTED = 2 };

// 错误码（接口返回负数时）
#define NOMI_E_FAIL (-1)
#define NOMI_E_DENIED (-2)   // 没在 manifest 里申请这个权限
#define NOMI_E_NO_SD (-3)    // 没插 SD 卡
#define NOMI_E_NOT_FOUND (-4)
#define NOMI_E_TOO_BIG (-5)
#define NOMI_E_NO_UI (-6)    // 界面没挂着（在后台）
#define NOMI_E_NO_NET (-7)   // 没联网（net_get 别的失败：连不上、不是 200，都是 NOMI_E_FAIL）

/* ---------- 界面：页面一层层往里压，应用里只上下滚，左边缘右划退一层 ---------- */

NOMI_IMPORT(ui_page) int32_t ui_page(const char *title);  // 压一页新页面（title 为空就不显示标题）
NOMI_IMPORT(ui_page_close) void ui_page_close(void);      // 关掉当前页
NOMI_IMPORT(ui_text) int32_t ui_text(const char *text, int32_t style);
NOMI_IMPORT(ui_item) int32_t ui_item(const char *title, const char *sub, const char *right);  // 可点的一行
NOMI_IMPORT(ui_button) int32_t ui_button(const char *text, int32_t primary);
NOMI_IMPORT(ui_set_text) void ui_set_text(int32_t id, const char *text);
// （api 2）拖条：label 里的 {} 换成当前值，拖的时候跟着变（比如 "跳到 {}%"）；松手时来 NOMI_EV_CHANGE，value 是新值
NOMI_IMPORT(ui_slider) int32_t ui_slider(const char *label, int32_t min, int32_t max, int32_t value);
NOMI_IMPORT(ui_set_value) void ui_set_value(int32_t id, int32_t value);  // 改拖条的值（不发 NOMI_EV_CHANGE）
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

/* ---------- 图片（api 3）：系统解码（JPEG 边读边解、按 EXIF 转正；PNG 只认 640×640 以内的） ---------- */

#define NOMI_PHOTO_SWIPE_LR 1  // ui_photo 的 flags：左滑下一张、右滑上一张
#define NOMI_PHOTO_SWIPE_UD 2  // 上滑下一张、下滑上一张
#define NOMI_PHOTO_TAP_LR 4    // 点左三分之一上一张、右三分之一下一张（点中间、长按都是 NOMI_PHOTO_MENU）
enum { NOMI_PHOTO_NEXT = 1, NOMI_PHOTO_PREV = 2, NOMI_PHOTO_MENU = 3 };  // NOMI_EV_PHOTO 的 value
// 网格容器：控件一排排往右放、放满换行（一行 4 张缩略图）；返回 id 给 ui_target
NOMI_IMPORT(ui_grid) int32_t ui_grid(void);
// 一张缩略图（方块、铺满裁切，88 像素），点了来 NOMI_EV_CLICK。系统在后台解（手机照片先用自带的小图），
// 解好的存在卡上 nomi/cache/thumbs/，下次秒开。一页别放太多（每张占 15KB 内存），多了分页
NOMI_IMPORT(ui_thumb) int32_t ui_thumb(const char *path);
// 全屏看图：占满当前页（这页别放别的控件，标题给空字符串），整张放进屏幕、四周留黑。翻图手势、点中间要菜单
// 都来 NOMI_EV_PHOTO；下一张放哪张由应用定，用 photo_show 换
NOMI_IMPORT(ui_photo) int32_t ui_photo(const char *path, int32_t flags);
NOMI_IMPORT(photo_show) void photo_show(int32_t id, const char *path);
NOMI_IMPORT(photo_set_flags) void photo_set_flags(int32_t id, int32_t flags);
// 预读：后台把这张解成全屏图存进卡上的缓存（排在别的后面），翻过去就快。看一张时预读前后两张。
// 全屏图解过一次都有缓存（每张约 300KB），第二次看秒开；没缓存时系统先把照片自带的小图放大当预览
NOMI_IMPORT(photo_preload) void photo_preload(const char *path);
// 电子相框：设备睡着时（设置里「睡着时显示」选了相册）轮播 dir 这个文件夹里的图（打乱顺序），每 seconds 秒
// 换一张（至少 10 秒）。dir 空字符串 = 不设了。设备记着，重启还在
NOMI_IMPORT(photo_frame) int32_t photo_frame(const char *dir, int32_t seconds);

/* ---------- 阅读器（api 2）：看 TXT 小说，系统排版、分页、翻页 ---------- */

// 认编码（UTF-8 / GBK）；空行去掉、段首缩进两格；系统字里没有的字用 SD 卡 fonts/ 里的 .ttf 补。
// 位置都是文件里的字节位置：书签、章节、跳转都用它。
#define NOMI_READER_SWIPE 1   // flags：上滑下一页、下滑上一页
#define NOMI_READER_TAP 2     // flags：点上三分之一上一页、下三分之一下一页
#define NOMI_READER_TAP_LR 4  // flags：点左三分之一上一页、右三分之一下一页（代替 NOMI_READER_TAP）
// 不管哪种，点中间那三分之一、长按都发 NOMI_READER_MENU
#define NOMI_READER_MENU (-1)  // NOMI_EV_READER 的 value：点了中间 / 长按（应用弹自己的菜单：快速跳转、目录、设置……）
#define NOMI_READER_END (-2)   // NOMI_EV_READER 的 value：在最后一页还往后翻
// 在当前页建一个阅读器，占满整页（这页别放别的控件，标题给空字符串）。path 一般是 sd:/ 的（要 sd.read），
// offset 是从哪开始（书签，0 = 开头）。建好就来一次 NOMI_EV_READER（这一页的位置），之后每翻一页来一次。
// ui_set_text(阅读器, 字) 改左下角那行（比如章名），右下角的百分比系统自己写。打不开返回 NOMI_E_NOT_FOUND
NOMI_IMPORT(ui_reader) int32_t ui_reader(const char *path, int32_t offset, int32_t flags);
NOMI_IMPORT(reader_goto) void reader_goto(int32_t id, int32_t offset);  // 跳到某个位置（对齐到那一段的开头）
NOMI_IMPORT(reader_set_flags) void reader_set_flags(int32_t id, int32_t flags);  // 换翻页方式
NOMI_IMPORT(reader_text) int32_t reader_text(int32_t id, char *buf, int32_t size);  // 这一页的字（给大模型当上下文）
// 章节（第 X 章 / 回 / 节 / 卷、楔子、尾声、Chapter N……）：返回一共几章；buf 里从第 first 章（从 0 起）起
// 一行一个「位置 TAB 章名」，放不下的不放。第一次要把整本书读一遍（几 MB 的书几秒），之后用卡上的缓存
NOMI_IMPORT(text_chapters) int32_t text_chapters(const char *path, int32_t first, char *buf, int32_t size);
// offset 在第几章（从 0 起）；在第一章之前、或者这本书没分章，返回 -1
NOMI_IMPORT(text_chapter_at) int32_t text_chapter_at(const char *path, int32_t offset);

/* ---------- 存储：data:/ 应用自己的数据，pkg:/ 应用包里的 assets（只读），sd:/ SD 卡 ---------- */
// sd:/ 读要 sd.read，写要 sd.write（有 sd.write 也能读）。卡上系统自己的 nomi/ 文件夹（应用、缓存）读写都不行。

NOMI_IMPORT(fs_list) int32_t fs_list(const char *dir, char *buf, int32_t size);  // 一行一个，目录带 '/'
NOMI_IMPORT(fs_size) int32_t fs_size(const char *path);
NOMI_IMPORT(fs_read) int32_t fs_read(const char *path, int32_t offset, void *buf, int32_t size);
// （api 2）在卡上 dir（sd:/ 开头）下面、含子文件夹找文件：扩展名是 exts 之一（逗号分开，"txt" 或 "mp3,flac"）、
// 不小于 min_size 字节。一行一个「路径 TAB 大小」（路径是 sd:/ 开头的），放不下的不放；返回放了几个。
// 跳过 . 开头的、系统的文件夹（nomi/、System Volume Information）。卡上东西多的话要几秒
NOMI_IMPORT(fs_find) int32_t fs_find(const char *dir, const char *exts, int32_t min_size, char *buf, int32_t size);
// （api 3）写：data:/ 随便写，sd:/ 要 sd.write。append 为 0 从头写（原来的内容不要了），1 接在后面；
// 上一级文件夹没有就建。返回写了几个字节
NOMI_IMPORT(fs_write) int32_t fs_write(const char *path, const void *buf, int32_t size, int32_t append);
NOMI_IMPORT(fs_remove) int32_t fs_remove(const char *path);  // （api 3）删文件或整个文件夹
// （api 3）改名 / 挪地方：只能在同一处挪（都在 data:/、或者都在卡上），目标已经有了不覆盖
NOMI_IMPORT(fs_rename) int32_t fs_rename(const char *from, const char *to);
NOMI_IMPORT(fs_mkdir) int32_t fs_mkdir(const char *path);  // （api 3）一层层建，已经有了也算成功
NOMI_IMPORT(kv_get) int32_t kv_get(const char *key, char *buf, int32_t size);  // 没有返回 NOMI_E_NOT_FOUND
NOMI_IMPORT(kv_set) int32_t kv_set(const char *key, const char *value);

/* ---------- 打开文件、选文件（api 3）：「文件」里点了卡上的文件、网页文件管理里点了「设备上打开」 ---------- */

// manifest 里 "opens": ["txt"] 声明能打开哪些扩展名（小写、不带点，要 sd.read）。能开的应用有好几个时系统让人选
// （能记住），选中的应用界面挂上之后来 NOMI_EV_OPEN（新起的进程先来 SHOW）。这时用 open_path 取路径（sd:/ 开头），
// 直接显示这个文件：小说翻到这本书、图片看这张、音乐放这首
NOMI_IMPORT(open_path) int32_t open_path(char *buf, int32_t size);

// 选文件：系统的文件列表（和设备上的「文件」一个样），占当前页的一块（放在页面里，跟着页面上下滚）。从 dir
// （sd:/ 开头，也是最上能到哪，"sd:/" 是整张卡）往下一层层点，顶上是路径和「上一级」。只列 exts 里的扩展名
// （"txt" 或 "mp3,flac"，空字符串 = 都列；文件夹都列）。点了文件来 NOMI_EV_FILE（value 0），flags 有
// NOMI_FILES_PICK_DIR 时每层顶上多一行「选这个文件夹」，选了来 NOMI_EV_FILE（value 1）。用 files_picked 取路径。
// 要 sd.read
#define NOMI_FILES_PICK_DIR 1
NOMI_IMPORT(ui_files) int32_t ui_files(const char *dir, const char *exts, int32_t flags);
NOMI_IMPORT(files_picked) int32_t files_picked(char *buf, int32_t size);

/* ---------- 播放（权限 media）：系统只管放"这一首"，放哪首、下一首放什么（队列）是应用的事 ---------- */

enum { NOMI_MEDIA_PAUSE = 1, NOMI_MEDIA_RESUME = 2, NOMI_MEDIA_STOP = 5, NOMI_MEDIA_TOGGLE = 6 };  // media_control
#define NOMI_PLAY_LIVE 1     // media_play 的 flags：直播（电台，没有进度、断了自动重连）
#define NOMI_PLAY_EPISODE 2  // 节目（播客）：按节目显示，左右键是倒回 15 秒 / 快进 30 秒；不写是一首歌

// NOMI_EV_MEDIA 的 value（只发给放这一首的应用；应用没在跑时系统先在后台把它拉起来）
enum {
    NOMI_MEDIA_FINISHED = 1,  // 这一首放完了：接着放什么你定（不放就停着）
    NOMI_MEDIA_NEXT = 2,      // 主人在系统的播放控件（ui_player、收听卡片）上点了下一首
    NOMI_MEDIA_PREV = 3,      // 上一首
    NOMI_MEDIA_PLAY = 4,      // 主人点了你的 ui_player / 首页卡片上的"放"，但停着的不是你的：你决定放什么（比如接着放上次的）
};

// 放一首 / 一期：src 是 sd:/ 的文件（还要 sd.read）或 http(s) 地址；title、artist 显示用（SD 卡上的歌系统会读
// 标签换掉；节目的 artist 写播客名）。start 从第几秒放；duration、bytes 是总长（秒）和文件大小（字节，网上的
// MP3 按时间跳转要用），不知道填 0。网上的歌系统先探一下（https 的先试 http、按文件头认格式），不是音频返回
// NOMI_E_NOT_FOUND，好换下一首。放的东西记在你名下：退出界面也接着放，放完来 NOMI_MEDIA_FINISHED
NOMI_IMPORT(media_play) int32_t media_play(const char *src, const char *title, const char *artist, int32_t flags,
                                           int32_t start, int32_t duration, int32_t bytes);
// 网上的歌、节目：给正在放的这个配封面（JPEG 地址）和 LRC 歌词，都可以空。SD 卡上的歌系统自己读内嵌封面和同名 .lrc
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

// GET 一个 http(s) 地址（跟着跳转），正文最多 size 字节（够了就断开，一次最多 512KB）；返回字节数，最多等 20 秒。
// 会卡住几秒（你自己的线程，界面不卡）
NOMI_IMPORT(net_get) int32_t net_get(const char *url, char *buf, int32_t size);
// 同上，收到 until 出现 count 次就不往下收了：RSS 有好几 MB，只要最新 10 期就 net_get_until(url, buf, size, "</item>", 10)
NOMI_IMPORT(net_get_until) int32_t net_get_until(const char *url, char *buf, int32_t size, const char *until,
                                                 int32_t count);

/* ---------- 大模型（权限 assistant） ---------- */

// 前台开着你的界面时，每轮对话的系统提示里带上这段（比如"正在看《xx》第 3 章"），最多 600 字节
NOMI_IMPORT(assistant_context) void assistant_context(const char *text);
// 收到 NOMI_EV_VOICE 时：取参数、回话（回话要在这次 nomi_event 里给；end = 1 时这句说完就回待机，
// 比如开始放歌了——不然对话还开着，声音出不来）
NOMI_IMPORT(voice_arg) int32_t voice_arg(char *buf, int32_t size);
NOMI_IMPORT(voice_reply) void voice_reply(const char *text, int32_t end);

/* ---------- 网页：应用自带一个网页设置页 ---------- */

// 包里放 assets/web/index.html（还可以有别的文件），设备的网页设置页「设备 → 应用」里就有「设置」，打开的是
// http://<设备>/app/<id>/。网页里 POST 到相对地址 call 的请求体交给应用（NOMI_EV_WEB），应用回的内容（一般是
// JSON，最多 32KB）原样回给网页；要在这次 nomi_event 里回，系统最多等 60 秒（可以先联网搜一下再回）
NOMI_IMPORT(web_body) int32_t web_body(char *buf, int32_t size);
NOMI_IMPORT(web_reply) void web_reply(const char *text);

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
