// 音乐：SD 卡里的歌、电台、播客、在线点歌。
//
// 系统只管"放这一首"（解码、封面、歌词、进度条），放哪首、下一首放什么都在应用里：
// - 本地（这个文件）：队列是一个文件夹里的歌（按文件名排），列表循环 / 单曲循环 / 随机；放到哪首记在 kv 里。
//   语音点歌在系统的媒体库里找（library_list：卡上的歌系统记着）；「文件」里点了一首（NOMI_EV_OPEN）：
//   它所在的文件夹排成队列，从这首放。
// - 电台（这个文件）：台单在 kv "stations" 里（一行"台名\t地址"，应用自带的网页上增删、排序），
//   还没动过就用 assets/stations.txt 里那几个默认的；台单里没有的台名去网上的电台目录搜（radio-browser）。
// - 播客（podcast.c）：订阅、最新几期、每个播客听到哪。
// - 在线（online.c）：在主人填的音乐网站上搜歌、放、配歌词封面。
// 首页的收听卡片是这个应用画的（manifest 里 "tile": true）。台单、订阅、网站地址在应用自带的网页里管（assets/web）。
#include "music.h"

#define MAX_TRACKS 300
#define MAX_STATIONS 40
#define MAX_ROWS 200
#define MAX_PAGES 8

char g_net[NET_BUF];
int g_src = SRC_NONE;

/* ---------- 小工具 ---------- */

static void copy(char *dst, const char *src, int size) { nomi_copy_n(dst, size, src, 0); }

static int compare(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int ends_with(const char *s, const char *suffix)
{
    int n = nomi_strlen(s), m = nomi_strlen(suffix);
    if (m > n) return 0;
    for (int i = 0; i < m; i++) {
        if (lower(s[n - m + i]) != suffix[i]) return 0;
    }
    return 1;
}

// 不分大小写（只管英文字母）的包含
static int contains(const char *s, const char *word)
{
    int n = nomi_strlen(s), m = nomi_strlen(word);
    for (int i = 0; i + m <= n; i++) {
        int k = 0;
        while (k < m && lower(s[i + k]) == lower(word[k])) k++;
        if (k == m) return 1;
    }
    return 0;
}

static int is_audio(const char *name)
{
    return ends_with(name, ".mp3") || ends_with(name, ".flac") || ends_with(name, ".wav") || ends_with(name, ".aac") ||
           ends_with(name, ".m4a");
}

static int is_dir(const char *name) { return name[0] && name[nomi_strlen(name) - 1] == '/'; }

// 显示用的名字：文件去掉扩展名，文件夹去掉结尾的 /
static void display_name(char *out, int size, const char *name)
{
    copy(out, name, size);
    int len = nomi_strlen(out);
    if (len && out[len - 1] == '/') {
        out[len - 1] = '\0';
        return;
    }
    for (int k = len - 1; k > 0; k--) {
        if (out[k] == '.') {
            out[k] = '\0';
            return;
        }
    }
}

// 路径最后一段（"sd:/a/b/" → "b"）
static void last_part(char *out, int size, const char *path)
{
    int len = nomi_strlen(path);
    int end = len > 0 && path[len - 1] == '/' ? len - 1 : len;
    int start = end - 1;
    while (start >= 0 && path[start] != '/') start--;
    int n = 0;
    for (int i = start + 1; i < end && n < size - 1; i++) out[n++] = path[i];
    out[n] = '\0';
    if (n == 0) copy(out, "SD 卡", size);
}

static uint32_t g_seed;
static uint32_t rand_next(void)
{
    if (g_seed == 0) g_seed = (uint32_t)time_now() | 1u;
    g_seed = g_seed * 1103515245u + 12345u;
    return g_seed >> 8;
}

int32_t json_str(char *out, int32_t size, const char *s)
{
    int32_t n = nomi_len(out);
    if (n < size - 1) out[n++] = '"';
    for (; *s && n < size - 2; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            if (n >= size - 3) break;
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c < 0x20) {
            out[n++] = ' ';
        } else {
            out[n++] = (char)c;
        }
    }
    if (n < size - 1) out[n++] = '"';
    out[n < size ? n : size - 1] = 0;
    return n;
}

static char g_list[16384];  // fs_list 的结果

// 一个文件夹里的东西拆成一行一个，放进 names（指向 g_list 里），文件夹在前、各自按名字排。返回个数（出错是负数）
static int list_dir(const char *dir, char **names, int max, int want_dirs)
{
    int n = fs_list(dir, g_list, sizeof(g_list));
    if (n < 0) return n;
    int count = 0;
    for (char *p = g_list; *p && count < max;) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = '\0';
        if (line[0] == '.' || compare(line, "System Volume Information/") == 0 || compare(line, "nomi/") == 0) continue;
        if (line[0] == '\0' || (is_dir(line) ? !want_dirs : !is_audio(line))) continue;
        names[count++] = line;
    }
    for (int i = 1; i < count; i++) {  // 插入排序
        char *x = names[i];
        int j = i - 1;
        for (; j >= 0; j--) {
            if (is_dir(names[j]) > is_dir(x) || (is_dir(names[j]) == is_dir(x) && compare(names[j], x) <= 0)) break;
            names[j + 1] = names[j];
        }
        names[j + 1] = x;
    }
    return count;
}

/* ---------- 本地：队列 ---------- */

