// 小说：看 SD 卡里的 TXT。
//
// 系统给能力：排版、分页、翻页、认编码（UTF-8 / GBK）、缺字用卡上的字体补（ui_reader），找章节（text_chapters），
// 整张卡找文件（fs_find）。应用管业务：书架、书签、目录、菜单、语音指令。
// - 书架：整张卡找 .txt（大于 16KB 的，小的多半是说明、歌词），最近看的排前面，右边是读到百分之几。
//   上次找到的记在 kv 里，打开就先显示，再在后台重新找一遍。
// - 看书：一整页都是阅读器；点屏幕中间（或长按）出快速跳转：拖进度、上一章 / 下一章、目录、翻页方式、回书架。
//   翻页方式五选一：滑 + 点上下（默认）、滑 + 点左右、只滑、只点上下、只点左右。左边缘右划回书架。
// - 书签：每本书读到哪（文件里的字节位置）记在 kv "p.<路径哈希>"，翻页后 10 秒内存一次，退出时也存。
// kv：books（上次找到的书，一行「路径\t大小」）、recent（最近看的书，一行一个路径）、turn（翻页方式）、p.*（书签）、
//     tip（看过「点中间出菜单」的提示了）。
#include "nomi_sdk.h"
#include "nomi_text.h"

#define MAX_BOOKS 200
#define PATH_LEN 256
#define MIN_BOOK 16384
#define MAX_RECENT 20
#define TOC_BATCH 40
#define TITLE_LEN 128

enum { TIMER_SCAN = 1, TIMER_SAVE = 2 };
enum { VOICE_OPEN = 1, VOICE_NEXT, VOICE_PREV, VOICE_CHAPTER };

static char g_buf[48 * 1024];  // fs_find、目录（同一时间只做一件事）
static char g_line[512];

static struct {
    char path[PATH_LEN];
    int32_t size;
} g_books[MAX_BOOKS];
static int g_book_count;
static int g_scanned;  // 这次开着的时候找过书了

static char g_recent[MAX_RECENT][PATH_LEN];
static int g_recent_count;

static int g_turn = NOMI_READER_SWIPE | NOMI_READER_TAP;

// 界面：页面一层层压，g_pages 是压了几页（0 = 界面没挂着）。1 书架、2 看书、3 菜单、4 目录
static int g_pages;
static int32_t g_shelf;
static int32_t g_shelf_rows[MAX_BOOKS];
static int g_shelf_book[MAX_BOOKS];
static int g_shelf_count;
static int32_t g_scan_row, g_note;

// 在看的书
static char g_book[PATH_LEN];
static int32_t g_reader;  // 阅读器（0 = 没在看）
static int32_t g_book_size;
static int32_t g_offset;  // 这一页开头
static int g_dirty;       // 书签还没存
static int g_chapter = -2;  // 这一页在第几章（-2 还没算，-1 第一章之前 / 没分章）
static char g_chapter_title[TITLE_LEN];
static char g_pending[PATH_LEN];  // 语音要打开的书：界面挂上（SHOW）后再打开

// 菜单（快速跳转）、目录、翻页方式。第 4 页是目录还是翻页方式看 g_page4
static int32_t g_menu_slider, g_menu_prev, g_menu_next, g_menu_toc, g_menu_turn, g_menu_shelf;
static int32_t g_prev_at, g_next_at;  // 上一章 / 下一章的位置
enum { PAGE4_NONE, PAGE4_TOC, PAGE4_TURN };
static int g_page4;
#define TURN_CHOICES 5
static const int kTurns[TURN_CHOICES] = {
    NOMI_READER_SWIPE | NOMI_READER_TAP, NOMI_READER_SWIPE | NOMI_READER_TAP_LR, NOMI_READER_SWIPE, NOMI_READER_TAP,
    NOMI_READER_TAP_LR};
static const char *const kTurnNames[TURN_CHOICES] = {"上下滑、点上下", "上下滑、点左右", "只上下滑", "只点上下",
                                                     "只点左右"};
static int32_t g_turn_rows[TURN_CHOICES];
static int32_t g_toc_rows[TOC_BATCH];
static int32_t g_toc_offsets[TOC_BATCH];
static int g_toc_count, g_toc_first;
static int32_t g_toc_prev, g_toc_next;

/* ---------- 小工具 ---------- */

static void copy(char *dst, const char *src, int size) { nomi_copy_n(dst, size, src, 0); }

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

// s 里有没有 word（英文不分大小写）
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

