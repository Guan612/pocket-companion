// 地图：周围的道路（SD 卡上的离线地图包）+ 手机导航的转向（Gadgetbridge）。
//
// 系统给能力：手机连接（phone_*：蓝牙收 Gadgetbridge 的消息）、画布（ui_canvas、canvas_*）、读卡、屏幕常亮。
// 应用管业务：
// - 道路：卡上 maps/*.nmap（电脑上用 tools/map/make_map.py 从 OpenStreetMap 做），按定位读周围 3×3 块（约 3 公里
//   见方）。车头朝上画：自己的位置固定在下方，地图跟着朝向转。按出行方式（步行 / 骑车 / 开车）画能走的路——
//   能走的亮、不能走的压暗垫底（切换时一眼能看出哪些路变了），缩放也跟着变（步行看近、开车看远）。
//   不存轨迹：走过的路画出来对导航没用，还白占内存。还没走的路线 Gadgetbridge 也不给（nav 消息只有
//   转向、距离、路名、到达时间，没有坐标），所以地图上没有路线高亮，该往哪走看底栏。
// - 导航：手机上用 OsmAnd 或 CoMaps 导航，Gadgetbridge 把转向、距离、路名、到达时间转过来，放在底栏。
// - 定位：手机 GPS（Gadgetbridge 里给这台设备打开「使用手机的GPS数据」）。界面挂着时让手机推、拿下时停；
//   在导航或定位一直在来时屏幕常亮。
// - 点屏幕出菜单：换出行方式。
// kv：mode（0 步行、1 骑车、2 开车）
#include "nomi_sdk.h"
#include "nomi_text.h"

#define C_TEXT 0xe8eaf0
#define C_MUTED 0x8b90a0
#define C_DIM 0x3a3f50
#define C_WARM 0xffc857
#define C_DANGER 0xff6b6b
#define C_BG 0x000000
#define C_BAR 0x0d0f14

#define PI 3.14159265358979
#define FRESH_S 10   // 定位多久以内算新的（秒）
#define REASK_S 30   // 这么久没收到定位就再让手机推一次
#define PACK_MAX 8
#define TILES_BUF (96 * 1024)  // 周围 9 块：成都城里一块平均 1.5KB、最多几 KB，留足余量
#define PTS_MAX 1024

enum { TIMER_TICK = 1 };

static int32_t g_canvas;  // 0 = 界面没挂着
static int g_pages;       // 压了几页（1 地图，2 菜单）
static int g_connected;
static int g_keep_on;     // 现在让屏幕常亮着没有
static int32_t g_tick;    // 界面挂着时每秒加一
static int32_t g_asked_tick = -1000;

// 出行方式
static int g_mode;
static const char *const kModeNames[] = {"步行", "骑车", "开车"};
static const char *const kModeSubs[] = {"人行道、小路都画，看近一点", "亮的是自行车能走的路", "亮的是车能走的路，看远一点"};
static const int kModeBits[] = {1, 2, 4};
static const double kModeScale[] = {1.1, 0.65, 0.38};  // 每米几个像素
static int32_t g_mode_rows[3];

// 导航（Gadgetbridge 的 {"t":"nav",…}）
static int g_nav;
static char g_action[24], g_instr[160], g_eta[24], g_dist_raw[40];
static double g_dist_m = -1;  // 到下一个路口多少米；单位认不出为 -1（显示原文）

// 定位（{"t":"gps",…}）
static int g_fix;
static int32_t g_fix_tick = -1000;
static double g_lat, g_lon;
static double g_speed = -1;    // 千米每小时
static double g_heading = -1;  // 度，北 0 顺时针；不知道为 -1
static int g_prev_ok;          // 有没有上一次的定位（算朝向、滤飘动用）
static double g_prev_lat, g_prev_lon;

// 卡上的地图包、读进来的周围 3×3 块
static struct {
    char path[96];
    int32_t lat0, lon0;  // 起始块号（度 × 100）
    int rows, cols;
} g_packs[PACK_MAX];
static int g_pack_n;
static uint8_t g_tiles[TILES_BUF];
static struct {
    int32_t lat_i, lon_i, off, len;
} g_tile[9];
static int g_tile_n;
static int32_t g_center_lat_i = -1000000, g_center_lon_i = -1000000;  // 读的是哪块周围
static int g_in_map;  // 这一带在地图包的范围里

static char g_msg[4096];
static int32_t g_pts[PTS_MAX * 2];

/* ---------- 小工具 ---------- */

static double wrap180(double d)
{
    while (d >= 180) d -= 360;
    while (d < -180) d += 360;
    return d;
}