enum { MODE_LOOP, MODE_ONE, MODE_SHUFFLE };
static const char *const kModeNames[] = {"列表循环", "单曲循环", "随机播放"};
static int g_mode;

static char g_qdir[PATH_LEN];  // 队列是哪个文件夹（sd:/…/）
static char g_tracks[MAX_TRACKS][NAME_LEN];
static int g_track_count;
static int g_index = -1;  // 队列里第几首（-1 还没挑）

static char g_station_names[MAX_STATIONS][48];
static char g_station_urls[MAX_STATIONS][200];
static int g_station_count;
static int g_station = -1;  // 上次放的是台单里第几个台；-1 = 搜来还没收藏的
static char g_temp_name[48], g_temp_url[200];  // 搜来还没收藏的台（「收藏这个台」存的就是它）

static const char *const kSources[] = {"", "local", "radio", "podcast", "online"};

void set_source(int src)
{
    g_src = src;
    kv_set("last", kSources[src]);
}

// 文件夹里的歌排成队列
static int load_queue(const char *dir)
{
    static char *names[MAX_TRACKS];
    int n = list_dir(dir, names, MAX_TRACKS, 0);
    if (n < 0) return n;
    copy(g_qdir, dir, sizeof(g_qdir));
    g_track_count = n;
    for (int i = 0; i < n; i++) copy(g_tracks[i], names[i], NAME_LEN);
    g_index = -1;
    return n;
}

static int find_track(const char *name)
{
    for (int i = 0; i < g_track_count; i++) {
        if (compare(g_tracks[i], name) == 0) return i;
    }
    return -1;
}

void update_context(void)
{
    char text[600];
    if (g_src == SRC_RADIO) {
        nomi_fmt(text, sizeof(text), "在听电台「%s」。", g_station >= 0 ? g_station_names[g_station] : g_temp_name);
    } else if (g_src == SRC_LOCAL && g_index >= 0) {
        char folder[NAME_LEN], title[NAME_LEN];
        last_part(folder, sizeof(folder), g_qdir);
        display_name(title, sizeof(title), g_tracks[g_index]);
        nomi_fmt(text, sizeof(text), "在放 SD 卡文件夹「%s」里的「%s」（第 %d 首，共 %d 首），播放模式%s。", folder, title,
                 g_index + 1, g_track_count, kModeNames[g_mode]);
    } else if (g_src == SRC_PODCAST) {
        pod_context(text, sizeof(text));
    } else if (g_src == SRC_ONLINE) {
        online_context(text, sizeof(text));
    } else {
        nomi_fmt(text, sizeof(text), "在音乐应用里，还没挑歌。");
    }
    assistant_context(text);
}

static int play_track(int i)
{
    if (i < 0 || i >= g_track_count) return NOMI_E_NOT_FOUND;
    char path[PATH_LEN + NAME_LEN], title[NAME_LEN];
    nomi_fmt(path, sizeof(path), "%s%s", g_qdir, g_tracks[i]);
    display_name(title, sizeof(title), g_tracks[i]);
    int r = media_play(path, title, "", 0, 0, 0, 0);
    if (r < 0) return r;
    g_index = i;
    kv_set("qdir", g_qdir);
    kv_set("qfile", g_tracks[i]);
    set_source(SRC_LOCAL);
    update_context();
    return 0;
}

static int play_station(int i)
{
    if (i < 0 || i >= g_station_count) return NOMI_E_NOT_FOUND;
    int r = media_play(g_station_urls[i], g_station_names[i], "电台", NOMI_PLAY_LIVE, 0, 0, 0);
    if (r < 0) return r;
    g_station = i;
    kv_set_int("station", i);
    set_source(SRC_RADIO);
    update_context();
    return 0;
}

// 放一个还不在台单里的台（搜来的）：先放起来，主人说「收藏这个台」才进台单
static int play_temp_station(const char *name, const char *url)
{
    int r = media_play(url, name, "电台", NOMI_PLAY_LIVE, 0, 0, 0);
    if (r < 0) return r;
    nomi_copy_n(g_temp_name, sizeof(g_temp_name), name, 0);
    nomi_copy_n(g_temp_url, sizeof(g_temp_url), url, 0);
    g_station = -1;
    set_source(SRC_RADIO);
    update_context();
    return 0;
}

// 本地队列换一首：user 为 1 是主人点的上一首 / 下一首（单曲循环时也换），0 是放完了自动接
static void local_step(int delta, int user)
{
    if (g_track_count == 0 || g_index < 0) return;
    int next;
    if (!user && g_mode == MODE_ONE) {
        next = g_index;
    } else if (g_mode == MODE_SHUFFLE && g_track_count > 1) {
        next = (int)(rand_next() % (uint32_t)(g_track_count - 1));
        if (next >= g_index) next++;  // 不连着放同一首
    } else {
        next = (g_index + delta + g_track_count) % g_track_count;
    }
    // 放不了的（文件坏了、被删了）跳过，最多试一圈
    for (int tries = 0; tries < g_track_count; tries++) {
        if (play_track(next) == 0) return;
        next = (next + (delta < 0 ? -1 : 1) + g_track_count) % g_track_count;
    }
    ui_toast("这个文件夹里的歌都放不了");
}

