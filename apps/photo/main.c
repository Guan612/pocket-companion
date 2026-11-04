// 图片：看 SD 卡里的照片。
//
// 系统给能力：解码（JPEG 边读边解、按 EXIF 转正）、缩略图（ui_thumb，解好的缓存在卡上）、全屏看图（ui_photo）、
// 电子相框（photo_frame：设备睡着时系统自己轮播）、媒体库（library_list：卡上的图片系统记着）、
// 打开文件（「文件」里点了图片，NOMI_EV_OPEN）。应用管业务：分相册、顺序、菜单、语音。
// - 相册：卡上的 .jpg / .jpeg / .png（大于 20KB；cover、folder 这类专辑封面不算），按所在文件夹分，
//   相册里按文件名排。每次打开问一遍媒体库（卡没变时马上就有）；「重新找照片」让系统重扫整张卡。
// - 「文件」里点了一张：打开它所在的相册、看这张；不算照片的（小图、封面）单独看这一张。
// - 相册里：最上面「设成电子相框」，下面缩略图一页 16 张，上一页 / 下一页。
// - 看图：翻页方式四选一（默认左右滑、点左右）；点中间（或长按）出菜单：电子相框、换图间隔、翻页方式、回相册。
// kv：last（上次看的相册）、frame（相框相册）、frame_sec（换图间隔）、turn（翻页方式）。
#include "nomi_sdk.h"
#include "nomi_text.h"

#define MAX_PHOTOS 2000
#define MAX_ALBUMS 200
#define PER_PAGE 16
#define MIN_PHOTO 20480
#define NAME_LEN 128
#define PATH_LEN 256

enum { TIMER_SCAN = 1 };
enum { VOICE_OPEN = 1, VOICE_FRAME };

static char g_buf[160 * 1024];       // 媒体库给的图片列表，原地切成一条条路径
static int32_t g_photo[MAX_PHOTOS];  // 每张照片的路径在 g_buf 里的位置（按相册、文件名排好）
static int32_t g_tmp[MAX_PHOTOS];    // 排序用
static int g_photo_count;
static struct {
    int first, count;
} g_albums[MAX_ALBUMS];
static int g_album_count;
static int g_scanned;

static char g_frame[PATH_LEN];  // 相框相册（sd:/… 文件夹），空 = 没设
static int g_frame_sec = 60;
static const int kIntervals[] = {30, 60, 300, 900, 3600};
static const char *const kIntervalNames[] = {"30 秒", "1 分钟", "5 分钟", "15 分钟", "1 小时"};
#define INTERVALS 5

#define TURN_CHOICES 4
static const int kTurns[TURN_CHOICES] = {NOMI_PHOTO_SWIPE_LR | NOMI_PHOTO_TAP_LR, NOMI_PHOTO_SWIPE_LR,
                                         NOMI_PHOTO_SWIPE_UD, NOMI_PHOTO_TAP_LR};
static const char *const kTurnNames[TURN_CHOICES] = {"左右滑、点左右", "只左右滑", "上下滑", "只点左右"};
static int g_turn = NOMI_PHOTO_SWIPE_LR | NOMI_PHOTO_TAP_LR;

// 界面：g_pages 是压了几页（0 = 界面没挂着）。1 相册列表、2 一个相册、3 看图、4 菜单、5 翻页方式
static int g_pages;
static int32_t g_list_page, g_note, g_rescan;
static int32_t g_album_rows[MAX_ALBUMS];
static int g_album_row_count;
static int g_album = -1;  // 打开的相册
static int g_grid_first;
static int32_t g_frame_row, g_prev_row, g_next_row;
static int32_t g_thumbs[PER_PAGE];
static int g_thumb_count;
static int g_index;  // 看到相册里第几张
static int32_t g_viewer;
static int32_t g_menu_frame, g_menu_interval, g_menu_turn, g_menu_back;
static int32_t g_turn_rows[TURN_CHOICES];
static char g_pending[PATH_LEN];  // 语音要打开的相册：界面挂上（SHOW）后再开

/* ---------- 小工具 ---------- */