// 截掉末尾不完整的 UTF-8 字（nomi_fmt 放不下时是按字节截的）
static void fix_tail(char *s)
{
    int n = nomi_strlen(s), i = n;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) return;
    unsigned char c = (unsigned char)s[i - 1];
    int need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (n - (i - 1) < need) s[i - 1] = 0;
}

// 书名：文件名去掉 .txt
static void book_name(const char *path, char *out, int size)
{
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') name = p + 1;
    }
    copy(out, name, size);
    int n = nomi_strlen(out);
    if (n > 4 && out[n - 4] == '.' && lower(out[n - 3]) == 't' && lower(out[n - 2]) == 'x' && lower(out[n - 1]) == 't') {
        out[n - 4] = 0;
    }
}

// 在哪个文件夹（卡的根上是空的）
static void book_folder(const char *path, char *out, int size)
{
    const char *start = nomi_starts(path, "sd:/") ? path + 4 : path;
    const char *slash = 0;
    for (const char *p = start; *p; p++) {
        if (*p == '/') slash = p;
    }
    out[0] = 0;
    if (slash) nomi_copy_n(out, size, start, slash);
}

static void book_key(const char *path, char out[16])
{
    char hex[9];
    nomi_hex32(nomi_hash(path), hex);
    nomi_fmt(out, 16, "p.%s", hex);
}

static int32_t saved_offset(const char *path)
{
    char key[16];
    book_key(path, key);
    return kv_get_int(key, 0);
}

static void save_offset(void)
{
    if (!g_dirty || !g_book[0]) return;
    char key[16];
    book_key(g_book, key);
    kv_set_int(key, g_offset);
    g_dirty = 0;
}

/* ---------- 书架：整张卡找书、最近看的 ---------- */

// 「路径\t大小」一行一本，读进 g_books，按书名排
static void parse_books(char *text)
{
    g_book_count = 0;
    char *p = text;
    while (*p && g_book_count < MAX_BOOKS) {
        char *end = p;
        while (*end && *end != '\n') end++;
        char *tab = p;
        while (tab < end && *tab != '\t') tab++;
        if (tab < end) {
            nomi_copy_n(g_books[g_book_count].path, PATH_LEN, p, tab);
            char num[16];
            nomi_copy_n(num, sizeof(num), tab + 1, end);
            g_books[g_book_count].size = nomi_atoi(num);
            g_book_count++;
        }
        p = *end ? end + 1 : end;
    }
    // 插入排序：按书名
    for (int i = 1; i < g_book_count; i++) {
        char a[TITLE_LEN], b[TITLE_LEN];
        for (int j = i; j > 0; j--) {
            book_name(g_books[j - 1].path, a, sizeof(a));
            book_name(g_books[j].path, b, sizeof(b));
            int k = 0;
            while (a[k] && a[k] == b[k]) k++;
            if ((unsigned char)a[k] <= (unsigned char)b[k]) break;
            char tmp_path[PATH_LEN];
            int32_t tmp_size = g_books[j].size;
            copy(tmp_path, g_books[j].path, PATH_LEN);
            copy(g_books[j].path, g_books[j - 1].path, PATH_LEN);
            g_books[j].size = g_books[j - 1].size;
            copy(g_books[j - 1].path, tmp_path, PATH_LEN);
            g_books[j - 1].size = tmp_size;
        }
    }
}

static void load_books(void)
{
    if (kv_get("books", g_buf, sizeof(g_buf)) > 0) parse_books(g_buf);
}

// 整张卡找一遍（几秒）；找到的记进 kv。返回找到几本，没插卡等返回 < 0
static int scan_books(void)
{
    int n = fs_find("sd:/", "txt", MIN_BOOK, g_buf, sizeof(g_buf) - 1);
    if (n < 0) return n;
    kv_set("books", g_buf);
    parse_books(g_buf);
    g_scanned = 1;
    return g_book_count;
}

static void load_recent(void)
{
    g_recent_count = 0;
    if (kv_get("recent", g_buf, sizeof(g_buf)) <= 0) return;
    char *p = g_buf;
    while (*p && g_recent_count < MAX_RECENT) {
        char *end = p;
        while (*end && *end != '\n') end++;
        if (end > p) nomi_copy_n(g_recent[g_recent_count++], PATH_LEN, p, end);
        p = *end ? end + 1 : end;
    }
}