// 接着放上次的那一路（没有就返回负数）
static int resume_last(void)
{
    switch (g_src) {
    case SRC_RADIO:
        if (g_station >= 0) return play_station(g_station);
        return g_temp_url[0] ? play_temp_station(g_temp_name, g_temp_url) : NOMI_E_NOT_FOUND;
    case SRC_PODCAST: return pod_resume();
    case SRC_ONLINE: return online_resume();
    case SRC_LOCAL: return play_track(g_index);
    default: return g_index >= 0 ? play_track(g_index) : NOMI_E_NOT_FOUND;
    }
}

/* ---------- 电台台单（kv "stations"，一行"台名\t地址"；没动过时用包里带的默认台单） ---------- */

static void save_stations(void)
{
    static char buf[8192];
    buf[0] = 0;
    for (int i = 0; i < g_station_count; i++) {
        nomi_append(buf, sizeof(buf), g_station_names[i]);
        nomi_append(buf, sizeof(buf), "\t");
        nomi_append(buf, sizeof(buf), g_station_urls[i]);
        nomi_append(buf, sizeof(buf), "\n");
    }
    kv_set("stations", buf);
}

// 台单里的台按行拆开（kv 或默认的 stations.txt，后者用 | 分隔）
static int parse_station_lines(char *text, char sep)
{
    g_station_count = 0;
    for (char *p = text; *p && g_station_count < MAX_STATIONS;) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = '\0';
        char *bar = line;
        while (*bar && *bar != sep) bar++;
        if (*bar != sep || line[0] == '#') continue;
        *bar = '\0';
        char *url = bar + 1;
        int len = nomi_strlen(url);
        while (len && (url[len - 1] == '\r' || url[len - 1] == ' ')) url[--len] = '\0';
        if (!len || !line[0]) continue;
        copy(g_station_names[g_station_count], line, 48);
        copy(g_station_urls[g_station_count], url, 200);
        g_station_count++;
    }
    return g_station_count;
}

static void load_stations(void)
{
    static char text[8192];
    if (kv_get("stations", text, sizeof(text)) > 0) {
        parse_station_lines(text, '\t');
        return;
    }
    // 还没改过台单：包里带的默认台单（只当默认值，不写进 kv；一改就整个存进去）
    int n = fs_read("pkg:/stations.txt", 0, text, sizeof(text) - 1);
    if (n < 0) n = 0;
    text[n] = '\0';
    parse_station_lines(text, '|');
}

// 同名就是改地址；满了返回 NOMI_E_TOO_BIG
static int add_station(const char *name, const char *url)
{
    for (int i = 0; i < g_station_count; i++) {
        if (nomi_equal(g_station_names[i], name)) {
            copy(g_station_urls[i], url, 200);
            save_stations();
            return 0;
        }
    }
    if (g_station_count >= MAX_STATIONS) return NOMI_E_TOO_BIG;
    copy(g_station_names[g_station_count], name, 48);
    copy(g_station_urls[g_station_count], url, 200);
    g_station_count++;
    save_stations();
    return 0;
}

static void remove_station(int i)
{
    if (i < 0 || i >= g_station_count) return;
    for (int k = i; k + 1 < g_station_count; k++) {
        copy(g_station_names[k], g_station_names[k + 1], 48);
        copy(g_station_urls[k], g_station_urls[k + 1], 200);
    }
    g_station_count--;
    if (g_station == i) g_station = -1;  // 正在放的还接着放，只是不在台单里了
    else if (g_station > i) g_station--;
    save_stations();
}

static void move_station(int from, int to)
{
    if (from < 0 || from >= g_station_count || to < 0 || to >= g_station_count || from == to) return;
    char name[48], url[200];
    copy(name, g_station_names[from], 48);
    copy(url, g_station_urls[from], 200);
    int step = from < to ? 1 : -1;
    for (int i = from; i != to; i += step) {
        copy(g_station_names[i], g_station_names[i + step], 48);
        copy(g_station_urls[i], g_station_urls[i + step], 200);
    }
    copy(g_station_names[to], name, 48);
    copy(g_station_urls[to], url, 200);
    if (g_station == from) g_station = to;
    else if (from < g_station && g_station <= to) g_station--;
    else if (to <= g_station && g_station < from) g_station++;
    save_stations();
}

/* ---------- 搜台（网上的电台目录 radio-browser） ---------- */

// 名字截断时可能切开多字节字符：把结尾不完整的那几个字节去掉
static void fix_utf8_tail(char *s)
{
    int n = nomi_strlen(s);
    if (n <= 0) return;
    unsigned char last = (unsigned char)s[n - 1];
    if (last < 0x80) return;                                    // 结尾是 ASCII：完整
    if (last >= 0xC0) {                                         // 结尾是起始字节：这个字没写完
        s[n - 1] = '\0';
        return;
    }
    int k = 1;  // 结尾连续的继续字节（0x80-0xBF）个数
    while (k < 4 && n - 1 - k >= 0) {
        unsigned char b = (unsigned char)s[n - 1 - k];
        if (b >= 0x80 && b < 0xC0) {
            k++;
            continue;
        }
        if (b >= 0xC0) {  // 它们的起始字节：数一数齐不齐
            int want = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
            if (k != want) s[n - 1 - k] = '\0';
        } else {
            s[n - k] = '\0';  // 前面是 ASCII：这些继续字节是坏的
        }
        return;
    }
    s[n - k] = '\0';
}

