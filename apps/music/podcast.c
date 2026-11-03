// 播客：订阅（kv "subs"，一行"名字\t订阅地址\t封面"）、读 RSS 最新几期、每个播客听到哪（kv "p<地址哈希>"：
// "节目地址\t秒\t听完了"）、上次听的那期（kv "pod_last"）。搜索用 Apple 播客目录。
// 放的时候交给系统按"节目"放（NOMI_PLAY_EPISODE）：左右键是倒回 15 秒 / 快进 30 秒，放完就停。
#include "music.h"

#define MAX_SUBS 12
#define MAX_EPS 10

typedef struct {
    char name[96], feed[256], art[256];
} sub_t;

typedef struct {
    char title[160], url[400], date[16];
    int32_t duration, bytes;
} ep_t;

static sub_t g_subs[MAX_SUBS];
static int g_sub_count;
static ep_t g_eps[MAX_EPS];  // 打开的那个播客的节目单
static int g_ep_count;
static sub_t g_ep_sub;       // 节目单是哪个播客的（可能没订阅）
static sub_t g_play_sub;     // 正在放 / 上次放的那期
static ep_t g_play_ep;
static int g_have_play;
static char g_kv[8192];

/* ---------- 小工具 ---------- */

// 一行按 \t 拆成 n 段（就地改），返回拆出几段
static int split(char *line, char **fields, int n)
{
    int k = 0;
    fields[k++] = line;
    for (char *p = line; *p && k < n; p++) {
        if (*p == '\t') {
            *p = 0;
            fields[k++] = p + 1;
        }
    }
    for (int i = k; i < n; i++) fields[i] = "";
    return k;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int contains(const char *s, const char *word)
{
    int n = nomi_len(s), m = nomi_len(word);
    for (int i = 0; i + m <= n; i++) {
        int k = 0;
        while (k < m && lower(s[i + k]) == lower(word[k])) k++;
        if (k == m) return 1;
    }
    return 0;
}

static void clock_text(int32_t s, char *out, int size)
{
    char mm[4] = {(char)('0' + s / 60 % 60 / 10), (char)('0' + s / 60 % 10), 0};
    char ss[4] = {(char)('0' + s % 60 / 10), (char)('0' + s % 10), 0};
    if (s >= 3600) nomi_fmt(out, size, "%d:%s:%s", s / 3600, mm, ss);
    else nomi_fmt(out, size, "%d:%s", s / 60, ss);
}

// itunes:duration："01:24:06"、"84:06" 或秒数
static int32_t parse_duration(const char *s)
{
    int32_t parts[3] = {0, 0, 0}, n = 0;
    for (const char *p = s; *p && n < 3;) {
        parts[n++] = nomi_atoi(p);
        while (*p && *p != ':') p++;
        if (*p == ':') p++;
        else break;
    }
    if (n == 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
    if (n == 2) return parts[0] * 60 + parts[1];
    return parts[0];
}

// pubDate（"Sat, 01 Nov 2025 07:00:00 GMT"）→ "11月1日"
static void short_date(const char *s, char *out, int size)
{
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    out[0] = 0;
    const char *end = s + nomi_len(s);
    for (int m = 0; m < 12; m++) {
        const char *p = nomi_find(s, end, months[m]);
        if (!p || p - s < 3) continue;
        int day = nomi_atoi(p - 3 + (p[-3] == ' ' ? 1 : 0));
        if (day <= 0) continue;
        nomi_fmt(out, size, "%d月%d日", m + 1, day);
        return;
    }
}

static void progress_key(const char *feed, char *key)
{
    char hex[9];
    nomi_hex32(nomi_hash(feed), hex);
    key[0] = 'p';
    nomi_copy_n(key + 1, 10, hex, 0);
}

// 这个播客上次听到哪：是这期、没听完才算（往回倒 3 秒接上前面那句）；done 不空时顺便说是不是听完了
static int32_t saved_position(const char *feed, const char *url, int *done)
{
    char key[12], buf[512];
    progress_key(feed, key);
    if (done) *done = 0;
    if (kv_get(key, buf, sizeof(buf)) <= 0) return 0;
    char *f[3];
    split(buf, f, 3);
    if (!nomi_equal(f[0], url)) return 0;
    if (nomi_atoi(f[2])) {
        if (done) *done = 1;
        return 0;
    }
    int32_t pos = nomi_atoi(f[1]) - 3;
    return pos > 0 ? pos : 0;
}

/* ---------- 订阅 ---------- */

static void save_subs(void)
{
    g_kv[0] = 0;
    for (int i = 0; i < g_sub_count; i++) {
        nomi_append(g_kv, sizeof(g_kv), g_subs[i].name);
        nomi_append(g_kv, sizeof(g_kv), "\t");
        nomi_append(g_kv, sizeof(g_kv), g_subs[i].feed);
        nomi_append(g_kv, sizeof(g_kv), "\t");
        nomi_append(g_kv, sizeof(g_kv), g_subs[i].art);
        nomi_append(g_kv, sizeof(g_kv), "\n");
    }
    kv_set("subs", g_kv);
}

static int read_sub_line(char *line, sub_t *s)
{
    char *f[3];
    split(line, f, 3);
    if (!f[0][0] || !f[1][0]) return 0;
    nomi_copy_n(s->name, sizeof(s->name), f[0], 0);
    nomi_copy_n(s->feed, sizeof(s->feed), f[1], 0);
    nomi_copy_n(s->art, sizeof(s->art), f[2], 0);
    return 1;
}

static void save_last(void)
{
    char dur[16], bytes[16];
    nomi_fmt(dur, sizeof(dur), "%d", g_play_ep.duration);
    nomi_fmt(bytes, sizeof(bytes), "%d", g_play_ep.bytes);
    g_kv[0] = 0;
    const char *parts[] = {g_play_sub.name, g_play_sub.feed, g_play_sub.art, g_play_ep.title, g_play_ep.url, dur, bytes};
    for (int i = 0; i < 7; i++) {
        if (i) nomi_append(g_kv, sizeof(g_kv), "\t");
        nomi_append(g_kv, sizeof(g_kv), parts[i]);
    }
    kv_set("pod_last", g_kv);
}

void pod_load(void)
{
    g_sub_count = 0;
    if (kv_get("subs", g_kv, sizeof(g_kv)) > 0) {
        for (char *p = g_kv; *p && g_sub_count < MAX_SUBS;) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p) *p++ = 0;
            if (read_sub_line(line, &g_subs[g_sub_count])) g_sub_count++;
        }
    }
    if (kv_get("pod_last", g_kv, sizeof(g_kv)) > 0) {
        char *f[7];
        split(g_kv, f, 7);
        nomi_copy_n(g_play_sub.name, sizeof(g_play_sub.name), f[0], 0);
        nomi_copy_n(g_play_sub.feed, sizeof(g_play_sub.feed), f[1], 0);
        nomi_copy_n(g_play_sub.art, sizeof(g_play_sub.art), f[2], 0);
        nomi_copy_n(g_play_ep.title, sizeof(g_play_ep.title), f[3], 0);
        nomi_copy_n(g_play_ep.url, sizeof(g_play_ep.url), f[4], 0);
        g_play_ep.duration = nomi_atoi(f[5]);
        g_play_ep.bytes = nomi_atoi(f[6]);
        g_have_play = g_play_ep.url[0] != 0;
    }
}