static double dsin(double deg)
{
    double d = wrap180(deg);
    if (d > 90) d = 180 - d;
    else if (d < -90) d = -180 - d;
    double x = d * PI / 180, x2 = x * x;
    return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
}

static double dcos(double deg) { return dsin(deg + 90); }

static double dfloor(double v)
{
    double t = (double)(int64_t)v;
    return t > v ? t - 1 : t;
}

// 从北顺时针的角度（0..360）：dx 往东、dy 往北
static double bearing(double dx, double dy)
{
    double ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ax == 0 && ay == 0) return 0;
    double z = ax < ay ? ax / ay : ay / ax;
    double a = (PI / 4) * z - z * (z - 1) * (0.2447 + 0.0663 * z);  // atan(z)，误差 0.1° 以内
    a = a * 180 / PI;
    if (ax > ay) a = 90 - a;  // 现在 a 是偏离北的角度（0..90）
    if (dy < 0) a = 180 - a;
    if (dx < 0) a = 360 - a;
    return a;
}

static double parse_double(const char *s)
{
    while (*s == ' ') s++;
    int neg = 0;
    if (*s == '-') neg = 1, s++;
    else if (*s == '+') s++;
    double v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == '.' || *s == ',') {
        double f = 0.1;
        for (s++; *s >= '0' && *s <= '9'; s++, f *= 0.1) v += (*s - '0') * f;
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        int eneg = 0, e = 0;
        if (*s == '-') eneg = 1, s++;
        else if (*s == '+') s++;
        while (*s >= '0' && *s <= '9') e = e * 10 + (*s++ - '0');
        while (e-- > 0) v = eneg ? v / 10 : v * 10;
    }
    return neg ? -v : v;
}

// 截短到大约 max_units 个半角宽（汉字算两个），放不下的换成省略号
static void fit_text(char *s, int max_units)
{
    int units = 0, cut = -1;
    for (int i = 0; s[i];) {
        unsigned char c = (unsigned char)s[i];
        int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        int w = c < 0x80 ? 1 : 2;
        if (cut < 0 && units + w > max_units - 2) cut = i;
        units += w;
        if (units > max_units) {
            s[cut] = 0;
            nomi_append(s, cut + 4, "…");
            return;
        }
        i += len;
    }
}

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

static const char *const kDirNames[] = {"北", "东北", "东", "东南", "南", "西南", "西", "西北"};

static const char *dir_name(double deg) { return kDirNames[((int)((deg + 22.5) / 45)) % 8]; }

static const char *action_name(const char *a)
{
    static const struct {
        const char *action, *name;
    } kNames[] = {{"continue", "直行"},
                  {"left", "左转"},
                  {"left_slight", "向左前方"},
                  {"left_sharp", "向左后方"},
                  {"right", "右转"},
                  {"right_slight", "向右前方"},
                  {"right_sharp", "向右后方"},
                  {"keep_left", "靠左"},
                  {"keep_right", "靠右"},
                  {"uturn_left", "掉头"},
                  {"uturn_right", "掉头"},
                  {"offroute", "偏离路线"},
                  {"roundabout_right", "进环岛右转"},
                  {"roundabout_left", "进环岛左转"},
                  {"roundabout_straight", "进环岛直行"},
                  {"roundabout_uturn", "进环岛掉头"},
                  {"finish", "到达目的地"}};
    for (unsigned i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        if (nomi_equal(a, kNames[i].action)) return kNames[i].name;
    }
    return "";
}

/* ---------- 地图包 ---------- */

// 找卡上 maps/ 里的 .nmap，记下每个包管哪一片
static void scan_packs(void)
{
    g_pack_n = 0;
    if (fs_list("sd:/maps", g_msg, sizeof(g_msg)) < 0) return;
    char *p = g_msg;
    while (*p && g_pack_n < PACK_MAX) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        int n = nomi_len(line);
        if (n < 6 || !nomi_equal(line + n - 5, ".nmap")) continue;
        char path[96];
        nomi_fmt(path, sizeof(path), "sd:/maps/%s", line);
        uint8_t h[20];
        if (fs_read(path, 0, h, 20) != 20 || h[0] != 'N' || h[1] != 'M' || h[2] != 'A' || h[3] != 'P' || rd32(h + 4) != 1)
            continue;
        nomi_copy_n(g_packs[g_pack_n].path, sizeof(g_packs[0].path), path, 0);
        g_packs[g_pack_n].lat0 = (int32_t)rd32(h + 8);
        g_packs[g_pack_n].lon0 = (int32_t)rd32(h + 12);
        g_packs[g_pack_n].rows = (int)rd16(h + 16);
        g_packs[g_pack_n].cols = (int)rd16(h + 18);
        g_pack_n++;
    }
}