// 千听的 https 地址换成 http：同一个 CDN，http 不用 TLS，省内部 RAM
static void prefer_http(char *url, int size)
{
    static const char *const kHttps[2] = {"https://lhttp.qtfm.cn/", "https://lhttp.qingting.fm/"};
    for (int i = 0; i < 2; i++) {
        if (!nomi_starts(url, kHttps[i])) continue;
        char rest[192];
        copy(rest, url + nomi_len(kHttps[i]), sizeof(rest));
        nomi_fmt(url, size, "http://lhttp.qingting.fm/%s", rest);
        return;
    }
}

// 按名字去电台目录搜（票数从高到低）：取第一个能直接放的（MP3 / AAC，不要 hls 分段流）
static int search_station(const char *query, char *name, int name_size, char *url, int url_size)
{
    char enc[160], request[512];
    nomi_url_encode(query, enc, sizeof(enc));
    nomi_fmt(request, sizeof(request),
             "http://all.api.radio-browser.info/json/stations/search?hidebroken=true&order=votes"
             "&reverse=true&limit=8&name=%s",
             enc);
    int32_t n = net_get(request, g_net, 128 * 1024);
    if (n < 0) return n;
    g_net[n] = 0;
    const char *end = g_net + n;
    for (const char *p = nomi_find(g_net, end, "{"); p;) {
        const char *next = nomi_find(p + 1, end, "},{");
        const char *e = next ? next + 1 : end;  // 对象到自己的 '}' 为止
        char codec[16], u[240], hls[8];
        if (nomi_json_field(p, e, "codec", codec, sizeof(codec)) <= 0 ||
            nomi_json_field(p, e, "url_resolved", u, sizeof(u)) <= 0 || !u[0]) {
            // 不认识的条目，跳过
        } else if ((nomi_json_field(p, e, "hls", hls, sizeof(hls)) > 0 && nomi_equal(hls, "1")) ||
                   nomi_find(u, u + nomi_len(u), ".m3u8")) {
            // 分段流放不了，跳过
        } else if (nomi_equal(codec, "MP3") || nomi_equal(codec, "AAC") || nomi_equal(codec, "AAC+")) {
            nomi_json_field(p, e, "name", name, name_size);
            copy(url, u, url_size);
            fix_utf8_tail(name);
            prefer_http(url, url_size);
            if (name[0] && url[0]) return 0;
        }
        p = next ? nomi_find(next + 2, end, "{") : 0;
    }
    return NOMI_E_NOT_FOUND;
}

/* ---------- 界面 ---------- */

enum { ROW_SD = 1, ROW_RADIO, ROW_QDIR, ROW_DIR, ROW_SONG, ROW_STATION, ROW_PODCASTS, ROW_ONLINE };

// 可点的控件 → 干什么。page 是它在第几页（1 是首页），退页时把上面那页的扔掉
static struct {
    int32_t id;
    int8_t page, kind;
    int16_t index;
} g_rows[MAX_ROWS];
static char g_row_names[MAX_ROWS][NAME_LEN];
static int g_row_count;
int g_pages;                                  // 现在压了几页（0 = 界面没挂着）
static char g_dirs[MAX_PAGES + 1][PATH_LEN];  // 浏览页对应的文件夹
static int32_t g_mode_button;
static int32_t g_tile_buttons[4];  // 首页卡片顶上的入口：本地、电台、播客、在线
static int g_open_page;            // 从卡片点进来要直接进哪页（ROW_SD 之类）

int32_t add_row(int32_t id, int kind, int index, const char *name)
{
    if (id < 0 || g_row_count >= MAX_ROWS) return id;
    g_rows[g_row_count].id = id;
    g_rows[g_row_count].page = (int8_t)g_pages;
    g_rows[g_row_count].kind = (int8_t)kind;
    g_rows[g_row_count].index = (int16_t)index;
    copy(g_row_names[g_row_count], name ? name : "", NAME_LEN);
    g_row_count++;
    return id;
}

void push_page(const char *title)
{
    ui_page(title);
    g_pages++;
}

static void show_home(void)
{
    g_row_count = 0;
    g_pages = 0;
    push_page("音乐");
    ui_player();
    g_mode_button = ui_button(kModeNames[g_mode], 0);
    add_row(ui_item("SD 卡", "按文件夹浏览", ">"), ROW_SD, 0, "");
    char sub[64];
    nomi_fmt(sub, sizeof(sub), "%d 个台", g_station_count);
    add_row(ui_item("电台", sub, ">"), ROW_RADIO, 0, "");
    nomi_fmt(sub, sizeof(sub), "%d 个订阅", pod_count());
    add_row(ui_item("播客", sub, ">"), ROW_PODCASTS, 0, "");
    add_row(ui_item("在线音乐", online_ready() ? "上次搜到的歌" : "还没填音乐网站", ">"), ROW_ONLINE, 0, "");
    if (g_qdir[0]) {
        char name[NAME_LEN];
        last_part(name, sizeof(name), g_qdir);
        nomi_fmt(sub, sizeof(sub), "%d 首", g_track_count);
        add_row(ui_item(name, sub, ">"), ROW_QDIR, 0, "");
    }
    update_context();
}