static void copy(char *dst, const char *src, int size) { nomi_copy_n(dst, size, src, 0); }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int contains(const char *s, const char *word)
{
    if (!word[0]) return 1;
    for (; *s; s++) {
        int i = 0;
        while (word[i] && lower(s[i]) == lower(word[i])) i++;
        if (!word[i]) return 1;
    }
    return 0;
}

static int starts_nocase(const char *s, const char *prefix)
{
    while (*prefix && lower(*s) == *prefix) s++, prefix++;
    return !*prefix;
}

static const char *path_of(int photo) { return g_buf + g_photo[photo]; }

// 最后一个 '/' 的位置
static int last_slash(const char *s)
{
    int at = -1;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '/') at = i;
    }
    return at;
}

// 先按文件夹、再按文件名
static int compare_paths(const char *a, const char *b)
{
    int da = last_slash(a), db = last_slash(b);
    int n = da < db ? da : db, i = 0;
    while (i < n && a[i] == b[i]) i++;
    if (i < n || da != db) {
        if (i == n) return da - db;
        return (unsigned char)a[i] - (unsigned char)b[i];
    }
    a += da + 1;
    b += db + 1;
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

static void sort_photos(void)  // 自底向上归并排序
{
    for (int width = 1; width < g_photo_count; width *= 2) {
        for (int lo = 0; lo < g_photo_count; lo += 2 * width) {
            int mid = lo + width < g_photo_count ? lo + width : g_photo_count;
            int hi = lo + 2 * width < g_photo_count ? lo + 2 * width : g_photo_count;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                g_tmp[k++] = compare_paths(path_of(i), path_of(j)) <= 0 ? g_photo[i++] : g_photo[j++];
            }
            while (i < mid) g_tmp[k++] = g_photo[i++];
            while (j < hi) g_tmp[k++] = g_photo[j++];
        }
        for (int i = 0; i < g_photo_count; i++) g_photo[i] = g_tmp[i];
    }
}

// 相册的文件夹（sd:/…，不带最后的 /）
static void album_dir(int album, char *out, int size)
{
    const char *p = path_of(g_albums[album].first);
    int at = last_slash(p);
    nomi_copy_n(out, size, p, p + (at > 0 ? at : 0));
}

static void album_name(int album, char *out, int size)
{
    char dir[PATH_LEN];
    album_dir(album, dir, sizeof(dir));
    int at = last_slash(dir);
    copy(out, nomi_equal(dir, "sd:") ? "SD 卡根上" : dir + at + 1, size);
}

static void file_name(int photo, char *out, int size)
{
    const char *p = path_of(photo);
    copy(out, p + last_slash(p) + 1, size);
}

static int is_frame_album(int album)
{
    if (!g_frame[0] || album < 0) return 0;
    char dir[PATH_LEN];
    album_dir(album, dir, sizeof(dir));
    return nomi_equal(dir, g_frame);
}

/* ---------- 找照片、分相册 ---------- */

// 专辑封面、缩略图这类不算照片
static int is_cover(const char *name)
{
    return starts_nocase(name, "cover") || starts_nocase(name, "folder") || starts_nocase(name, "front") ||
           starts_nocase(name, "albumart") || starts_nocase(name, "thumb") || name[0] == '.';
}

