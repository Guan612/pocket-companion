// 在线点歌：在主人填的音乐网站上按歌名搜（kv "site"），从歌曲页拿音频直链、封面和带时间的歌词，交给系统放。
//
// 只认一种站点格式（docs/online-music.md）：搜索页 {网站}/so/{关键词}/ 每条结果带 data-rid；歌曲页
// {网站}/song/{rid}/ 里有 base64 编码的音频地址（let code = "…"）、id="album-cover" 的封面和 data-time 的歌词。
// 网站失效了换一个同款的填上就行。上次搜到的记在 kv "songs"（一行"rid\t歌名\t歌手"），放完这首就停。
#include "music.h"

#define MAX_SONGS 10

typedef struct {
    char id[64], title[128], artist[96];
} song_t;

static char g_site[160];  // "http://example.com"，不带结尾的 /；空 = 还没设
static song_t g_songs[MAX_SONGS];
static int g_song_count;
static int g_song = -1;  // 放的是第几首
static char g_lrc[16 * 1024];

// [s, end) 里 begin 和 close 之间的文字（解开实体、去首尾空白）；找不到返回 0，找到返回 close 的位置
static const char *between(const char *s, const char *end, const char *begin, const char *close, char *out, int size)
{
    out[0] = 0;
    const char *b = nomi_find(s, end, begin);
    if (!b) return 0;
    b += nomi_len(begin);
    const char *e = nomi_find(b, end, close);
    if (!e) return 0;
    nomi_xml_text(b, e, out, size);
    return e;
}

static void save_songs(void)
{
    static char buf[4096];
    buf[0] = 0;
    for (int i = 0; i < g_song_count; i++) {
        nomi_append(buf, sizeof(buf), g_songs[i].id);
        nomi_append(buf, sizeof(buf), "\t");
        nomi_append(buf, sizeof(buf), g_songs[i].title);
        nomi_append(buf, sizeof(buf), "\t");
        nomi_append(buf, sizeof(buf), g_songs[i].artist);
        nomi_append(buf, sizeof(buf), "\n");
    }
    kv_set("songs", buf);
}

void online_load(void)
{
    if (kv_get("site", g_site, sizeof(g_site)) < 0) g_site[0] = 0;
    static char buf[4096];
    g_song_count = 0;
    if (kv_get("songs", buf, sizeof(buf)) > 0) {
        for (char *p = buf; *p && g_song_count < MAX_SONGS;) {
            char *line = p, *f[3] = {line, "", ""};
            int k = 1;
            while (*p && *p != '\n') {
                if (*p == '\t' && k < 3) {
                    *p = 0;
                    f[k++] = p + 1;
                }
                p++;
            }
            if (*p) *p++ = 0;
            if (!f[0][0]) continue;
            song_t *s = &g_songs[g_song_count++];
            nomi_copy_n(s->id, sizeof(s->id), f[0], 0);
            nomi_copy_n(s->title, sizeof(s->title), f[1], 0);
            nomi_copy_n(s->artist, sizeof(s->artist), f[2], 0);
        }
    }
    g_song = kv_get_int("song", -1);
    if (g_song >= g_song_count) g_song = -1;
}

int online_ready(void) { return g_site[0] != 0; }

static int search(const char *query)
{
    if (!g_site[0]) return NOMI_E_DENIED;
    char enc[256], url[480];
    nomi_url_encode(query, enc, sizeof(enc));
    nomi_fmt(url, sizeof(url), "%s/so/%s/", g_site, enc);
    int32_t n = net_get(url, g_net, NET_BUF - 1);
    if (n < 0) return n;
    g_net[n] = 0;
    const char *end = g_net + n;
    // 每条结果：<div class="result-item" data-rid="…"> 里有 result-title、result-artist
    int count = 0;
    for (const char *p = nomi_find(g_net, end, "data-rid=\""); p && count < MAX_SONGS;) {
        const char *next = nomi_find(p + 10, end, "data-rid=\"");
        const char *e = next ? next : end;
        song_t *s = &g_songs[count];
        between(p, e, "data-rid=\"", "\"", s->id, sizeof(s->id));
        between(p, e, "result-title\">", "<", s->title, sizeof(s->title));
        between(p, e, "result-artist\">", "<", s->artist, sizeof(s->artist));
        if (s->id[0] && s->title[0]) count++;
        p = next;
    }
    // 一条都没有：是这种网站的搜索页就是真没搜到，不是（换了格式、跳到了别处）就当网站不能用
    if (count == 0 && !nomi_find(g_net, end, "search-results")) return NOMI_E_FAIL;
    g_song_count = count;
    g_song = -1;
    save_songs();
    return count;
}