// 刚打开的书挪到最前面
static void touch_recent(const char *path)
{
    int i = 0;
    while (i < g_recent_count && !nomi_equal(g_recent[i], path)) i++;
    if (i == g_recent_count && g_recent_count < MAX_RECENT) g_recent_count++;
    if (i >= MAX_RECENT) i = MAX_RECENT - 1;
    for (; i > 0; i--) copy(g_recent[i], g_recent[i - 1], PATH_LEN);
    copy(g_recent[0], path, PATH_LEN);
    g_buf[0] = 0;
    for (int k = 0; k < g_recent_count; k++) {
        nomi_append(g_buf, sizeof(g_buf), g_recent[k]);
        nomi_append(g_buf, sizeof(g_buf), "\n");
    }
    kv_set("recent", g_buf);
}

static int32_t push_page(const char *title)
{
    int32_t id = ui_page(title);
    if (id > 0) g_pages++;
    return id;
}

// 书架上的一行：书名、文件夹、读到百分之几
static void add_shelf_row(int book)
{
    if (g_shelf_count >= MAX_BOOKS) return;
    char name[TITLE_LEN], folder[TITLE_LEN], right[16];
    book_name(g_books[book].path, name, sizeof(name));
    book_folder(g_books[book].path, folder, sizeof(folder));
    int32_t offset = saved_offset(g_books[book].path);
    right[0] = 0;
    if (offset > 0 && g_books[book].size > 0) {
        int pct = (int)((int64_t)offset * 100 / g_books[book].size);
        nomi_fmt(right, sizeof(right), "%d%%", pct < 1 ? 1 : pct);
    }
    g_shelf_rows[g_shelf_count] = ui_item(name, folder[0] ? folder : "SD 卡", right);
    g_shelf_book[g_shelf_count] = book;
    g_shelf_count++;
}

// 书架重画：最近看的在前，其余按书名
static void draw_shelf(void)
{
    if (g_pages < 1 || g_shelf <= 0) return;
    ui_target(g_shelf);
    for (int i = 0; i < g_shelf_count; i++) ui_remove(g_shelf_rows[i]);
    g_shelf_count = 0;
    if (g_note) ui_remove(g_note);
    if (g_scan_row) ui_remove(g_scan_row);
    g_note = g_scan_row = 0;
    if (g_book_count == 0) {
        g_note = ui_text(g_scanned ? "SD 卡里没找到 .txt 小说。把书拷到卡上任意文件夹，再点「重新找书」"
                                   : "正在找卡上的书…",
                         NOMI_TEXT_MUTED);
    }
    char shown[MAX_BOOKS];
    for (int i = 0; i < g_book_count; i++) shown[i] = 0;
    for (int r = 0; r < g_recent_count; r++) {
        for (int i = 0; i < g_book_count; i++) {
            if (!shown[i] && nomi_equal(g_books[i].path, g_recent[r])) {
                add_shelf_row(i);
                shown[i] = 1;
            }
        }
    }
    for (int i = 0; i < g_book_count; i++) {
        if (!shown[i]) add_shelf_row(i);
    }
    g_scan_row = ui_item("重新找书", "整张卡找 .txt（大于 16KB 的）", "");
}

static void show_shelf(void)
{
    g_pages = 0;
    g_shelf_count = 0;
    g_note = g_scan_row = 0;
    g_reader = 0;
    g_shelf = push_page("小说");
    draw_shelf();
    if (!g_scanned) timer_start(TIMER_SCAN, 300, 0);  // 先把上次的显示出来，再在后台重新找
}

/* ---------- 看书 ---------- */

// 第 index 章的位置和章名（从 0 起）；没有返回 0
static int chapter_info(int index, int32_t *offset, char *title, int size)
{
    if (index < 0 || text_chapters(g_book, index, g_line, sizeof(g_line)) <= index) return 0;
    char *tab = g_line;
    while (*tab && *tab != '\t') tab++;
    if (!*tab) return 0;
    *tab = 0;
    *offset = nomi_atoi(g_line);
    char *end = tab + 1;
    while (*end && *end != '\n') end++;
    nomi_copy_n(title, size, tab + 1, end);
    return 1;
}

static void update_context(void)
{
    if (!g_reader) {
        assistant_context("");
        return;
    }
    char name[TITLE_LEN], page[360], ctx[600];
    book_name(g_book, name, sizeof(name));
    if (reader_text(g_reader, page, sizeof(page)) < 0) page[0] = 0;
    for (char *p = page; *p; p++) {
        if (*p == '\n') *p = ' ';
    }
    nomi_fmt(ctx, sizeof(ctx), "主人正在看小说《%s》%s。这一页：%s", name,
             g_chapter >= 0 ? g_chapter_title : "", page);
    fix_tail(ctx);
    assistant_context(ctx);
}