// 问媒体库要卡上的图片（卡变了系统要扫一遍，几秒；rescan 不管记着的、重扫），分成相册。
// 一次取不完分几次取，接着放进 g_buf；不要的（小图、封面）当场挤掉
static int scan_photos(int rescan)
{
    g_photo_count = g_album_count = 0;
    g_scanned = 1;
    int32_t used = 0, first = 0, total = 1;
    int kind = NOMI_LIB_IMAGE | (rescan ? NOMI_LIB_RESCAN : 0);
    while (first < total && g_photo_count < MAX_PHOTOS && used < (int32_t)sizeof(g_buf) - 1024) {
        total = library_list(kind, first, g_buf + used, (int32_t)sizeof(g_buf) - used);
        kind = NOMI_LIB_IMAGE;  // 重扫一次就够
        if (total < 0) return total;
        // 「路径\t大小\n」一行一个：留下的路径（带结尾）往前挪到 w，记下位置
        int got = 0;
        int32_t w = used;
        char *p = g_buf + used;
        while (*p) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p) *p++ = 0;
            got++;
            char *tab = line;
            while (*tab && *tab != '\t') tab++;
            int32_t size = *tab ? nomi_atoi(tab + 1) : 0;
            *tab = 0;
            if (size < MIN_PHOTO || is_cover(line + last_slash(line) + 1) || g_photo_count >= MAX_PHOTOS) continue;
            int32_t len = (int32_t)(tab - line) + 1;
            char *dst = g_buf + w;
            for (int32_t k = 0; k < len && dst != line; k++) dst[k] = line[k];
            g_photo[g_photo_count++] = w;
            w += len;
        }
        if (got == 0) break;
        first += got;
        used = w;
    }
    sort_photos();
    // 排好序以后同一个文件夹的挨在一起：一段就是一个相册
    for (int i = 0; i < g_photo_count && g_album_count < MAX_ALBUMS; i++) {
        int same = 0;
        if (g_album_count) {
            const char *a = path_of(i), *b = path_of(g_albums[g_album_count - 1].first);
            int da = last_slash(a);
            same = da == last_slash(b);
            for (int k = 0; same && k < da; k++) same = a[k] == b[k];
        }
        if (same) {
            g_albums[g_album_count - 1].count++;
        } else {
            g_albums[g_album_count].first = i;
            g_albums[g_album_count].count = 1;
            g_album_count++;
        }
    }
    return g_photo_count;
}

static int find_album(const char *query)
{
    char name[NAME_LEN];
    for (int a = 0; a < g_album_count; a++) {
        album_name(a, name, sizeof(name));
        if (contains(name, query)) return a;
    }
    return -1;
}

static int album_by_dir(const char *dir)
{
    char d[PATH_LEN];
    for (int a = 0; a < g_album_count; a++) {
        album_dir(a, d, sizeof(d));
        if (nomi_equal(d, dir)) return a;
    }
    return -1;
}

/* ---------- 界面 ---------- */

static int32_t push_page(const char *title)
{
    int32_t id = ui_page(title);
    if (id > 0) g_pages++;
    return id;
}

static void close_to(int pages)
{
    while (g_pages > pages && g_pages > 1) {
        ui_page_close();
        g_pages--;
    }
}

static const char *interval_name(int sec)
{
    for (int i = 0; i < INTERVALS; i++) {
        if (kIntervals[i] == sec) return kIntervalNames[i];
    }
    return kIntervalNames[1];
}

static const char *turn_name(int turn)
{
    for (int i = 0; i < TURN_CHOICES; i++) {
        if (kTurns[i] == turn) return kTurnNames[i];
    }
    return kTurnNames[0];
}

static void update_context(void)
{
    char ctx[400], name[NAME_LEN], file[NAME_LEN];
    if (g_album < 0 || g_pages < 2) {
        assistant_context("");
        return;
    }
    album_name(g_album, name, sizeof(name));
    if (g_viewer) {
        file_name(g_albums[g_album].first + g_index, file, sizeof(file));
        nomi_fmt(ctx, sizeof(ctx), "主人在看相册「%s」第 %d / %d 张（%s）", name, g_index + 1,
                 g_albums[g_album].count, file);
    } else {
        nomi_fmt(ctx, sizeof(ctx), "主人在看相册「%s」（%d 张）", name, g_albums[g_album].count);
    }
    assistant_context(ctx);
}

// 相册列表（第 1 页）重画
static void draw_list(void)
{
    if (g_pages < 1) return;
    ui_target(g_list_page);
    for (int i = 0; i < g_album_row_count; i++) ui_remove(g_album_rows[i]);
    g_album_row_count = 0;
    if (g_note) ui_remove(g_note);
    if (g_rescan) ui_remove(g_rescan);
    g_note = g_rescan = 0;
    if (!g_scanned) {
        g_note = ui_text("正在找卡上的照片…", NOMI_TEXT_MUTED);
        return;
    }
    if (g_album_count == 0) g_note = ui_text("SD 卡里没找到照片（.jpg / .png，大于 20KB 的）", NOMI_TEXT_MUTED);
    char name[NAME_LEN], sub[64];
    for (int a = 0; a < g_album_count; a++) {
        album_name(a, name, sizeof(name));
        nomi_fmt(sub, sizeof(sub), is_frame_album(a) ? "%d 张 · 电子相框" : "%d 张", g_albums[a].count);
        g_album_rows[g_album_row_count++] = ui_item(name, sub, ">");
    }
    g_rescan = ui_item("重新找照片", "整张卡重新找一遍", "");
}