static void put2(char *out, int size, int v)
{
    char d[3] = {(char)('0' + v / 10 % 10), (char)('0' + v % 10), 0};
    nomi_append(out, size, d);
}

// 取歌曲页、放起来、配上歌词封面。放不了返回负数
static int play(int i)
{
    if (i < 0 || i >= g_song_count || !g_site[0]) return NOMI_E_NOT_FOUND;
    song_t *song = &g_songs[i];
    char enc[128], url[480];
    nomi_url_encode(song->id, enc, sizeof(enc));
    nomi_fmt(url, sizeof(url), "%s/song/%s/", g_site, enc);
    int32_t n = net_get(url, g_net, NET_BUF - 1);
    if (n < 0) return n;
    g_net[n] = 0;
    const char *end = g_net + n;

    // 音频地址：页面脚本里的 let code = "…"，base64
    static char code[2048], audio[1536];
    if (!between(g_net, end, "let code = \"", "\"", code, sizeof(code)) || !code[0]) return NOMI_E_NOT_FOUND;
    if (nomi_base64_decode(code, nomi_len(code), audio, sizeof(audio)) <= 0) return NOMI_E_NOT_FOUND;
    if (!nomi_starts(audio, "http://") && !nomi_starts(audio, "https://")) return NOMI_E_NOT_FOUND;

    // 歌词：<div class="lyric-line" data-time="12">…</div>，时间是秒；换成 LRC 交给系统
    g_lrc[0] = 0;
    const char *lc = nomi_find(g_net, end, "lyrics-container");
    for (const char *p = lc; p && (p = nomi_find(p, end, "data-time=\"")) != 0;) {
        p += 11;
        int32_t t = nomi_atoi(p);
        const char *gt = nomi_find(p, end, ">");
        const char *close = gt ? nomi_find(gt, end, "</div>") : 0;
        if (!close) break;
        char text[256];
        nomi_xml_text(gt + 1, close, text, sizeof(text));
        if (text[0] && nomi_len(g_lrc) + nomi_len(text) + 16 < (int)sizeof(g_lrc)) {
            nomi_append(g_lrc, sizeof(g_lrc), "[");
            put2(g_lrc, sizeof(g_lrc), t / 60);
            nomi_append(g_lrc, sizeof(g_lrc), ":");
            put2(g_lrc, sizeof(g_lrc), t % 60);
            nomi_append(g_lrc, sizeof(g_lrc), ".00]");
            nomi_append(g_lrc, sizeof(g_lrc), text);
            nomi_append(g_lrc, sizeof(g_lrc), "\n");
        }
        p = close;
    }

    // 封面：<img id="album-cover" … src="…">；地址里带尺寸（…/albumcover/300/…）时换成 120 的小图
    char cover[400];
    cover[0] = 0;
    const char *img = nomi_find(g_net, end, "id=\"album-cover\"");
    if (img) {
        const char *tag = img;
        while (tag > g_net && *tag != '<') tag--;
        const char *gt = nomi_find(img, end, ">");
        if (gt) between(tag, gt, "src=\"", "\"", cover, sizeof(cover));
        char *size = (char *)nomi_find(cover, cover + nomi_len(cover), "/albumcover/300/");
        if (size) size[12] = '1', size[13] = '2', size[14] = '0';  // "/albumcover/" 12 个字符，后面是 300
    }

    int r = media_play(audio, song->title, song->artist, 0, 0, 0, 0);  // 系统探格式；不是音频返回 NOT_FOUND
    if (r < 0) return r;
    media_meta(cover, g_lrc);
    g_song = i;
    kv_set_int("song", i);
    set_source(SRC_ONLINE);
    update_context();
    return 0;
}

// 放搜到的第 i 首，放不了（下架、地址失效）就往下试，最多试 3 首
static int play_from(int i, char *title_out, int size)
{
    int r = NOMI_E_NOT_FOUND;
    for (int k = 0; k < 3 && i + k < g_song_count; k++) {
        if ((r = play(i + k)) == 0) {
            if (title_out) nomi_copy_n(title_out, size, g_songs[i + k].title, 0);
            return 0;
        }
    }
    return r;
}