static void show_dir(const char *dir, const char *title)
{
    if (g_pages >= MAX_PAGES) {
        ui_toast("文件夹太深了");
        return;
    }
    push_page(title);
    copy(g_dirs[g_pages], dir, PATH_LEN);
    static char *names[MAX_ROWS];
    int n = list_dir(dir, names, MAX_ROWS - g_row_count, 1);
    if (n == NOMI_E_NO_SD) {
        ui_text("没插 SD 卡。", NOMI_TEXT_MUTED);
        return;
    }
    if (n < 0) {
        ui_text("打不开这个文件夹。", NOMI_TEXT_MUTED);
        return;
    }
    if (n == 0) ui_text("这里没有歌（认 MP3、FLAC、WAV、AAC、M4A）。", NOMI_TEXT_MUTED);
    // 先拷名字：g_list 下次 fs_list 就被覆盖了
    static char kept[MAX_ROWS][NAME_LEN];
    for (int i = 0; i < n; i++) copy(kept[i], names[i], NAME_LEN);
    for (int i = 0; i < n; i++) {
        char title_buf[NAME_LEN];
        display_name(title_buf, sizeof(title_buf), kept[i]);
        int folder = is_dir(kept[i]);
        int playing = !folder && g_src == SRC_LOCAL && g_index >= 0 && compare(dir, g_qdir) == 0 &&
                      compare(kept[i], g_tracks[g_index]) == 0;
        add_row(ui_item(title_buf, folder ? "文件夹" : (playing ? "正在放" : ""), folder ? ">" : ""),
                folder ? ROW_DIR : ROW_SONG, 0, kept[i]);
    }
}

static void show_radio(void)
{
    push_page("电台");
    if (g_station_count == 0) {
        ui_text("台单是空的：在网页设置页的「设备 → 应用 → 音乐 → 设置」里添加，或跟它说一个台名去搜。", NOMI_TEXT_MUTED);
    }
    for (int i = 0; i < g_station_count; i++) {
        add_row(ui_item(g_station_names[i], g_src == SRC_RADIO && i == g_station ? "正在放" : "", ""), ROW_STATION, i,
                "");
    }
}

// 回到首页（挑了一首之后看「正在放」）
void back_home(void)
{
    while (g_pages > 1) {
        ui_page_close();
        g_pages--;
        while (g_row_count && g_rows[g_row_count - 1].page > g_pages) g_row_count--;
    }
}

static void open_page(int kind)
{
    if (kind == ROW_SD) show_dir("sd:/", "SD 卡");
    else if (kind == ROW_RADIO) show_radio();
    else if (kind == ROW_PODCASTS) pod_show_subs();
    else if (kind == ROW_ONLINE) online_show();
}

// 首页卡片：系统的「正在放」（带歌词），顶上原来标签的位置放几个入口
static void draw_tile(void)
{
    if (ui_now_playing("跟它说「放周杰伦」「放忽左忽右」，或者点上面的入口挑") < 0) return;
    g_tile_buttons[0] = ui_button("本地", 0);
    g_tile_buttons[1] = ui_button("电台", 0);
    g_tile_buttons[2] = ui_button("播客", 0);
    g_tile_buttons[3] = ui_button("在线", 0);
}

/* ---------- 大模型的指令（顺序和 manifest 的 voice 一致） ---------- */

enum { VOICE_PLAY = 1, VOICE_SKIP, VOICE_MODE, VOICE_PODCAST, VOICE_SUBSCRIBE, VOICE_ONLINE, VOICE_RADIO };

// 在卡上的歌里按名字找（系统的媒体库，整张卡）：文件夹名、文件名里带这个词的，浅的优先，同一层文件夹先于歌
// （说歌手名放整个文件夹）。找到文件夹时 found_file 为空。g_net 借来装列表（这时不联网）
static int search(const char *word, char *found_dir, char *found_file)
{
    int best = 1 << 30;  // 层数 * 2 + 是不是歌
    int32_t first = 0, total = 1;
    while (first < total && best > 0) {
        total = library_list(NOMI_LIB_AUDIO, first, g_net, NET_BUF);
        if (total <= 0) break;
        int got = 0;
        for (char *p = g_net; *p;) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p) *p++ = '\0';
            got++;
            char *tab = line;
            while (*tab && *tab != '\t') tab++;
            *tab = '\0';
            // 「sd:/文件夹/…/歌.mp3」一段段看
            char *c = line + 4;
            for (int depth = 0;; depth++) {
                char *slash = c;
                while (*slash && *slash != '/') slash++;
                int is_file = !*slash;
                int score = depth * 2 + is_file;
                *slash = '\0';
                int hit = score < best && contains(c, word);
                if (!is_file) *slash = '/';
                if (hit) {
                    best = score;
                    nomi_copy_n(found_dir, PATH_LEN, line, is_file ? c : slash + 1);  // 文件夹带结尾的 /
                    if (is_file) copy(found_file, c, NAME_LEN);
                    else found_file[0] = '\0';
                }
                if (is_file) break;
                c = slash + 1;
            }
        }
        if (got == 0) break;
        first += got;
    }
    return best < (1 << 30);
}

// 回话并结束这次指令
#define REPLY(text, end)        \
    do {                        \
        voice_reply(text, end); \
        return;                 \
    } while (0)

