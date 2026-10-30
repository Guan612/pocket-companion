#include "face.h"

#include <math.h>

#define PI_F 3.14159265f
#define LW 240.0f  // 逻辑宽
#define LH 160.0f  // 逻辑高
#define SCALE 2.0f

#define WHITE RGB565(255, 255, 255)
#define BLACK RGB565(8, 8, 10)
#define BLUSH RGB565(235, 235, 245)

typedef struct {
    float x, v, t, k, d;
} spring_t;

static struct {
    spring_t open, gx, gy, mouth, wide, tilt, hl, hr, smile, happy, blush;
} P;

static float s_bob, s_time, s_mode_time;
static float s_next_blink, s_blink_t, s_next_look, s_look_x, s_look_y;
static float s_level = -1;
static face_mode_t s_mode = FACE_IDLE;
static uint32_t s_rng;
static rrect_t s_prev;
static int s_first = 1;
static int s_sleep;

static spring_t spring(float x, float k, float d) { return (spring_t){x, 0, x, k, d}; }

static void spring_step(spring_t *s, float dt)
{
    s->v += (s->t - s->x) * s->k * dt;
    s->v *= expf(-s->d * dt);
    s->x += s->v * dt;
}

static float frand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (s_rng >> 8) / 16777216.0f;
}

void face_init(uint32_t seed)
{
    s_rng = seed ? seed : 0x9e3779b9u;
    P.open = spring(1, 260, 18);
    P.gx = spring(0, 90, 12);
    P.gy = spring(0, 90, 12);
    P.mouth = spring(0, 300, 28);
    P.wide = spring(1, 120, 12);
    P.tilt = spring(0, 100, 12);
    P.hl = spring(0, 120, 13);
    P.hr = spring(0, 120, 13);
    P.smile = spring(0, 140, 13);
    P.happy = spring(0, 140, 13);
    P.blush = spring(0, 80, 10);
    s_time = s_mode_time = 0;
    s_next_blink = 2;
    s_blink_t = -1;
    s_next_look = 1;
    s_first = 1;
}

void face_set_mode(face_mode_t mode)
{
    s_mode = mode;
    s_mode_time = 0;
}

face_mode_t face_get_mode(void) { return s_mode; }

const char *face_mode_name(face_mode_t mode)
{
    static const char *names[] = {"idle", "listen", "think", "speak", "happy", "confused"};
    return mode < FACE_MODE_COUNT ? names[mode] : "?";
}

void face_set_level(float level) { s_level = level; }

void face_set_sleep(int sleeping) { s_sleep = sleeping; }

static float speech_level(void)
{
    if (s_mode != FACE_SPEAK) return 0;
    if (s_level >= 0) return s_level > 1 ? 1 : s_level;
    if (sinf(s_time * 1.3f) <= -0.7f) return 0;  // 模拟句间停顿
    float v = 0.45f + 0.4f * sinf(s_time * 11) * sinf(s_time * 4.3f) + 0.15f * sinf(s_time * 23);
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

void face_step(float dt)
{
    s_time += dt;
    s_mode_time += dt;
    const face_mode_t m = s_mode;

    s_next_blink -= dt;
    if (s_next_blink < 0) {
        s_blink_t = 0;
        s_next_blink = 2 + frand() * 3.5f;
    }
    int blink = 0;
    if (s_blink_t >= 0) {
        s_blink_t += dt;
        blink = s_blink_t < 0.11f;
        if (s_blink_t > 0.22f) s_blink_t = -1;
    }

    P.open.t = blink ? 0.02f : (m == FACE_THINK ? 0.72f : 1);
    P.wide.t = m == FACE_LISTEN ? 1.15f : 1;
    P.happy.t = P.blush.t = m == FACE_HAPPY ? 1 : 0;
    P.smile.t = (m == FACE_HAPPY || m == FACE_SPEAK) ? 1 : 0;
    P.tilt.t = m == FACE_CONFUSED ? 0.3f * sinf(s_time * 2) : (m == FACE_THINK ? -0.12f : 0);
    P.hl.t = (m == FACE_SPEAK || m == FACE_HAPPY) ? 1 : 0;
    P.hr.t = m == FACE_IDLE ? 0 : 1;

    s_next_look -= dt;
    if (s_next_look < 0) {
        s_look_x = frand() * 2 - 1;
        s_look_y = (frand() * 2 - 1) * 0.6f;
        s_next_look = 1 + frand() * 2.2f;
    }
    float gx = 0, gy = 0;
    if (m == FACE_IDLE) {
        gx = s_look_x;
        gy = s_look_y;
    } else if (m == FACE_THINK) {
        gx = -0.7f;
        gy = -0.8f;
    } else if (m == FACE_SPEAK) {
        gx = sinf(s_time * 0.9f) * 0.25f;
    } else if (m == FACE_CONFUSED) {
        gx = sinf(s_time * 2.4f) * 0.8f;
    }
    P.gx.t = gx;
    P.gy.t = gy;
    P.mouth.t = speech_level();

    s_bob = sinf(s_time * 1.7f) * 1.5f - (m == FACE_LISTEN ? 2 : 0);
    if (m == FACE_HAPPY) s_bob -= fabsf(sinf(s_mode_time * 9)) * 7 * expf(-s_mode_time * 1.2f);

    if (s_sleep) {
        P.open.t = 0.02f;
        P.gx.t = P.gy.t = P.mouth.t = P.tilt.t = 0;
        P.hl.t = P.hr.t = P.smile.t = P.happy.t = P.blush.t = 0;
        P.wide.t = 1;
        s_bob = sinf(s_time * 0.8f) * 2.5f + 3;  // 慢呼吸，整张脸略微下沉
    }

    spring_t *all[] = {&P.open, &P.gx, &P.gy, &P.mouth, &P.wide, &P.tilt,
                       &P.hl,   &P.hr, &P.smile, &P.happy, &P.blush};
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) spring_step(all[i], dt);
}