// 翻到新的一页：左下角换成这一章的章名（第一次要把整本书读一遍找章节）
static void on_page(int32_t offset)
{
    g_offset = offset;
    g_dirty = 1;
    int index = text_chapter_at(g_book, offset);
    if (index != g_chapter) {
        g_chapter = index;
        int32_t at;
        if (index < 0 || !chapter_info(index, &at, g_chapter_title, sizeof(g_chapter_title))) {
            book_name(g_book, g_chapter_title, sizeof(g_chapter_title));
        }
        ui_set_text(g_reader, g_chapter_title);
    }
    update_context();
}

static void close_to(int pages)
{
    while (g_pages > pages && g_pages > 1) {
        ui_page_close();
        g_pages--;
    }
}

static void left_reader(void)
{
    save_offset();
    g_reader = 0;
    g_chapter = -2;
    timer_stop(TIMER_SAVE);
    update_context();
}

static int open_book(const char *path)
{
    if (g_pages < 1) {  // 界面没挂着（语音叫开的）：挂上再开
        copy(g_pending, path, PATH_LEN);
        ui_open();
        return 1;
    }
    if (g_reader) left_reader();
    close_to(1);
    copy(g_book, path, PATH_LEN);
    g_offset = saved_offset(path);
    g_book_size = 0;
    for (int i = 0; i < g_book_count; i++) {
        if (nomi_equal(g_books[i].path, path)) g_book_size = g_books[i].size;
    }
    if (g_book_size <= 0) g_book_size = fs_size(path);
    g_chapter = -2;
    book_name(path, g_chapter_title, sizeof(g_chapter_title));
    push_page("");
    g_reader = ui_reader(path, g_offset, g_turn);
    if (g_reader < 0) {
        g_reader = 0;
        close_to(1);
        return 0;
    }
    ui_set_text(g_reader, g_chapter_title);
    touch_recent(path);
    timer_start(TIMER_SAVE, 10000, 1);
    if (!kv_get_int("tip", 0)) {
        ui_toast("点屏幕中间：快速跳转、目录、翻页方式");
        kv_set_int("tip", 1);
    }
    return 1;
}

static const char *turn_name(int turn)
{
    for (int i = 0; i < TURN_CHOICES; i++) {
        if (kTurns[i] == turn) return kTurnNames[i];
    }
    return kTurnNames[0];
}

static int valid_turn(int turn)
{
    for (int i = 0; i < TURN_CHOICES; i++) {
        if (kTurns[i] == turn) return 1;
    }
    return 0;
}

// 快速跳转：拖进度、上一章 / 下一章、目录、翻页方式、回书架
static void show_menu(void)
{
    char name[TITLE_LEN], title[TITLE_LEN];
    book_name(g_book, name, sizeof(name));
    push_page(name);
    int pct = g_book_size > 0 ? (int)((int64_t)g_offset * 100 / g_book_size) : 0;
    g_menu_slider = ui_slider("拖到哪就跳到哪：{}%", 0, 100, pct);
    int now = g_chapter >= -1 ? g_chapter : text_chapter_at(g_book, g_offset);
    int32_t at;
    g_menu_prev = g_menu_next = 0;
    g_prev_at = g_next_at = -1;
    if (now >= 1 && chapter_info(now - 1, &at, title, sizeof(title))) {
        g_prev_at = at;
        g_menu_prev = ui_item("上一章", title, "");
    }
    if (now >= -1 && chapter_info(now + 1, &at, title, sizeof(title))) {
        g_next_at = at;
        g_menu_next = ui_item("下一章", title, "");
    }
    g_menu_toc = ui_item("目录", now >= 0 ? g_chapter_title : "", ">");
    g_menu_turn = ui_item("翻页方式", turn_name(g_turn), ">");
    g_menu_shelf = ui_item("回书架", "", "");
}

static void show_turn(void)
{
    push_page("翻页方式");
    g_page4 = PAGE4_TURN;
    ui_text("不管哪种，点屏幕中间都是出快速跳转", NOMI_TEXT_MUTED);
    for (int i = 0; i < TURN_CHOICES; i++) {
        g_turn_rows[i] = ui_item(kTurnNames[i], "", kTurns[i] == g_turn ? "在用" : "");
    }
}