int pod_count(void) { return g_sub_count; }

static int find_sub(const char *query)
{
    for (int i = 0; i < g_sub_count; i++) {
        if (contains(g_subs[i].name, query) || contains(query, g_subs[i].name)) return i;
    }
    return -1;
}

static int add_sub(const sub_t *s)
{
    for (int i = 0; i < g_sub_count; i++) {
        if (nomi_equal(g_subs[i].feed, s->feed)) return 1;
    }
    if (g_sub_count >= MAX_SUBS) return NOMI_E_TOO_BIG;
    g_subs[g_sub_count++] = *s;
    save_subs();
    return 0;
}

static void remove_sub(const char *feed)
{
    for (int i = 0; i < g_sub_count; i++) {
        if (!nomi_equal(g_subs[i].feed, feed)) continue;
        for (int k = i; k + 1 < g_sub_count; k++) g_subs[k] = g_subs[k + 1];
        g_sub_count--;
        save_subs();
        return;
    }
}

/* ---------- 联网：Apple 播客目录、RSS ---------- */

static int itunes_search(const char *query, sub_t *out, int max)
{
    char enc[256], url[400];
    nomi_url_encode(query, enc, sizeof(enc));
    nomi_fmt(url, sizeof(url), "https://itunes.apple.com/search?media=podcast&entity=podcast&limit=%d&term=%s", max, enc);
    int32_t n = net_get(url, g_net, 128 * 1024);
    if (n < 0) return n;
    g_net[n] = 0;
    const char *end = g_net + n;
    int count = 0;
    for (const char *p = nomi_find(g_net, end, "{\"wrapperType\""); p && count < max;) {
        const char *next = nomi_find(p + 1, end, "{\"wrapperType\"");
        const char *e = next ? next : end;
        sub_t *s = &out[count];
        if (nomi_json_field(p, e, "collectionName", s->name, sizeof(s->name)) > 0 &&
            nomi_json_field(p, e, "feedUrl", s->feed, sizeof(s->feed)) > 0) {
            if (nomi_json_field(p, e, "artworkUrl100", s->art, sizeof(s->art)) < 0) s->art[0] = 0;
            count++;
        }
        p = next;
    }
    return count;
}