// 读 (lat_i, lon_i) 这块和周围 8 块（块号 = floor(度 × 100)）
static void load_tiles(int32_t lat_i, int32_t lon_i)
{
    g_center_lat_i = lat_i;
    g_center_lon_i = lon_i;
    g_tile_n = 0;
    g_in_map = 0;
    int32_t used = 0;
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            int32_t ti = lat_i + dr, tj = lon_i + dc;
            for (int k = 0; k < g_pack_n; k++) {
                int r = ti - g_packs[k].lat0, c = tj - g_packs[k].lon0;
                if (r < 0 || c < 0 || r >= g_packs[k].rows || c >= g_packs[k].cols) continue;
                uint8_t ix[8];
                if (fs_read(g_packs[k].path, 20 + 8 * (r * g_packs[k].cols + c), ix, 8) != 8) break;
                g_in_map = 1;
                int32_t off = (int32_t)rd32(ix), len = (int32_t)rd32(ix + 4);
                if (len <= 0 || used + len > TILES_BUF) break;
                int32_t got = 0;
                while (got < len) {  // 一次最多读 32KB
                    int32_t want = len - got > 32768 ? 32768 : len - got;
                    int32_t n = fs_read(g_packs[k].path, off + got, g_tiles + used + got, want);
                    if (n <= 0) break;
                    got += n;
                }
                if (got == len) {
                    g_tile[g_tile_n].lat_i = ti;
                    g_tile[g_tile_n].lon_i = tj;
                    g_tile[g_tile_n].off = used;
                    g_tile[g_tile_n].len = len;
                    g_tile_n++;
                    used += len;
                }
                break;
            }
        }
    }
}

static void update_tiles(void)
{
    int32_t lat_i = (int32_t)dfloor(g_lat * 100), lon_i = (int32_t)dfloor(g_lon * 100);
    if (lat_i != g_center_lat_i || lon_i != g_center_lon_i) load_tiles(lat_i, lon_i);
}

/* ---------- 收消息 ---------- */

// 距离可能是数字（米），也可能是带单位的字符串（"200 m"、"1.2 km"、"966yd"……）
static void parse_distance(const char *json, const char *end)
{
    char v[40];
    g_dist_m = -1;
    g_dist_raw[0] = 0;
    if (nomi_json_field(json, end, "distance", v, sizeof(v)) < 0 || !v[0]) return;
    nomi_copy_n(g_dist_raw, sizeof(g_dist_raw), v, 0);
    const char *p = v;
    while (*p == ' ') p++;
    if (!(*p >= '0' && *p <= '9')) return;
    double num = parse_double(p);
    while ((*p >= '0' && *p <= '9') || *p == '.' || *p == ',') p++;
    while (*p == ' ') p++;
    if (!*p || nomi_starts(p, "米")) g_dist_m = num;
    else if (nomi_starts(p, "km") || nomi_starts(p, "公里") || nomi_starts(p, "千米")) g_dist_m = num * 1000;
    else if (nomi_starts(p, "mi")) g_dist_m = num * 1609.34;
    else if (nomi_starts(p, "m")) g_dist_m = num;
    else if (nomi_starts(p, "yd")) g_dist_m = num * 0.9144;
    else if (nomi_starts(p, "ft")) g_dist_m = num * 0.3048;
}

static void on_nav(const char *json, const char *end)
{
    int a = nomi_json_field(json, end, "action", g_action, sizeof(g_action)) >= 0;
    int i = nomi_json_field(json, end, "instr", g_instr, sizeof(g_instr)) >= 0;
    int e = nomi_json_field(json, end, "eta", g_eta, sizeof(g_eta)) >= 0;
    parse_distance(json, end);
    g_nav = a || i || e || g_dist_raw[0];  // 只有 "t" = 导航结束了
}

static void on_gps(const char *json, const char *end)
{
    char v[40];
    if (nomi_json_field(json, end, "lat", v, sizeof(v)) < 0) return;
    double lat = parse_double(v);
    if (nomi_json_field(json, end, "lon", v, sizeof(v)) < 0 && nomi_json_field(json, end, "long", v, sizeof(v)) < 0) return;
    double lon = parse_double(v);
    if (lat == 0 && lon == 0) return;
    g_fix = 1;
    g_lat = lat;
    g_lon = lon;
    g_fix_tick = g_tick;
    g_speed = nomi_json_field(json, end, "speed", v, sizeof(v)) >= 0 ? parse_double(v) : -1;
    double course = nomi_json_field(json, end, "course", v, sizeof(v)) >= 0 ? parse_double(v) : -1;
    if (g_prev_ok) {
        double dx = (lon - g_prev_lon) * 111320.0 * dcos(lat), dy = (lat - g_prev_lat) * 110574.0;
        if (__builtin_sqrt(dx * dx + dy * dy) >= 3) {  // 走了 3 米以上才动基准点、算朝向（原地不动时定位会飘）
            if (course < 0) g_heading = bearing(dx, dy);
            g_prev_lat = lat;
            g_prev_lon = lon;
        }
    } else {
        g_prev_ok = 1;
        g_prev_lat = lat;
        g_prev_lon = lon;
    }
    if (course >= 0 && g_speed >= 1) g_heading = course;
    update_tiles();
}