static void show_list(void)
{
    g_pages = 0;
    g_album = -1;
    g_viewer = 0;
    g_album_row_count = 0;
    g_note = g_rescan = 0;
    g_list_page = push_page("图片");
    draw_list();
    if (!g_scanned) timer_start(TIMER_SCAN, 200, 0);  // 先把「正在找」画出来
}

// 一个相册（第 2 页）：电子相框、缩略图（从 first 起一页）、上一页 / 下一页
static void show_album(int album, int first)
{
    if (album < 0 || album >= g_album_count) return;
    close_to(1);
    g_album = album;
    g_grid_first = first;
    char name[NAME_LEN], title[NAME_LEN + 32], dir[PATH_LEN], range[64];
    album_name(album, name, sizeof(name));
    album_dir(album, dir, sizeof(dir));
    kv_set("last", dir);
    int count = g_albums[album].count;
    nomi_fmt(title, sizeof(title), "%s（%d 张）", name, count);
    int32_t page = push_page(title);
    g_frame_row = is_frame_album(album) ? ui_item("已是电子相框", "点一下取消", "")
                                        : ui_item("设成电子相框", "设备睡着时轮播这个相册", "");
    int32_t grid = ui_grid();
    ui_target(grid);
    g_thumb_count = 0;
    for (int i = first; i < count && g_thumb_count < PER_PAGE; i++) {
        g_thumbs[g_thumb_count++] = ui_thumb(path_of(g_albums[album].first + i));
    }
    ui_target(page);
    g_prev_row = g_next_row = 0;
    if (count > PER_PAGE) {
        nomi_fmt(range, sizeof(range), "第 %d–%d 张", first + 1, first + g_thumb_count);
        if (first > 0) g_prev_row = ui_item("上一页", range, "");
        if (first + g_thumb_count < count) g_next_row = ui_item("下一页", first > 0 ? "" : range, "");
    }
    update_context();
}

// 预读前后两张：翻过去就快（系统存进卡上的缓存）
static void preload_around(void)
{
    if (g_index + 1 < g_albums[g_album].count) photo_preload(path_of(g_albums[g_album].first + g_index + 1));
    if (g_index > 0) photo_preload(path_of(g_albums[g_album].first + g_index - 1));
}

static void show_photo(int index)
{
    g_index = index;
    close_to(2);
    push_page("");
    g_viewer = ui_photo(path_of(g_albums[g_album].first + index), g_turn);
    if (g_viewer < 0) {
        g_viewer = 0;
        close_to(2);
        return;
    }
    preload_around();
    update_context();
}

static void set_frame(int album)
{
    char dir[PATH_LEN], name[NAME_LEN], msg[NAME_LEN + 64];
    if (album < 0) {
        g_frame[0] = 0;
        kv_set("frame", "");
        photo_frame("", 0);
        ui_toast("不用电子相框了，睡着时回到时钟");
        return;
    }
    album_dir(album, dir, sizeof(dir));
    album_name(album, name, sizeof(name));
    copy(g_frame, dir, PATH_LEN);
    kv_set("frame", dir);
    photo_frame(dir, g_frame_sec);
    nomi_fmt(msg, sizeof(msg), "设好了：设备睡着时轮播「%s」", name);
    ui_toast(msg);
}

static void show_menu(void)
{
    char title[48], file[NAME_LEN], interval[48];
    nomi_fmt(title, sizeof(title), "第 %d / %d 张", g_index + 1, g_albums[g_album].count);
    push_page(title);
    g_menu_frame = is_frame_album(g_album) ? ui_item("已是电子相框", "点一下取消", "")
                                           : ui_item("设成电子相框", "设备睡着时轮播这个相册", "");
    nomi_fmt(interval, sizeof(interval), "相框换图：%s", interval_name(g_frame_sec));
    g_menu_interval = ui_item(interval, "点一下换", "");
    g_menu_turn = ui_item("翻页方式", turn_name(g_turn), ">");
    g_menu_back = ui_item("回相册", "", "");
    file_name(g_albums[g_album].first + g_index, file, sizeof(file));
    ui_text(file, NOMI_TEXT_MUTED);
}