// 拉节目单：最新 MAX_EPS 期（有的 RSS 好几 MB，收到这么多期就断开）
static int fetch_episodes(const sub_t *sub)
{
    g_ep_count = 0;
    g_ep_sub = *sub;
    int32_t n = net_get_until(sub->feed, g_net, NET_BUF - 1, "</item>", MAX_EPS);
    if (n < 0) return n;
    g_net[n] = 0;
    const char *end = g_net + n;
    const char *first = nomi_find(g_net, end, "<item");
    if (!g_ep_sub.name[0] && first) nomi_xml_field(g_net, first, "title", 0, g_ep_sub.name, sizeof(g_ep_sub.name));
    char tmp[96];
    for (const char *p = first; p && g_ep_count < MAX_EPS;) {
        const char *e = nomi_find(p, end, "</item>");
        if (!e) break;
        ep_t *ep = &g_eps[g_ep_count];
        if (nomi_xml_field(p, e, "enclosure", "url", ep->url, sizeof(ep->url)) > 0) {
            if (nomi_xml_field(p, e, "title", 0, ep->title, sizeof(ep->title)) <= 0) nomi_copy_n(ep->title, 8, "未命名", 0);
            ep->duration = nomi_xml_field(p, e, "itunes:duration", 0, tmp, sizeof(tmp)) > 0 ? parse_duration(tmp) : 0;
            ep->bytes = nomi_xml_field(p, e, "enclosure", "length", tmp, sizeof(tmp)) > 0 ? nomi_atoi(tmp) : 0;
            if (ep->bytes < 0) ep->bytes = 0;  // 超过 2GB 的不管了
            if (nomi_xml_field(p, e, "pubDate", 0, tmp, sizeof(tmp)) > 0) short_date(tmp, ep->date, sizeof(ep->date));
            else ep->date[0] = 0;
            g_ep_count++;
        }
        p = nomi_find(e, end, "<item");
    }
    return g_ep_count > 0 ? g_ep_count : NOMI_E_FAIL;
}

/* ---------- 放 ---------- */

static int play(const sub_t *sub, const ep_t *ep)
{
    int32_t start = saved_position(sub->feed, ep->url, 0);
    int r = media_play(ep->url, ep->title, sub->name, NOMI_PLAY_EPISODE, start, ep->duration, ep->bytes);
    if (r < 0) return r;
    // 封面：Apple 的图能直接要指定尺寸（…/100x100bb.jpg → 88x88），下载、解码都省
    char art[300];
    nomi_copy_n(art, sizeof(art), sub->art, 0);
    char *p = (char *)nomi_find(art, art + nomi_len(art), "100x100bb");
    if (p) {
        char rest[200];
        nomi_copy_n(rest, sizeof(rest), p + 9, 0);
        nomi_copy_n(p, (int)(art + sizeof(art) - p), "88x88bb", 0);
        nomi_append(art, sizeof(art), rest);
    }
    media_meta(art, "");
    g_play_sub = *sub;
    g_play_ep = *ep;
    g_have_play = 1;
    save_last();
    set_source(SRC_PODCAST);
    update_context();
    return 0;
}