static void handle(const char *json, int len)
{
    const char *end = json + len;
    char t[16];
    if (nomi_json_field(json, end, "t", t, sizeof(t)) < 0) return;
    if (nomi_equal(t, "nav")) on_nav(json, end);
    else if (nomi_equal(t, "gps")) on_gps(json, end);
}

static void ask_gps(int on)
{
    phone_send(on ? "{\"t\":\"gps_power\",\"status\":true}" : "{\"t\":\"gps_power\",\"status\":false}");
    g_asked_tick = g_tick;
}

/* ---------- 画 ---------- */

static int text(int x, int y, const char *s, int size, int color, int align)
{
    return canvas_text(g_canvas, x, y, s, size, color, align);
}

static void line(const int32_t *xy, int points, int width, int color)
{
    canvas_line(g_canvas, xy, points * 8, width, color);
}

static void rect(int x0, int y0, int x1, int y1, int color)
{
    int32_t q[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
    canvas_fill(g_canvas, q, sizeof(q), color);
}

typedef struct {
    int cx, cy, s;  // 箭头框的中心、边长：形状按 100×100 的格子写
} Box;

static double box_x(const Box *b, double u) { return b->cx + (u - 50) * b->s / 100.0; }
static double box_y(const Box *b, double v) { return b->cy + (v - 50) * b->s / 100.0; }

// 一条折线（格子坐标，mirror 时左右翻过来），head 时最后一段画成箭头
static void path(const Box *b, const double *uv, int n, int mirror, int width, int color, int head)
{
    double x[24], y[24];
    int32_t pts[48];
    if (n > 24) n = 24;
    for (int i = 0; i < n; i++) {
        x[i] = box_x(b, mirror ? 100 - uv[i * 2] : uv[i * 2]);
        y[i] = box_y(b, uv[i * 2 + 1]);
    }
    int32_t tri[6];
    if (head && n >= 2) {
        double dx = x[n - 1] - x[n - 2], dy = y[n - 1] - y[n - 2];
        double len = __builtin_sqrt(dx * dx + dy * dy);
        if (len < 1) len = 1;
        double ux = dx / len, uy = dy / len, hl = width * 2.0, hw = width * 1.6;
        double tx = x[n - 1], ty = y[n - 1], bx = tx - ux * hl, by = ty - uy * hl;
        tri[0] = (int32_t)tx, tri[1] = (int32_t)ty;
        tri[2] = (int32_t)(bx - uy * hw), tri[3] = (int32_t)(by + ux * hw);
        tri[4] = (int32_t)(bx + uy * hw), tri[5] = (int32_t)(by - ux * hw);
        x[n - 1] = bx + ux * 2;  // 线收进箭头底边一点，别露缝
        y[n - 1] = by + uy * 2;
    }
    for (int i = 0; i < n; i++) pts[i * 2] = (int32_t)x[i], pts[i * 2 + 1] = (int32_t)y[i];
    line(pts, n, width, color);
    if (head && n >= 2) canvas_fill(g_canvas, tri, sizeof(tri), color);
}

static void draw_arrow(const Box *b, const char *a)
{
    int w = b->s * 9 / 100;
    if (w < 3) w = 3;
    int right = nomi_starts(a, "right") || nomi_equal(a, "uturn_right") || nomi_equal(a, "keep_right");
    // 形状都按左边写，右边的镜像
    static const double kStraight[] = {50, 92, 50, 8};
    static const double kTurn[] = {62, 92, 62, 50, 12, 50};
    static const double kSlight[] = {60, 92, 60, 56, 24, 20};
    static const double kSharp[] = {66, 92, 66, 36, 24, 78};
    static const double kKeep[] = {50, 92, 50, 60, 26, 18};
    static const double kKeepOther[] = {50, 60, 74, 18};
    if (nomi_equal(a, "left") || nomi_equal(a, "right")) {
        path(b, kTurn, 3, right, w, C_TEXT, 1);
    } else if (nomi_equal(a, "left_slight") || nomi_equal(a, "right_slight")) {
        path(b, kSlight, 3, right, w, C_TEXT, 1);
    } else if (nomi_equal(a, "left_sharp") || nomi_equal(a, "right_sharp")) {
        path(b, kSharp, 3, right, w, C_TEXT, 1);
    } else if (nomi_equal(a, "keep_left") || nomi_equal(a, "keep_right")) {
        path(b, kKeepOther, 2, right, w * 2 / 3, C_DIM, 0);
        path(b, kKeep, 3, right, w, C_TEXT, 1);
    } else if (nomi_starts(a, "uturn")) {
        // 往上走，绕个半圆（圆心 50,40，半径 16），再往下
        double uv[24];
        int n = 0;
        uv[n++] = 66, uv[n++] = 92;
        for (int k = 0; k <= 6; k++) {
            uv[n++] = 50 + 16 * dcos(k * 30.0);
            uv[n++] = 40 - 16 * dsin(k * 30.0);
        }
        uv[n++] = 34, uv[n++] = 82;
        path(b, uv, n / 2, right, w, C_TEXT, 1);
    } else if (nomi_starts(a, "roundabout")) {
        canvas_circle(g_canvas, (int)box_x(b, 50), (int)box_y(b, 42), b->s * 18 / 100, w * 2 / 3, C_DIM);
        static const double kIn[] = {50, 92, 50, 62};
        path(b, kIn, 2, 0, w, C_TEXT, 0);
        static const double kOutRight[] = {68, 42, 94, 42};
        static const double kOutLeft[] = {32, 42, 6, 42};
        static const double kOutStraight[] = {50, 22, 50, 2};
        static const double kOutBack[] = {36, 54, 28, 92};
        const double *out = nomi_equal(a, "roundabout_right")  ? kOutRight
                            : nomi_equal(a, "roundabout_left") ? kOutLeft
                            : nomi_equal(a, "roundabout_uturn") ? kOutBack
                                                                 : kOutStraight;
        path(b, out, 2, 0, w, C_TEXT, 1);
    } else if (nomi_equal(a, "finish")) {
        // 终点：一个大头针
        int32_t tri[6] = {(int32_t)box_x(b, 30), (int32_t)box_y(b, 50), (int32_t)box_x(b, 70),
                          (int32_t)box_y(b, 50), (int32_t)box_x(b, 50), (int32_t)box_y(b, 92)};
        canvas_fill(g_canvas, tri, sizeof(tri), C_WARM);
        canvas_circle(g_canvas, (int)box_x(b, 50), (int)box_y(b, 38), b->s * 24 / 100, 0, C_WARM);
        canvas_circle(g_canvas, (int)box_x(b, 50), (int)box_y(b, 38), b->s * 9 / 100, 0, C_BAR);
    } else if (nomi_equal(a, "offroute")) {
        canvas_circle(g_canvas, b->cx, b->cy, b->s * 40 / 100, w * 2 / 3, C_DANGER);
        static const double kBang[] = {50, 28, 50, 56};
        path(b, kBang, 2, 0, w, C_DANGER, 0);
        canvas_circle(g_canvas, b->cx, (int)box_y(b, 70), w * 2 / 3, 0, C_DANGER);
    } else {
        path(b, kStraight, 2, 0, w, nomi_equal(a, "continue") ? C_TEXT : C_MUTED, 1);
    }
}

static int fresh(void) { return g_fix && g_tick - g_fix_tick <= FRESH_S; }

// 道路的样子：0 主干、1 次干、2 支路、3 小路；不能走的压暗到 55% 垫底，亮暗是「这个模式能不能走」的信号
static const int kRoadWidth[] = {7, 5, 3, 2};
static const int kRoadColor[] = {0x8e94a3, 0x6b7180, 0x4b505e, 0x474d5a};
static const int kRoadGhost[] = {0x4e5159, 0x3a3e46, 0x292c33, 0x272a31};

// 车头朝上：自己在 (cx, cy)，朝向指向屏幕上方；scale 是每米几个像素。h_top 以下才画（底栏不画）
static void draw_roads(int cx, int cy, double scale, int bottom)
{
    double m_lat = 1.10574, m_lon = 1.11320 * dcos(g_lat);  // 1e-5° 是多少米
    double h = g_heading >= 0 ? g_heading : 0, ch = dcos(h), sh = dsin(h);
    double plat = g_lat * 1e5, plon = g_lon * 1e5;
    int bit = kModeBits[g_mode];
    for (int pass = 0; pass < 2; pass++) {  // 先画不能走的（暗），能走的亮线压在上面；小路先画，主干最上
        for (int cls = 3; cls >= 0; cls--) {
            for (int t = 0; t < g_tile_n; t++) {
                const uint8_t *p = g_tiles + g_tile[t].off, *end = p + g_tile[t].len;
                double base_lat = g_tile[t].lat_i * 1000.0 - plat, base_lon = g_tile[t].lon_i * 1000.0 - plon;
                int count = (int)rd16(p);
                p += 2;
                for (int i = 0; i < count && p + 4 <= end; i++) {
                    int c = p[0], modes = p[1], n = (int)rd16(p + 2);
                    const uint8_t *q = p + 4;
                    p = q + n * 4;
                    if (p > end) break;
                    int ok = (modes & bit) != 0;
                    if (c != cls || n < 2 || ok != (pass == 1)) continue;
                    if (n > PTS_MAX) n = PTS_MAX;
                    int32_t minx = 1 << 30, maxx = -(1 << 30), miny = 1 << 30, maxy = -(1 << 30);
                    for (int k = 0; k < n; k++) {
                        double dx = (base_lon + (int16_t)rd16(q + k * 4)) * m_lon;
                        double dy = (base_lat + (int16_t)rd16(q + k * 4 + 2)) * m_lat;
                        double rx = dx * ch - dy * sh, ry = dx * sh + dy * ch;
                        int32_t sx = cx + (int32_t)(rx * scale), sy = cy - (int32_t)(ry * scale);
                        g_pts[k * 2] = sx;
                        g_pts[k * 2 + 1] = sy;
                        if (sx < minx) minx = sx;
                        if (sx > maxx) maxx = sx;
                        if (sy < miny) miny = sy;
                        if (sy > maxy) maxy = sy;
                    }
                    if (maxx < -20 || minx > 500 || maxy < -20 || miny > bottom + 20) continue;  // 不在屏幕上
                    line(g_pts, n, kRoadWidth[cls], ok ? kRoadColor[cls] : kRoadGhost[cls]);
                }
            }
        }
    }
}

// 自己：朝上的白三角（不知道朝向时是个圆点），外面描一圈黑边看得清
static void draw_me(int cx, int cy)
{
    if (g_heading >= 0) {
        int32_t outer[6] = {cx, cy - 18, cx + 13, cy + 12, cx - 13, cy + 12};
        int32_t inner[6] = {cx, cy - 13, cx + 9, cy + 9, cx - 9, cy + 9};
        canvas_fill(g_canvas, outer, sizeof(outer), C_BG);
        canvas_fill(g_canvas, inner, sizeof(inner), C_TEXT);
    } else {
        canvas_circle(g_canvas, cx, cy, 10, 0, C_BG);
        canvas_circle(g_canvas, cx, cy, 7, 0, C_TEXT);
    }
}

// 右上角的小指北针：红点指着北
static void draw_north(int x, int y)
{
    double h = g_heading >= 0 ? g_heading : 0;
    int nx = x - (int)(dsin(h) * 13), ny = y - (int)(dcos(h) * 13);
    canvas_circle(g_canvas, x, y, 17, 2, C_DIM);
    int32_t l[4] = {x, y, nx, ny};
    line(l, 2, 2, C_MUTED);
    canvas_circle(g_canvas, nx, ny, 4, 0, C_DANGER);
}

// 地图那一块：道路、自己、指北针；bottom 以下留给底栏
static void draw_map(int cx, int cy, int bottom)
{
    double scale = kModeScale[g_mode];
    if (g_fix) {
        draw_roads(cx, cy, scale, bottom);
        draw_me(cx, cy);
    }
    draw_north(452, 30);
    if (g_fix && !g_in_map) {
        text(240, 70, "这一带没有离线地图", NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_CENTER);
        text(240, 102, "电脑上用 tools/map 做一份放进卡上 maps/", NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_CENTER);
    }
}

static void distance_text(char *out, int size)
{
    double m = g_dist_m;
    if (m < 0) {
        nomi_copy_n(out, size, g_dist_raw, 0);
    } else if (m < 1000) {
        int v = m < 50 ? ((int)(m + 2.5) / 5) * 5 : ((int)(m + 5) / 10) * 10;
        nomi_fmt(out, size, "%d 米", v);
    } else if (m < 9950) {
        int tenths = (int)(m / 100 + 0.5);
        nomi_fmt(out, size, "%d.%d 公里", tenths / 10, tenths % 10);
    } else {
        nomi_fmt(out, size, "%d 公里", (int)(m / 1000 + 0.5));
    }
}

static void speed_text(char *out, int size)
{
    if (fresh() && g_speed >= 0) nomi_fmt(out, size, "%d 公里/小时", (int)(g_speed + 0.5));
    else if (g_fix) nomi_copy_n(out, size, "定位断了", 0);
    else nomi_copy_n(out, size, "等手机的定位…", 0);
}

// 导航：上面地图，底栏转向、距离、路名、到达时间
static void draw_nav(void)
{
    draw_map(240, 178, 236);
    rect(0, 236, 480, 320, C_BAR);
    int32_t edge[4] = {0, 236, 480, 236};
    line(edge, 2, 2, C_DIM);
    Box box = {46, 278, 66};
    draw_arrow(&box, g_action);
    char line1[200], line2[200], d[40];
    if (nomi_equal(g_action, "finish")) {
        nomi_copy_n(line1, sizeof(line1), "到达目的地", 0);
        nomi_copy_n(line2, sizeof(line2), g_instr, 0);
    } else if (nomi_equal(g_action, "offroute")) {
        nomi_copy_n(line1, sizeof(line1), "偏离路线", 0);
        nomi_copy_n(line2, sizeof(line2), "手机在重新规划", 0);
    } else {
        distance_text(d, sizeof(d));
        nomi_fmt(line1, sizeof(line1), "%s%s%s", d, d[0] ? "后" : "", action_name(g_action));
        nomi_copy_n(line2, sizeof(line2), g_instr, 0);
    }
    fit_text(line1, 22);
    fit_text(line2, 24);
    text(92, 246, line1, NOMI_CANVAS_TEXT, nomi_equal(g_action, "offroute") ? C_DANGER : C_TEXT, NOMI_ALIGN_LEFT);
    text(92, 280, line2, NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_LEFT);
    if (g_eta[0]) {
        char eta[48];
        nomi_fmt(eta, sizeof(eta), "%s 到", g_eta);
        text(466, 246, eta, NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_RIGHT);
    }
    if (fresh() && g_speed >= 0) {
        char s[24];
        nomi_fmt(s, sizeof(s), "%d km/h", (int)(g_speed + 0.5));
        text(466, 280, s, NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_RIGHT);
    }
}

// 没在导航：整屏地图，左上速度和朝向，左下出行方式
static void draw_free(void)
{
    if (!g_fix) {
        text(24, 24, "等手机的定位…", NOMI_CANVAS_TEXT, C_TEXT, NOMI_ALIGN_LEFT);
        text(24, 76, "要在 Gadgetbridge 里给这台设备", NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_LEFT);
        text(24, 108, "打开「使用手机的GPS数据」", NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_LEFT);
        text(24, 172, "导航：手机上用 OsmAnd 或 CoMaps，", NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_LEFT);
        text(24, 204, "转向就显示在这里", NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_LEFT);
        return;
    }
    draw_map(240, 214, 320);
    char s[48];
    speed_text(s, sizeof(s));
    text(16, 12, s, NOMI_CANVAS_TEXT, fresh() ? C_TEXT : C_MUTED, NOMI_ALIGN_LEFT);
    if (g_heading >= 0) {
        nomi_fmt(s, sizeof(s), "朝%s", dir_name(g_heading));
        text(16, 44, s, NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_LEFT);
    }
    nomi_fmt(s, sizeof(s), "%s · 点屏幕换", kModeNames[g_mode]);
    text(16, 286, s, NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_LEFT);
    nomi_fmt(s, sizeof(s), "前方约 %d 米", (int)(214 / kModeScale[g_mode] / 10) * 10);
    text(466, 286, s, NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_RIGHT);
}

static void draw_disconnected(void)
{
    text(24, 24, "地图", NOMI_CANVAS_TEXT, C_TEXT, NOMI_ALIGN_LEFT);
    text(24, 76, "手机没连上", NOMI_CANVAS_TEXT, C_TEXT, NOMI_ALIGN_LEFT);
    text(24, 124, "手机装 Gadgetbridge，添加设备", NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_LEFT);
    text(24, 156, "「Bangle.js Nomi」；再用 OsmAnd", NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_LEFT);
    text(24, 188, "或 CoMaps 导航，转向就显示在这里", NOMI_CANVAS_TEXT, C_MUTED, NOMI_ALIGN_LEFT);
}

static void update_context(void)
{
    char ctx[400];
    if (!g_connected) {
        nomi_copy_n(ctx, sizeof(ctx), "主人开着地图，手机没连上（要在手机的 Gadgetbridge 里连这台设备）。", 0);
    } else if (g_nav) {
        char dist[24] = "";
        if (g_dist_m >= 0) nomi_fmt(dist, sizeof(dist), "%d 米后", (int)(g_dist_m + 0.5));
        nomi_fmt(ctx, sizeof(ctx), "主人在用地图导航（%s）：%s%s，%s%s%s。", kModeNames[g_mode], dist,
                 action_name(g_action), g_instr, g_eta[0] ? "，预计 " : "", g_eta[0] ? g_eta : "");
    } else if (fresh() && g_speed >= 0) {
        nomi_fmt(ctx, sizeof(ctx), "主人开着地图（%s），没在导航：速度 %d 公里/小时%s%s。", kModeNames[g_mode],
                 (int)(g_speed + 0.5), g_heading >= 0 ? "，朝" : "", g_heading >= 0 ? dir_name(g_heading) : "");
    } else {
        nomi_copy_n(ctx, sizeof(ctx), "主人开着地图，还没收到手机的定位。", 0);
    }
    assistant_context(ctx);
}

// 在导航、或者定位一直在来（在走）：屏幕别睡；都没有就照常睡（省电）
static void update_keep_on(void)
{
    int want = g_canvas && g_connected && (g_nav || fresh());
    if (want == g_keep_on) return;
    g_keep_on = want;
    screen_keep_on(want);
}

static void draw(void)
{
    if (!g_canvas) return;
    update_keep_on();
    canvas_clear(g_canvas);
    if (!g_connected) draw_disconnected();
    else if (g_nav) draw_nav();
    else draw_free();
    canvas_show(g_canvas);
    update_context();
}

/* ---------- 菜单：换出行方式 ---------- */

static void show_menu(void)
{
    if (ui_page("地图") > 0) g_pages = 2;
    for (int i = 0; i < 3; i++) g_mode_rows[i] = ui_item(kModeNames[i], kModeSubs[i], i == g_mode ? "在用" : "");
    scan_packs();  // 刚往卡上放了新地图也认得
    g_center_lat_i = -1000000;
    if (g_fix) update_tiles();
    char s[160];
    if (g_pack_n == 0) {
        nomi_copy_n(s, sizeof(s), "卡上 maps/ 里没有离线地图：电脑上用 tools/map/make_map.py 做一份放进去", 0);
    } else {
        nomi_copy_n(s, sizeof(s), "离线地图：", 0);
        for (int i = 0; i < g_pack_n; i++) {
            const char *name = g_packs[i].path + 9;  // 去掉 "sd:/maps/"
            if (i) nomi_append(s, sizeof(s), "、");
            nomi_append(s, sizeof(s), name);
        }
    }
    ui_text(s, NOMI_TEXT_MUTED);
    ui_text("道路数据 © OpenStreetMap contributors", NOMI_TEXT_MUTED);
}

/* ---------- 入口 ---------- */

static void start_ui(void)
{
    if (ui_page("") > 0) g_pages = 1;
    g_canvas = ui_canvas(0);
    if (g_canvas < 0) {
        g_canvas = 0;
        return;
    }
    scan_packs();
    g_center_lat_i = -1000000;
    if (g_fix) update_tiles();
    g_connected = phone_listen("nav,gps") == 1;
    if (g_connected) ask_gps(1);
    timer_start(TIMER_TICK, 1000, 1);
    draw();
}

static void stop_ui(void)
{
    if (!g_canvas) return;
    if (g_connected) ask_gps(0);
    phone_listen("");
    if (g_keep_on) screen_keep_on(0);
    g_keep_on = 0;
    timer_stop(TIMER_TICK);
    g_canvas = 0;
    g_pages = 0;
    assistant_context("");
}

void nomi_main(void)
{
    g_mode = kv_get_int("mode", 0);
    if (g_mode < 0 || g_mode > 2) g_mode = 0;
}

int32_t nomi_event(int32_t type, int32_t id, int32_t value)
{
    switch (type) {
    case NOMI_EV_SHOW:
        start_ui();
        return 1;
    case NOMI_EV_HIDE:
    case NOMI_EV_CLOSE:
        stop_ui();
        return 1;
    case NOMI_EV_BACK:
        g_pages = value;
        draw();
        return 1;
    case NOMI_EV_TIMER:
        g_tick++;
        // 一直没收到定位：隔一阵再让手机推（手机那边刚打开分享定位、或者刚连上）
        if (g_connected && !fresh() && g_tick - g_asked_tick >= REASK_S) ask_gps(1);
        draw();
        return 1;
    case NOMI_EV_PHONE:
        if (value == NOMI_PHONE_CONNECTED) {
            g_connected = 1;
            ask_gps(1);
        } else if (value == NOMI_PHONE_DISCONNECTED) {
            g_connected = 0;
        } else {
            int32_t n;
            while ((n = phone_read(g_msg, sizeof(g_msg))) != 0) {
                if (n > 0) handle(g_msg, n);
            }
        }
        draw();
        return 1;
    case NOMI_EV_CLICK:
        if (id == g_canvas && g_pages == 1) {
            show_menu();
            return 1;
        }
        for (int i = 0; i < 3 && g_pages == 2; i++) {
            if (id != g_mode_rows[i]) continue;
            g_mode = i;
            kv_set_int("mode", i);
            ui_page_close();
            g_pages = 1;
            draw();
        }
        return 1;
    default:
        return 0;
    }
}