static void show_turn(void)
{
    push_page("翻页方式");
    ui_text("不管哪种，点屏幕中间都是出菜单", NOMI_TEXT_MUTED);
    for (int i = 0; i < TURN_CHOICES; i++) g_turn_rows[i] = ui_item(kTurnNames[i], "", kTurns[i] == g_turn ? "在用" : "");
}

/* ---------- 语音 ---------- */

#define REPLY(text, end)        \
    do {                        \
        voice_reply(text, end); \
        return;                 \
    } while (0)

// 「文件」里点了一张：在相册里就打开那个相册、翻到那一页、看这张；不在（小图、封面、刚拷的）单独看
static void open_file(const char *path)
{
    for (int a = 0; a < g_album_count; a++) {
        for (int i = 0; i < g_albums[a].count; i++) {
            if (!nomi_equal(path_of(g_albums[a].first + i), path)) continue;
            show_album(a, i / PER_PAGE * PER_PAGE);
            show_photo(i);
            return;
        }
    }
    close_to(1);
    g_album = -1;  // 不在相册里：翻不了上一张下一张，也没有菜单，左边缘右划回列表
    push_page("");
    g_viewer = ui_photo(path, g_turn);
    if (g_viewer < 0) {
        g_viewer = 0;
        close_to(1);
        ui_toast("这张图打不开");
    }
}

static void open_album(int album)
{
    if (g_pages < 1) {  // 界面没挂着：挂上再开
        album_dir(album, g_pending, PATH_LEN);
        ui_open();
        return;
    }
    show_album(album, 0);
}

static void voice(int command)
{
    char arg[200], reply[300], name[NAME_LEN], last[PATH_LEN];
    voice_arg(arg, sizeof(arg));
    if (!g_scanned) scan_photos(0);
    if (command == VOICE_OPEN) {
        int album = arg[0] ? find_album(arg) : (kv_get("last", last, sizeof(last)) > 0 ? album_by_dir(last) : -1);
        if (album < 0) {
            nomi_fmt(reply, sizeof(reply), arg[0] ? "SD 卡里没找到「%s」这个相册" : "还没看过相册，问问主人想看哪个", arg);
            REPLY(reply, 0);
        }
        album_name(album, name, sizeof(name));
        open_album(album);
        nomi_fmt(reply, sizeof(reply), "好的，打开相册「%s」", name);
        REPLY(reply, 1);
    }
    if (command == VOICE_FRAME) {
        if (contains(arg, "取消") || contains(arg, "不要")) {
            set_frame(-1);
            REPLY("好的，不用电子相框了", 1);
        }
        int album = arg[0] ? find_album(arg) : g_album;
        if (album < 0) {
            nomi_fmt(reply, sizeof(reply), arg[0] ? "SD 卡里没找到「%s」这个相册" : "先说要哪个相册", arg);
            REPLY(reply, 0);
        }
        set_frame(album);
        album_name(album, name, sizeof(name));
        nomi_fmt(reply, sizeof(reply), "好了，设备睡着时轮播「%s」，每 %s 换一张", name, interval_name(g_frame_sec));
        REPLY(reply, 1);
    }
}

/* ---------- 入口 ---------- */

void nomi_main(void)
{
    g_turn = kv_get_int("turn", g_turn);
    int ok = 0;
    for (int i = 0; i < TURN_CHOICES; i++) ok |= kTurns[i] == g_turn;
    if (!ok) g_turn = kTurns[0];
    g_frame_sec = kv_get_int("frame_sec", 60);
    if (kv_get("frame", g_frame, sizeof(g_frame)) <= 0) g_frame[0] = 0;
}