int pod_resume(void)
{
    if (!g_have_play) return NOMI_E_NOT_FOUND;
    return play(&g_play_sub, &g_play_ep);
}

int pod_play_latest(const char *query)
{
    if (!query[0]) return pod_resume();
    sub_t found;
    int i = find_sub(query);
    if (i >= 0) {
        found = g_subs[i];
    } else {
        int n = itunes_search(query, &found, 1);
        if (n < 0) return n;
        if (n == 0) return NOMI_E_NOT_FOUND;
    }
    int n = fetch_episodes(&found);
    if (n < 0) return n;
    return play(&g_ep_sub, &g_eps[0]);
}

int pod_subscribe(const char *query, char *name_out, int size)
{
    sub_t found;
    int n = itunes_search(query, &found, 1);
    if (n < 0) return n;
    if (n == 0) return NOMI_E_NOT_FOUND;
    nomi_copy_n(name_out, size, found.name, 0);
    return add_sub(&found);
}

void pod_save_progress(int done)
{
    if (g_src != SRC_PODCAST || !g_have_play || !media_mine()) return;
    int32_t pos = media_position();
    if (!done && pos <= 0) return;
    char key[12], buf[512], num[16];
    progress_key(g_play_sub.feed, key);
    nomi_fmt(num, sizeof(num), "%d", done ? 0 : pos);
    nomi_fmt(buf, sizeof(buf), "%s\t%s\t%d", g_play_ep.url, num, done);
    kv_set(key, buf);
}

void pod_media(int value)
{
    if (value == NOMI_MEDIA_FINISHED) {
        pod_save_progress(1);  // 听完了：下次从头放；不自动接下一期
    } else if (value == NOMI_MEDIA_NEXT || value == NOMI_MEDIA_PREV) {
        int32_t pos = media_position() + (value == NOMI_MEDIA_NEXT ? 30 : -15);
        media_seek(pos > 0 ? pos : 0);
    }
}

void pod_context(char *out, int size)
{
    char at[24];
    clock_text(media_mine() ? media_position() : 0, at, sizeof(at));
    nomi_fmt(out, size, "在听播客《%s》：%s（放到 %s）。", g_play_sub.name, g_play_ep.title, at);
}

/* ---------- 界面 ---------- */

void pod_show_subs(void)
{
    push_page("播客");
    if (g_have_play) {
        char sub[200];
        nomi_fmt(sub, sizeof(sub), "%s · %s", g_play_sub.name, g_play_ep.title);
        add_row(ui_item("接着听", sub, ""), ROW_POD_RESUME, 0, "");
    }
    if (g_sub_count == 0) {
        ui_text("还没订阅。跟它说「订阅忽左忽右」，或者在网页设置页的「应用 → 音乐 → 设置」里搜索订阅。", NOMI_TEXT_MUTED);
    }
    for (int i = 0; i < g_sub_count; i++) add_row(ui_item(g_subs[i].name, "", ">"), ROW_SUB, i, "");
}

static void show_episodes(const sub_t *sub)
{
    push_page(sub->name);
    int32_t loading = ui_text("加载中…", NOMI_TEXT_MUTED);
    int n = fetch_episodes(sub);
    ui_remove(loading);
    if (n < 0) {
        ui_text(n == NOMI_E_NO_NET ? "没联网。" : "节目单拉不下来，退出来再进一次试试。", NOMI_TEXT_MUTED);
        return;
    }
    for (int i = 0; i < g_ep_count; i++) {
        ep_t *ep = &g_eps[i];
        char sub_text[80], len[24], at[24];
        int done = 0;
        int32_t pos = saved_position(g_ep_sub.feed, ep->url, &done);
        clock_text(ep->duration, len, sizeof(len));
        if (done) nomi_fmt(sub_text, sizeof(sub_text), "%s · %s · 听完了", ep->date, len);
        else if (pos > 0) {
            clock_text(pos, at, sizeof(at));
            nomi_fmt(sub_text, sizeof(sub_text), "%s · %s · 听到 %s", ep->date, len, at);
        } else nomi_fmt(sub_text, sizeof(sub_text), "%s · %s", ep->date, ep->duration > 0 ? len : "");
        add_row(ui_item(ep->title, sub_text, ""), ROW_EPISODE, i, "");
    }
}