static void voice(int command)
{
    char arg[200], reply[300];
    voice_arg(arg, sizeof(arg));
    if (command == VOICE_PLAY) {
        if (!arg[0]) {
            if (media_mine() && media_state() == 3) media_control(NOMI_MEDIA_RESUME);
            else if (resume_last() < 0) REPLY("还没放过什么，问问主人想听哪首", 0);
            REPLY("好的，接着放", 1);
        }
        char dir[PATH_LEN], file[NAME_LEN];
        if (!search(arg, dir, file)) {
            nomi_fmt(reply, sizeof(reply), "SD 卡里没找到「%s」；在网上找用 command=online", arg);
            REPLY(reply, 0);
        }
        if (load_queue(dir) <= 0) REPLY("找到了文件夹，但里面没有能放的歌", 0);
        int i = file[0] ? find_track(file) : 0;
        if (g_mode == MODE_SHUFFLE && !file[0]) i = (int)(rand_next() % (uint32_t)g_track_count);
        if (play_track(i < 0 ? 0 : i) < 0) REPLY("找到了，但放不了这个文件", 0);
        char title[NAME_LEN];
        display_name(title, sizeof(title), g_tracks[g_index]);
        nomi_fmt(reply, sizeof(reply), "好的，放「%s」", title);
        REPLY(reply, 1);
    }
    if (command == VOICE_SKIP) {
        if (!media_mine()) REPLY("现在放的不是音乐应用里的", 0);
        nomi_event(NOMI_EV_MEDIA, 0, nomi_equal(arg, "prev") ? NOMI_MEDIA_PREV : NOMI_MEDIA_NEXT);
        REPLY("好的", 1);
    }
    if (command == VOICE_MODE) {
        int mode = contains(arg, "单曲") ? MODE_ONE : contains(arg, "随机") ? MODE_SHUFFLE : MODE_LOOP;
        g_mode = mode;
        kv_set_int("mode", mode);
        if (g_pages) ui_set_text(g_mode_button, kModeNames[mode]);
        update_context();
        nomi_fmt(reply, sizeof(reply), "已经换成%s", kModeNames[mode]);
        REPLY(reply, 0);
    }
    if (command == VOICE_PODCAST) {
        int r = pod_play_latest(arg);
        if (r == NOMI_E_NOT_FOUND) {
            nomi_fmt(reply, sizeof(reply), arg[0] ? "订阅里没有、播客目录也没搜到「%s」" : "还没听过播客，问问主人想听哪个", arg);
            REPLY(reply, 0);
        }
        if (r < 0) REPLY("节目单拉不下来（网络不好？），过会儿再试", 0);
        pod_context(reply, sizeof(reply));
        REPLY(reply, 1);
    }
    if (command == VOICE_SUBSCRIBE) {
        char name[128];
        int r = pod_subscribe(arg, name, sizeof(name));
        if (r < 0) {
            nomi_fmt(reply, sizeof(reply), "播客目录里没搜到「%s」", arg);
            REPLY(reply, 0);
        }
        nomi_fmt(reply, sizeof(reply), r == 0 ? "已订阅《%s》" : "《%s》已经订阅过了", name);
        REPLY(reply, 0);
    }
    if (command == VOICE_ONLINE) {
        char title[200];
        int r = online_search_play(arg, title, sizeof(title));
        if (r == NOMI_E_DENIED) REPLY("还没设置音乐网站：请主人在网页设置页的「应用 → 音乐 → 设置」里填", 0);
        if (r < 0) {
            nomi_fmt(reply, sizeof(reply), "音乐网站上没找到能放的「%s」", arg);
            REPLY(reply, 0);
        }
        nomi_fmt(reply, sizeof(reply), "好的，放《%s》", title);
        REPLY(reply, 1);
    }
    if (command == VOICE_RADIO) {
        if (!arg[0]) {  // 接着放（本来在放、对话时暂停着的：说完自动接着放）
            if (media_mine() && media_state() == 3) media_control(NOMI_MEDIA_RESUME);
            else if (g_src == SRC_RADIO && resume_last() == 0) REPLY("好的，接着放", 1);
            else REPLY("还没放过电台，问问主人想听哪个台，或说个类型我去搜", 0);
        } else if (nomi_equal(arg, "下一个") || nomi_equal(arg, "换台") || nomi_equal(arg, "next") ||
                   nomi_equal(arg, "上一个") || nomi_equal(arg, "prev")) {
            if (!g_station_count) REPLY("台单是空的，先在网页设置页里加几个台，或者说一个台名我去搜", 0);
            int back = nomi_equal(arg, "上一个") || nomi_equal(arg, "prev");
            int i = ((g_station < 0 ? 0 : g_station) + (back ? -1 : 1) + g_station_count) % g_station_count;
            if (play_station(i) < 0) REPLY("这个台连不上，换个台试试", 0);
            nomi_fmt(reply, sizeof(reply), "换到「%s」", g_station_names[g_station]);
            REPLY(reply, 1);
        } else if (nomi_equal(arg, "收藏") || nomi_equal(arg, "收藏这个台")) {
            if (g_station >= 0) REPLY("正在放的这个台已经在台单里了", 0);
            if (!g_temp_url[0]) REPLY("现在没在放搜来的台（台单里的台本来就在）", 0);
            add_station(g_temp_name, g_temp_url);  // 同名就是改地址，不一定加在最后：按下标找回来
            for (int i = 0; i < g_station_count; i++) {
                if (nomi_equal(g_station_names[i], g_temp_name)) {
                    g_station = i;
                    kv_set_int("station", i);
                    break;
                }
            }
            nomi_fmt(reply, sizeof(reply), "已收藏「%s」", g_temp_name);
            REPLY(reply, 0);
        } else {
            int i;
            for (i = 0; i < g_station_count; i++) {
                if (contains(g_station_names[i], arg) || contains(arg, g_station_names[i])) break;
            }
            if (i < g_station_count) {
                if (play_station(i) < 0) REPLY("这个台连不上，换个台试试", 0);
                nomi_fmt(reply, sizeof(reply), "好的，放「%s」", g_station_names[g_station]);
                REPLY(reply, 1);
            }
            char name[48], url[200];  // 台单里没有：去网上的电台目录搜
            if (search_station(arg, name, sizeof(name), url, sizeof(url)) < 0) {
                nomi_fmt(reply, sizeof(reply), "收藏里没有，网上的电台目录也没搜到「%s」；可以换个说法", arg);
                REPLY(reply, 0);
            }
            if (play_temp_station(name, url) < 0) REPLY("搜到了但连不上这个台", 0);
            nomi_fmt(reply, sizeof(reply), "搜到「%s」，马上放；想以后还听可以说\"收藏这个台\"", name);
            REPLY(reply, 1);
        }
    }
}