static void show_toc(int first)
{
    int total = text_chapters(g_book, first, g_buf, sizeof(g_buf));
    if (total <= 0) {
        ui_toast(total == 0 ? "这本书没找到章节" : "读不了章节");
        return;
    }
    g_toc_first = first;
    push_page("目录");
    g_page4 = PAGE4_TOC;
    g_toc_prev = first > 0 ? ui_item("上一批", "", "") : 0;
    g_toc_count = 0;
    char *p = g_buf;
    char title[TITLE_LEN];
    while (*p && g_toc_count < TOC_BATCH) {
        char *end = p;
        while (*end && *end != '\n') end++;
        char *tab = p;
        while (tab < end && *tab != '\t') tab++;
        if (tab < end) {
            *tab = 0;
            g_toc_offsets[g_toc_count] = nomi_atoi(p);
            nomi_copy_n(title, sizeof(title), tab + 1, end);
            g_toc_rows[g_toc_count] = ui_item(title, "", first + g_toc_count == g_chapter ? "在看" : "");
            g_toc_count++;
        }
        p = *end ? end + 1 : end;
    }
    g_toc_next = first + g_toc_count < total ? ui_item("下一批", "", "") : 0;
}

static void goto_chapter(int index)
{
    int32_t at;
    char title[TITLE_LEN];
    if (g_reader && chapter_info(index, &at, title, sizeof(title))) reader_goto(g_reader, at);
}

/* ---------- 语音 ---------- */

#define REPLY(text, end)        \
    do {                        \
        voice_reply(text, end); \
        return;                 \
    } while (0)

// 按名字找书：先看最近看的，再看整张卡的
static const char *find_book(const char *query)
{
    if (!query[0]) return g_recent_count ? g_recent[0] : 0;
    char name[TITLE_LEN];
    for (int i = 0; i < g_recent_count; i++) {
        book_name(g_recent[i], name, sizeof(name));
        if (contains(name, query)) return g_recent[i];
    }
    for (int i = 0; i < g_book_count; i++) {
        book_name(g_books[i].path, name, sizeof(name));
        if (contains(name, query)) return g_books[i].path;
    }
    return 0;
}

// 「第几章」：数字 n 是从第一个「第…」开头的章数起的第 n 个（前面的序章、楔子不算）；不是数字就按章名找
static int find_chapter(const char *arg)
{
    int digits = arg[0] != 0;
    for (const char *p = arg; *p; p++) digits &= *p >= '0' && *p <= '9';
    int first = 0;
    for (;;) {
        int total = text_chapters(g_book, first, g_buf, sizeof(g_buf));
        if (total <= 0) return -1;
        int index = first;
        char *p = g_buf;
        while (*p) {
            char *end = p;
            while (*end && *end != '\n') end++;
            char *tab = p;
            while (tab < end && *tab != '\t') tab++;
            char title[TITLE_LEN];
            nomi_copy_n(title, sizeof(title), tab < end ? tab + 1 : end, end);
            if (digits ? nomi_starts(title, "第") : contains(title, arg)) {
                return digits ? index + nomi_atoi(arg) - 1 : index;
            }
            index++;
            p = *end ? end + 1 : end;
        }
        if (index >= total || index == first) return -1;
        first = index;
    }
}

static void voice(int command)
{
    char arg[200], reply[300], name[TITLE_LEN];
    voice_arg(arg, sizeof(arg));
    if (command == VOICE_OPEN) {
        const char *path = find_book(arg);
        if (!path && !g_scanned && scan_books() >= 0) path = find_book(arg);
        if (!path) {
            nomi_fmt(reply, sizeof(reply), arg[0] ? "SD 卡里没找到「%s」这本书" : "还没看过小说，问问主人想看哪本", arg);
            REPLY(reply, 0);
        }
        book_name(path, name, sizeof(name));
        if (!open_book(path)) REPLY("这本书打不开（SD 卡拔了？）", 0);
        nomi_fmt(reply, sizeof(reply), "好的，打开《%s》", name);
        REPLY(reply, 1);
    }
    if (!g_reader) REPLY("现在没在看小说", 0);
    int index = -1;
    if (command == VOICE_NEXT || command == VOICE_PREV) {
        int now = text_chapter_at(g_book, g_offset);
        if (now < -1) REPLY("读不了这本书的章节", 0);
        index = now + (command == VOICE_NEXT ? 1 : -1);
        if (index < 0) REPLY("已经是第一章了", 0);
    } else if (command == VOICE_CHAPTER) {
        index = find_chapter(arg);
    }
    int32_t at;
    char title[TITLE_LEN];
    if (index < 0 || !chapter_info(index, &at, title, sizeof(title))) {
        REPLY(command == VOICE_NEXT ? "已经是最后一章了" : "这本书里没找到这一章", 0);
    }
    reader_goto(g_reader, at);
    nomi_fmt(reply, sizeof(reply), "好的，翻到%s", title);
    REPLY(reply, 1);
}