int pod_click(int kind, int index)
{
    if (kind == ROW_SUB && index < g_sub_count) {
        show_episodes(&g_subs[index]);
        return 1;
    }
    if (kind == ROW_EPISODE && index < g_ep_count) {
        if (play(&g_ep_sub, &g_eps[index]) < 0) ui_toast("这期放不了");
        else back_home();
        return 1;
    }
    if (kind == ROW_POD_RESUME) {
        if (pod_resume() < 0) ui_toast("放不了");
        else back_home();
        return 1;
    }
    return 0;
}

/* ---------- 网页 ---------- */

static void sub_json(char *out, int size, const sub_t *s)
{
    nomi_append(out, size, "{\"name\":");
    json_str(out, size, s->name);
    nomi_append(out, size, ",\"feed\":");
    json_str(out, size, s->feed);
    nomi_append(out, size, ",\"art\":");
    json_str(out, size, s->art);
    nomi_append(out, size, "}");
}

static void subs_json(char *out, int size, const sub_t *list, int n, const char *key)
{
    nomi_append(out, size, "{\"ok\":true,\"");
    nomi_append(out, size, key);
    nomi_append(out, size, "\":[");
    for (int i = 0; i < n; i++) {
        if (i) nomi_append(out, size, ",");
        sub_json(out, size, &list[i]);
    }
    nomi_append(out, size, "]}");
}

void pod_web(const char *body, char *out, int size)
{
    const char *end = body + nomi_len(body);
    char cmd[32];
    nomi_json_field(body, end, "cmd", cmd, sizeof(cmd));
    if (nomi_equal(cmd, "pod_list")) {
        subs_json(out, size, g_subs, g_sub_count, "subs");
    } else if (nomi_equal(cmd, "pod_play")) {  // 在设备上放这个订阅的最新一期（没听完的接着放）
        char feed[256];
        nomi_json_field(body, end, "feed", feed, sizeof(feed));
        int i = 0;
        while (i < g_sub_count && !nomi_equal(g_subs[i].feed, feed)) i++;
        int r = i < g_sub_count ? fetch_episodes(&g_subs[i]) : NOMI_E_NOT_FOUND;
        if (r >= 0) r = play(&g_ep_sub, &g_eps[0]);
        if (r < 0) nomi_fmt(out, size, "{\"ok\":false,\"error\":\"%s\"}", r == NOMI_E_NOT_FOUND ? "没订阅这个" : "节目单拉不下来或放不了");
        else {
            nomi_append(out, size, "{\"ok\":true,\"title\":");
            json_str(out, size, g_play_ep.title);
            nomi_append(out, size, "}");
        }
    } else if (nomi_equal(cmd, "pod_search")) {
        static sub_t results[8];
        char q[200];
        nomi_json_field(body, end, "q", q, sizeof(q));
        int n = q[0] ? itunes_search(q, results, 8) : 0;
        if (n < 0) nomi_fmt(out, size, "{\"ok\":false,\"error\":\"%s\"}", n == NOMI_E_NO_NET ? "设备没联网" : "播客目录连不上");
        else subs_json(out, size, results, n, "results");
    } else if (nomi_equal(cmd, "pod_subscribe")) {
        sub_t s;
        nomi_json_field(body, end, "name", s.name, sizeof(s.name));
        nomi_json_field(body, end, "art", s.art, sizeof(s.art));
        if (nomi_json_field(body, end, "feed", s.feed, sizeof(s.feed)) <= 0 || !s.name[0]) {
            nomi_fmt(out, size, "{\"ok\":false,\"error\":\"缺名字或订阅地址\"}");
            return;
        }
        int r = add_sub(&s);
        if (r == NOMI_E_TOO_BIG) nomi_fmt(out, size, "{\"ok\":false,\"error\":\"最多订阅 %d 个\"}", MAX_SUBS);
        else nomi_fmt(out, size, "{\"ok\":true}");
    } else if (nomi_equal(cmd, "pod_unsubscribe")) {
        char feed[256];
        nomi_json_field(body, end, "feed", feed, sizeof(feed));
        remove_sub(feed);
        nomi_fmt(out, size, "{\"ok\":true}");
    } else {
        nomi_fmt(out, size, "{\"ok\":false,\"error\":\"不认识的请求\"}");
    }
}