/* ---------- 网页（assets/web） ---------- */

// 台单：列出（顺序就是设备上换台的顺序）、加、删、挪（排序）、放
static void station_web(const char *body, char *out, int size)
{
    const char *end = body + nomi_len(body);
    char cmd[32];
    nomi_json_field(body, end, "cmd", cmd, sizeof(cmd));
    if (nomi_equal(cmd, "station_list")) {
        nomi_append(out, size, "{\"ok\":true,\"max\":");
        nomi_fmt(out + nomi_len(out), size - nomi_len(out), "%d", MAX_STATIONS);
        nomi_append(out, size, ",\"playing\":");
        nomi_fmt(out + nomi_len(out), size - nomi_len(out), "%d", g_src == SRC_RADIO ? g_station : -1);
        nomi_append(out, size, ",\"stations\":[");
        for (int i = 0; i < g_station_count; i++) {
            if (i) nomi_append(out, size, ",");
            nomi_append(out, size, "{\"name\":");
            json_str(out, size, g_station_names[i]);
            nomi_append(out, size, ",\"url\":");
            json_str(out, size, g_station_urls[i]);
            nomi_append(out, size, "}");
        }
        nomi_append(out, size, "]}");
    } else if (nomi_equal(cmd, "station_add")) {
        char name[64], url[220];
        nomi_json_field(body, end, "name", name, sizeof(name));
        nomi_json_field(body, end, "url", url, sizeof(url));
        if (!name[0] || (!nomi_starts(url, "http://") && !nomi_starts(url, "https://"))) {
            nomi_fmt(out, size, "{\"ok\":false,\"error\":\"台名要填，地址要以 http:// 或 https:// 开头\"}");
        } else if (nomi_find(url, url + nomi_len(url), ".m3u8")) {
            nomi_fmt(out, size, "{\"ok\":false,\"error\":\"不支持 m3u8 分段流，要 MP3 / AAC 直播地址\"}");
        } else if (add_station(name, url) == NOMI_E_TOO_BIG) {
            nomi_fmt(out, size, "{\"ok\":false,\"error\":\"最多存 %d 个台，先删掉几个\"}", MAX_STATIONS);
        } else {
            nomi_fmt(out, size, "{\"ok\":true}");
        }
    } else if (nomi_equal(cmd, "station_delete")) {
        char idx[16];
        nomi_json_field(body, end, "index", idx, sizeof(idx));
        int i = nomi_atoi(idx);
        if (i < 0 || i >= g_station_count) nomi_fmt(out, size, "{\"ok\":false,\"error\":\"没有这个台\"}");
        else {
            remove_station(i);
            nomi_fmt(out, size, "{\"ok\":true}");
        }
    } else if (nomi_equal(cmd, "station_move")) {  // {"index":n,"to":m}：把第 n 个挪到第 m 个的位置（排序）
        char a[16], b[16];
        nomi_json_field(body, end, "index", a, sizeof(a));
        nomi_json_field(body, end, "to", b, sizeof(b));
        move_station(nomi_atoi(a), nomi_atoi(b));
        nomi_fmt(out, size, "{\"ok\":true}");
    } else if (nomi_equal(cmd, "station_play")) {  // 在设备上放台单里的这个台
        char idx[16];
        nomi_json_field(body, end, "index", idx, sizeof(idx));
        if (play_station(nomi_atoi(idx)) < 0) nomi_fmt(out, size, "{\"ok\":false,\"error\":\"放不了这个台\"}");
        else nomi_fmt(out, size, "{\"ok\":true}");
    } else {
        nomi_fmt(out, size, "{\"ok\":false,\"error\":\"不认识的请求\"}");
    }
}

static void web(void)
{
    static char body[2048], out[24 * 1024];
    web_body(body, sizeof(body));
    out[0] = 0;
    char cmd[32];
    nomi_json_field(body, body + nomi_len(body), "cmd", cmd, sizeof(cmd));
    if (nomi_starts(cmd, "pod_")) pod_web(body, out, sizeof(out));
    else if (nomi_starts(cmd, "online_")) online_web(body, out, sizeof(out));
    else if (nomi_starts(cmd, "station_")) station_web(body, out, sizeof(out));
    else nomi_fmt(out, sizeof(out), "{\"ok\":false,\"error\":\"不认识的请求\"}");
    web_reply(out);
}