/* ---------- 入口 ---------- */

void nomi_main(void)
{
    g_turn = kv_get_int("turn", NOMI_READER_SWIPE | NOMI_READER_TAP);
    if (!valid_turn(g_turn)) g_turn = NOMI_READER_SWIPE | NOMI_READER_TAP;
    load_books();
    load_recent();
}

int32_t nomi_event(int32_t type, int32_t id, int32_t value)
{
    switch (type) {
    case NOMI_EV_SHOW:
        show_shelf();
        if (g_pending[0]) {
            char path[PATH_LEN];
            copy(path, g_pending, PATH_LEN);
            g_pending[0] = 0;
            open_book(path);
        }
        return 1;
    case NOMI_EV_HIDE:
        if (g_reader) left_reader();
        g_pages = 0;
        return 1;
    case NOMI_EV_CLOSE:
        save_offset();
        return 1;
    case NOMI_EV_TIMER:
        if (id == TIMER_SCAN) {
            scan_books();
            g_scanned = 1;  // 没插卡也算找过了，别反复找
            draw_shelf();
        } else if (id == TIMER_SAVE) {
            save_offset();
        }
        return 1;
    case NOMI_EV_BACK:
        g_pages = value;
        if (g_pages < 2 && g_reader) {  // 从书里退回书架：存书签，书架重排（刚看的到最前面）
            left_reader();
            draw_shelf();
        }
        return 1;
    case NOMI_EV_READER:
        if (id != g_reader || !g_reader) return 1;
        if (value >= 0) {
            on_page(value);
        } else if (value == NOMI_READER_MENU && g_pages == 2) {
            show_menu();
        }
        return 1;
    case NOMI_EV_CHANGE:  // 拖进度条松手：跳过去，回到书
        if (g_pages == 3 && id == g_menu_slider && g_reader && g_book_size > 0) {
            reader_goto(g_reader, value >= 100 ? g_book_size : (int32_t)((int64_t)g_book_size * value / 100));
            close_to(2);
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
    if (id == g_scan_row && g_pages == 1) {
        ui_toast("正在整张卡找书…");
        scan_books();
        g_scanned = 1;
        draw_shelf();
        return 1;
    }
    for (int i = 0; i < g_shelf_count && g_pages == 1; i++) {
        if (id == g_shelf_rows[i]) {
            open_book(g_books[g_shelf_book[i]].path);
            return 1;
        }
    }
    if (g_pages == 3) {
        if (id == g_menu_toc) {
            show_toc(g_chapter > 5 ? g_chapter - 5 : 0);
        } else if (id == g_menu_turn) {
            show_turn();
        } else if ((id == g_menu_prev && g_prev_at >= 0) || (id == g_menu_next && g_next_at >= 0)) {
            reader_goto(g_reader, id == g_menu_prev ? g_prev_at : g_next_at);
            close_to(2);
        } else if (id == g_menu_shelf) {
            left_reader();
            close_to(1);
            draw_shelf();
        }
        return 1;
    }
    if (g_pages == 4 && g_page4 == PAGE4_TURN) {
        for (int i = 0; i < TURN_CHOICES; i++) {
            if (id == g_turn_rows[i]) {
                g_turn = kTurns[i];
                kv_set_int("turn", g_turn);
                reader_set_flags(g_reader, g_turn);
                close_to(2);  // 回到书，直接试新的翻页方式
                return 1;
            }
        }
        return 1;
    }
    if (g_pages == 4) {
        if (id == g_toc_prev || id == g_toc_next) {
            int first = id == g_toc_prev ? g_toc_first - TOC_BATCH : g_toc_first + g_toc_count;
            close_to(3);
            show_toc(first < 0 ? 0 : first);
            return 1;
        }
        for (int i = 0; i < g_toc_count; i++) {
            if (id == g_toc_rows[i]) {
                reader_goto(g_reader, g_toc_offsets[i]);
                close_to(2);
                return 1;
            }
        }
    }
    return 1;
}