int online_search_play(const char *query, char *title_out, int size)
{
    int n = search(query);
    if (n < 0) return n;
    if (n == 0) return NOMI_E_NOT_FOUND;
    return play_from(0, title_out, size);
}

int online_resume(void) { return g_song >= 0 ? play(g_song) : NOMI_E_NOT_FOUND; }

void online_media(int value)
{
    // 搜到的多是同一首的不同版本：放完就停；主人点上一首 / 下一首才在搜到的里面换
    if (value == NOMI_MEDIA_NEXT && g_song + 1 < g_song_count) play_from(g_song + 1, 0, 0);
    else if (value == NOMI_MEDIA_PREV && g_song > 0) play(g_song - 1);
}

void online_context(char *out, int size)
{
    if (g_song < 0) {
        nomi_fmt(out, size, "在音乐应用里。");
        return;
    }
    nomi_fmt(out, size, "在放网上点的歌《%s》%s%s。", g_songs[g_song].title, g_songs[g_song].artist[0] ? " - " : "",
             g_songs[g_song].artist);
}

void online_show(void)
{
    push_page("在线音乐");
    if (!g_site[0]) {
        ui_text("还没填音乐网站：在网页设置页的「设备 → 应用 → 音乐 → 设置」里填。", NOMI_TEXT_MUTED);
        return;
    }
    ui_text("跟它说「放一首晴天」「来首周杰伦的歌」。", NOMI_TEXT_MUTED);
    for (int i = 0; i < g_song_count; i++) {
        add_row(ui_item(g_songs[i].title, g_songs[i].artist, i == g_song && g_src == SRC_ONLINE ? "在放" : ""),
                ROW_ONLINE_SONG, i, "");
    }
}

int online_click(int kind, int index)
{
    if (kind != ROW_ONLINE_SONG) return 0;
    if (play_from(index, 0, 0) < 0) ui_toast("这首放不了");
    else back_home();
    return 1;
}

/* ---------- 网页 ---------- */

static void songs_json(char *out, int size)
{
    nomi_append(out, size, "{\"ok\":true,\"site\":");
    json_str(out, size, g_site);
    nomi_append(out, size, ",\"songs\":[");
    for (int i = 0; i < g_song_count; i++) {
        if (i) nomi_append(out, size, ",");
        nomi_append(out, size, "{\"title\":");
        json_str(out, size, g_songs[i].title);
        nomi_append(out, size, ",\"artist\":");
        json_str(out, size, g_songs[i].artist);
        nomi_append(out, size, "}");
    }
    nomi_append(out, size, "]}");
}

void online_web(const char *body, char *out, int size)
{
    const char *end = body + nomi_len(body);
    char cmd[32];
    nomi_json_field(body, end, "cmd", cmd, sizeof(cmd));
    if (nomi_equal(cmd, "online_get")) {
        songs_json(out, size);
    } else if (nomi_equal(cmd, "online_site")) {
        char site[160];
        nomi_json_field(body, end, "site", site, sizeof(site));
        int len = nomi_len(site);
        while (len > 0 && (site[len - 1] == '/' || site[len - 1] == ' ')) site[--len] = 0;
        if (len && !nomi_starts(site, "http://") && !nomi_starts(site, "https://")) {
            nomi_fmt(out, size, "{\"ok\":false,\"error\":\"地址要以 http:// 或 https:// 开头\"}");
            return;
        }
        nomi_copy_n(g_site, sizeof(g_site), site, 0);
        kv_set("site", g_site);
        nomi_fmt(out, size, "{\"ok\":true}");
    } else if (nomi_equal(cmd, "online_search")) {
        char q[200];
        nomi_json_field(body, end, "q", q, sizeof(q));
        int n = search(q);
        if (n == NOMI_E_DENIED) nomi_fmt(out, size, "{\"ok\":false,\"error\":\"先填音乐网站\"}");
        else if (n < 0) nomi_fmt(out, size, "{\"ok\":false,\"error\":\"网站打不开，或者不是这种格式的网站\"}");
        else songs_json(out, size);
    } else if (nomi_equal(cmd, "online_play")) {
        char idx[16];
        nomi_json_field(body, end, "index", idx, sizeof(idx));
        if (play_from(nomi_atoi(idx), 0, 0) < 0) nomi_fmt(out, size, "{\"ok\":false,\"error\":\"这首放不了\"}");
        else nomi_fmt(out, size, "{\"ok\":true}");
    } else {
        nomi_fmt(out, size, "{\"ok\":false,\"error\":\"不认识的请求\"}");
    }
}