static void draw_hand(raster_t *r, rxf_t base, float x, float y, float rot, float flip)
{
    rxf_t m = rxf_scale(rxf_rotate(rxf_translate(base, x, y), rot), flip, 1);
    raster_ellipse(r, m, 0, 0, 11, 8, 0, WHITE, 1);
    raster_ellipse(r, m, -9, -6, 3.5f, 5, -0.6f, WHITE, 1);
    for (int i = 0; i < 3; i++) raster_ellipse(r, m, 8, -5 + i * 5, 4.5f, 2.6f, 0, WHITE, 1);
}

static void draw_face(raster_t *r)
{
    const float cx = LW / 2, cy = LH / 2;
    const float bob = s_bob, wide = P.wide.x, op = P.open.x, hp = P.happy.x;
    const float gx = P.gx.x, gy = P.gy.x, mo = P.mouth.x, sm = P.smile.x;
    const face_mode_t m = s_mode;

    rxf_t base = rxf_translate(rxf_identity(), cx, cy);
    base = rxf_translate(rxf_rotate(base, P.tilt.x * 0.3f), -cx, -cy);

    // 眼睛：睁眼是竖胶囊；开心/说话时先压扁再长成 ∩ 弧线，两者不同时出现
    for (int s = -1; s <= 1; s += 2) {
        float x = cx + s * 30 + gx * 8, y = cy - 10 + bob + gy * 4 - mo * 3, w = 9 * wide;
        if (sm < 0.5f) {
            float h = 22 * wide * op * (1 - sm * 2);
            if (h < 3) h = 3;
            raster_rrect(r, base, x, y, w, h, (w < h ? w : h) / 2, WHITE, 1);
        } else {
            float rad = 9 * (sm - 0.5f) * 2 * (1 + mo * 0.25f);
            raster_arc(r, base, x, y + 4, rad, PI_F * 1.1f, PI_F * 1.9f, 4, WHITE, 1);
        }
    }

    if (P.blush.x > 0.01f) {
        for (int s = -1; s <= 1; s += 2)
            raster_glow(r, base, cx + s * 44, cy + 10, 11, BLUSH, 0.4f * P.blush.x);
    }

    if (hp > 0.05f) {
        static const float stars[3][4] = {{40, -28, 6, 0}, {-42, -20, 4.5f, 1.7f}, {30, -8, 3, 3.1f}};
        for (int i = 0; i < 3; i++) {
            float a = hp * (0.55f + 0.45f * sinf(s_time * 5 + stars[i][3]));
            raster_star(r, base, cx + stars[i][0], cy + stars[i][1] + bob, stars[i][2], WHITE, a);
        }
    }

    // 小手：从屏幕下沿滑进来
    for (int s = -1; s <= 1; s += 2) {
        float v = s < 0 ? P.hl.x : P.hr.x;
        if (v < 0.02f) continue;
        float x = cx + s * 44, y = 178 - v * 62, rot = 0;
        if (m == FACE_THINK && s == 1) x = cx + 10;
        if (m == FACE_LISTEN) {
            x = cx + 64;
            y = cy + 8 + (1 - v) * 90;
            rot = PI_F / 2 * 0.85f + sinf(s_time * 3) * 0.06f;
        }
        if (m == FACE_SPEAK) {
            if (s == 1) {
                x = cx + 30;
                y -= mo * 3;
                if (v > 0.3f) {  // 麦克风
                    raster_line(r, base, x + 2, y - 17, x, y - 3, 4, WHITE, 1);
                    raster_ellipse(r, base, x + 2, y - 24, 6.5f, 6.5f, 0, WHITE, 1);
                    raster_ellipse(r, base, x + 2, y - 24, 2.2f, 2.2f, 0, BLACK, 1);
                }
            } else {
                y -= mo * 10;
                rot = -mo * 0.35f;
            }
        }
        if (m == FACE_HAPPY) rot = sinf(s_time * 10 + s) * 0.4f;
        if (m == FACE_CONFUSED && s == 1) rot = sinf(s_time * 8) * 0.3f;
        draw_hand(r, base, x, y, rot, (float)-s);
    }
}

rrect_t face_render(uint16_t *fb)
{
    raster_t r;
    raster_begin(&r, fb, FACE_W, FACE_H, SCALE);
    const rrect_t full = {0, 0, FACE_W, FACE_H};

    // 底色是纯黑，所以每帧只需擦掉上一帧画过的区域
    raster_clear(&r, s_first ? full : s_prev, BLACK);
    draw_face(&r);

    rrect_t out = s_first ? full : rrect_union(s_prev, r.dirty);
    s_prev = r.dirty;
    s_first = 0;
    return out;
}