/* ---------- 入口 ---------- */

void nomi_main(void)
{
    g_mode = kv_get_int("mode", MODE_LOOP);
    if (g_mode < 0 || g_mode > 2) g_mode = MODE_LOOP;
    load_stations();
    pod_load();
    online_load();
    int station = kv_get_int("station", -1);
    if (station >= 0 && station < g_station_count) g_station = station;
    char dir[PATH_LEN], file[NAME_LEN], last[16];
    if (kv_get("qdir", dir, sizeof(dir)) > 0 && load_queue(dir) > 0 && kv_get("qfile", file, sizeof(file)) > 0) {
        g_index = find_track(file);
    }
    if (kv_get("last", last, sizeof(last)) > 0) {
        for (int i = 1; i < 5; i++) {
            if (nomi_equal(last, kSources[i])) g_src = i;
        }
    }
    draw_tile();
    timer_start(1, 15000, 1);  // 播客：每 15 秒记一次听到哪
}

int32_t nomi_event(int32_t type, int32_t id, int32_t value)
{
    switch (type) {
    case NOMI_EV_SHOW:
        show_home();
        if (g_open_page) open_page(g_open_page);
        g_open_page = 0;
        return 1;
    case NOMI_EV_HIDE:
        g_pages = 0;
        g_row_count = 0;
        pod_save_progress(0);
        return 1;
    case NOMI_EV_CLOSE:
        pod_save_progress(0);
        return 1;
    case NOMI_EV_TIMER:
        if (id == 1) pod_save_progress(0);
        return 1;
    case NOMI_EV_BACK:
        g_pages = value;
        while (g_row_count && g_rows[g_row_count - 1].page > g_pages) g_row_count--;
        return 1;
    case NOMI_EV_MEDIA:
        if (value == NOMI_MEDIA_PLAY) {
            if (resume_last() < 0) ui_open();  // 没放过：打开让主人挑
        } else if (g_src == SRC_PODCAST) {
            pod_media(value);
        } else if (g_src == SRC_ONLINE) {
            online_media(value);
        } else if (g_src == SRC_RADIO) {
            if (value != NOMI_MEDIA_FINISHED && g_station_count) {  // 电台是直播，不会"放完"
                int i = (g_station < 0 ? 0 : g_station) + (value == NOMI_MEDIA_PREV ? -1 : 1);
                play_station((i + g_station_count) % g_station_count);
            }
        } else {
            local_step(value == NOMI_MEDIA_PREV ? -1 : 1, value != NOMI_MEDIA_FINISHED);
        }
        return 1;
    case NOMI_EV_VOICE:
        voice(id);
        return 1;
    case NOMI_EV_OPEN: {  // 「文件」里点了一首：它所在的文件夹排成队列，从这首放
        char path[PATH_LEN], dir[PATH_LEN];
        if (open_path(path, sizeof(path)) <= 0) return 1;
        char *name = path + nomi_strlen(path);
        while (name > path && name[-1] != '/') name--;
        nomi_copy_n(dir, PATH_LEN, path, name);
        if (load_queue(dir) <= 0 || play_track(find_track(name)) < 0) ui_toast("这首放不了");
        return 1;
    }
    case NOMI_EV_WEB:
        web();
        return 1;
    case NOMI_EV_CLICK:
        break;
    default:
        return 0;
    }
    static const int kTilePages[4] = {ROW_SD, ROW_RADIO, ROW_PODCASTS, ROW_ONLINE};
    for (int i = 0; i < 4; i++) {
        if (id == g_tile_buttons[i]) {
            g_open_page = kTilePages[i];
            ui_open();
            return 1;
        }
    }
    if (g_pages && id == g_mode_button) {
        g_mode = (g_mode + 1) % 3;
        kv_set_int("mode", g_mode);
        ui_set_text(g_mode_button, kModeNames[g_mode]);
        update_context();
        return 1;
    }
    for (int i = 0; i < g_row_count; i++) {
        if (g_rows[i].id != id) continue;
        char *name = g_row_names[i];
        char path[PATH_LEN], title[NAME_LEN];
        int kind = g_rows[i].kind;
        switch (kind) {
        case ROW_SD:
        case ROW_RADIO:
        case ROW_PODCASTS:
        case ROW_ONLINE:
            open_page(kind);
            break;
        case ROW_QDIR:
            last_part(title, sizeof(title), g_qdir);
            copy(path, g_qdir, sizeof(path));
            show_dir(path, title);
            break;
        case ROW_DIR:
            nomi_fmt(path, sizeof(path), "%s%s", g_dirs[g_rows[i].page], name);
            display_name(title, sizeof(title), name);
            show_dir(path, title);
            break;
        case ROW_SONG: {
            const char *dir = g_dirs[g_rows[i].page];
            if (compare(dir, g_qdir) != 0 || g_track_count == 0) load_queue(dir);
            if (play_track(find_track(name)) < 0) ui_toast("放不了这首");
            else back_home();
            break;
        }
        case ROW_STATION:
            if (play_station(g_rows[i].index) < 0) ui_toast("放不了这个台");
            else back_home();
            break;
        default:
            if (!pod_click(kind, g_rows[i].index)) online_click(kind, g_rows[i].index);
            break;
        }
        return 1;
    }
    return 0;
}