int32_t nomi_event(int32_t type, int32_t id, int32_t value)
{
    switch (type) {
    case NOMI_EV_SHOW:
        show_list();
        if (g_pending[0]) {
            if (!g_scanned) scan_photos(0);
            draw_list();
            int album = album_by_dir(g_pending);
            g_pending[0] = 0;
            if (album >= 0) show_album(album, 0);
        }
        return 1;
    case NOMI_EV_HIDE:
        g_pages = 0;
        g_viewer = 0;
        g_scanned = 0;  // 下次打开再问一遍媒体库（可能新拷了照片）
        assistant_context("");
        return 1;
    case NOMI_EV_OPEN: {  // 「文件」里点了一张（界面已经挂上、列表建好了）
        char path[PATH_LEN];
        if (open_path(path, sizeof(path)) <= 0) return 1;
        if (!g_scanned) {
            scan_photos(0);
            draw_list();
        }
        open_file(path);
        return 1;
    }
    case NOMI_EV_TIMER:
        if (id == TIMER_SCAN && g_pages >= 1) {
            scan_photos(0);
            draw_list();
        }
        return 1;
    case NOMI_EV_BACK:
        g_pages = value;
        if (g_pages < 3) g_viewer = 0;
        if (g_pages < 2) g_album = -1;
        update_context();
        return 1;
    case NOMI_EV_PHOTO:
        if (id != g_viewer || g_album < 0) return 1;
        if (value == NOMI_PHOTO_MENU) {
            if (g_pages == 3) show_menu();
        } else if (value == NOMI_PHOTO_NEXT) {
            if (g_index + 1 >= g_albums[g_album].count) {
                ui_toast("已经是最后一张");
            } else {
                photo_show(g_viewer, path_of(g_albums[g_album].first + ++g_index));
                preload_around();
                update_context();
            }
        } else if (value == NOMI_PHOTO_PREV) {
            if (g_index == 0) {
                ui_toast("已经是第一张");
            } else {
                photo_show(g_viewer, path_of(g_albums[g_album].first + --g_index));
                preload_around();
                update_context();
            }
        }
        return 1;
    case NOMI_EV_VOICE:
        voice(id);
        return 1;
    case NOMI_EV_CLICK:
        break;
    default:
        return 0;
    }
    // 点击
    if (g_pages == 1) {
        if (id == g_rescan) {
            ui_toast("正在整张卡找照片…");
            scan_photos(1);
            draw_list();
            return 1;
        }
        for (int a = 0; a < g_album_row_count; a++) {
            if (id == g_album_rows[a]) show_album(a, 0);
        }
        return 1;
    }
    if (g_pages == 2) {
        if (id == g_frame_row) {
            set_frame(is_frame_album(g_album) ? -1 : g_album);
            show_album(g_album, g_grid_first);  // 重画，第一行换成新的状态
        } else if (id == g_prev_row || id == g_next_row) {
            int first = g_grid_first + (id == g_next_row ? PER_PAGE : -PER_PAGE);
            show_album(g_album, first < 0 ? 0 : first);
        } else {
            for (int i = 0; i < g_thumb_count; i++) {
                if (id == g_thumbs[i]) show_photo(g_grid_first + i);
            }
        }
        return 1;
    }
    if (g_pages == 4) {
        if (id == g_menu_frame) {
            set_frame(is_frame_album(g_album) ? -1 : g_album);
            close_to(3);
        } else if (id == g_menu_interval) {
            int i = 0;
            while (i < INTERVALS && kIntervals[i] != g_frame_sec) i++;
            g_frame_sec = kIntervals[(i + 1) % INTERVALS];
            kv_set_int("frame_sec", g_frame_sec);
            if (g_frame[0]) photo_frame(g_frame, g_frame_sec);
            char interval[48];
            nomi_fmt(interval, sizeof(interval), "相框换图：%s", interval_name(g_frame_sec));
            ui_set_text(g_menu_interval, interval);
        } else if (id == g_menu_turn) {
            show_turn();
        } else if (id == g_menu_back) {
            g_viewer = 0;
            close_to(2);
            update_context();
        }
        return 1;
    }
    if (g_pages == 5) {
        for (int i = 0; i < TURN_CHOICES; i++) {
            if (id == g_turn_rows[i]) {
                g_turn = kTurns[i];
                kv_set_int("turn", g_turn);
                photo_set_flags(g_viewer, g_turn);
                close_to(3);  // 回到图，直接试新的翻页方式
            }
        }
        return 1;
    }
    return 1;
}
