#include "face.h"

#include <math.h>
#include <stddef.h>

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
    spring_t browu, browd, sweat, prop;  // 表情脚本用：眉毛、汗滴的淡入淡出，道具弹出来
} P;

static float s_bob, s_time, s_mode_time;
static float s_next_blink, s_blink_t, s_next_look, s_look_x, s_look_y;
static float s_level = -1;
static face_mode_t s_mode = FACE_IDLE;
static uint32_t s_rng;
static rrect_t s_prev;
static int s_first = 1;
static int s_sleep;
static float s_wake_t = -1;  // 醒来动画进行了多久，<0 表示没在播
static float s_fall_t = -1;  // 入睡动画进行了多久，<0 表示没在播（已经睡熟或醒着）
#define FALL_ASLEEP_S 2.2f   // 入睡动画时长，播完 Z 才开始冒

// 同一个状态里轮换几个动作，等得久也不单调。进入状态先放第 0 个（最能说明状态的那个）：
//   待机：0 随机张望（大部分时间）｜1 左右张望 2 原地蹦两下 3 歪一下头（偶尔插一个）
//   聆听：0 手放耳边｜1 边听边点头
//   思考：0 托腮看左上｜1 眼珠慢慢转一圈 2 抬头看头顶闪烁的三个点（在问大模型）3 挠头使劲想
static const int kVariants[FACE_MODE_COUNT] = {4, 2, 4, 1, 1, 1};
static int s_variant;
static float s_variant_time, s_variant_len;

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
    P.browu = spring(0, 160, 14);
    P.browd = spring(0, 160, 14);
    P.sweat = spring(0, 120, 14);
    P.prop = spring(0, 220, 11);  // 阻尼小一点，出现时会略微冲过头再回来
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
    // 一醒来就要听 / 想 / 说（比如睡着时直接按住 BOOT）：不伸懒腰，直接进这个动作，免得两套动作混在一起
    if (mode != FACE_IDLE) s_wake_t = -1;
    s_variant = 0;
    s_variant_time = 0;
    s_variant_len = mode == FACE_IDLE ? 6 + frand() * 6 : 2.4f;
}

// 换下一个动作：待机多数时间回到张望，偶尔插个短动作；其他状态随机换，不连着重复
static void next_variant(void)
{
    const int n = kVariants[s_mode];
    if (s_mode == FACE_IDLE) {
        s_variant = s_variant == 0 ? 1 + (int)(frand() * (n - 1)) : 0;
        s_variant_len = s_variant == 0 ? 6 + frand() * 6 : 1.4f;
    } else if (n > 1) {
        int next = (int)(frand() * (n - 1));
        s_variant = next >= s_variant ? next + 1 : next;
        s_variant_len = 2.6f + frand() * 1.2f;
    }
    s_variant_time = 0;
}

/* ---------- 表情脚本播放 ---------- */

static face_script_t s_script;
static int s_play = -1;       // 正在播第几步，-1 表示没在播
static float s_play_t;        // 这一步播了多久
static int s_play_hold;       // 演完了但话还没说完：停在最后一步（整脸动作停掉，其余保持）
#define PLAY_HOLD_MAX_S 20.0f
static int s_prop_step = -1;  // 画的是哪一步的道具（换掉或播完后淡出时还要接着画）
static float s_prop_t;        // 道具出现了多久，给道具动画用
static int s_eye_kind = FACE_EYES_CAPSULE, s_eye_next = FACE_EYES_CAPSULE;  // CAPSULE 表示常规画法（胶囊 / 弧线）
static float s_eye_switch = -1;  // 换眼型时"压扁再撑开"进行了多久，<0 表示没在换
#define EYE_SWITCH_S 0.18f

static void start_step(int i)
{
    const face_step_t *st = &s_script.steps[i];
    s_play = i;
    s_play_t = 0;
    s_play_hold = 0;
    if (!st->nstrokes) return;
    const face_step_t *cur = s_prop_step >= 0 ? &s_script.steps[s_prop_step] : NULL;
    // 换了道具才从小弹出来；同一个道具接着用，只换动作
    if (!cur || cur->stroke0 != st->stroke0 || cur->nstrokes != st->nstrokes || P.prop.x < 0.3f) {
        s_prop_t = 0;
        P.prop.x = P.prop.v = 0;
    }
    s_prop_step = i;
}

void face_play(const face_script_t *script)
{
    if (s_sleep || script == NULL || script->nsteps <= 0) return;
    s_script = *script;
    s_prop_step = -1;
    start_step(0);
}

void face_stop(void) { s_play = -1; }
int face_is_playing(void) { return s_play >= 0; }

face_mode_t face_get_mode(void) { return s_mode; }

const char *face_mode_name(face_mode_t mode)
{
    static const char *names[] = {"idle", "listen", "think", "speak", "happy", "confused"};
    return mode < FACE_MODE_COUNT ? names[mode] : "?";
}

void face_set_level(float level) { s_level = level; }

void face_set_sleep(int sleeping)
{
    if (s_sleep && !sleeping) s_wake_t = s_mode == FACE_IDLE ? 0 : -1;  // 醒来回到待机才伸懒腰
    if (!s_sleep && sleeping) s_fall_t = 0;  // 从醒着到睡着，先犯困再闭眼
    if (sleeping) s_wake_t = -1;
    else s_fall_t = -1;
    s_sleep = sleeping;
}

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

    // 表情脚本：到时间就换下一步，播完回到跟着状态走
    if (s_play >= 0) {
        s_play_t += dt;
        if (s_sleep) {
            s_play = -1;
        } else if (s_play_t >= s_script.steps[s_play].dur) {
            if (s_play + 1 < s_script.nsteps) {
                start_step(s_play + 1);
            } else if (m == FACE_SPEAK && s_play_t < s_script.steps[s_play].dur + PLAY_HOLD_MAX_S) {
                s_play_hold = 1;  // 话还没说完，别突然变回举麦克风
            } else {
                s_play = -1;
                s_play_hold = 0;
            }
        }
    }
    s_prop_t += dt;

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

    s_bob = sinf(s_time * 1.7f) * 1.5f - (m == FACE_LISTEN ? 2 : 0);
    if (m == FACE_HAPPY) s_bob -= fabsf(sinf(s_mode_time * 9)) * 7 * expf(-s_mode_time * 1.2f);

    // 当前动作对上面默认姿势的改动（动作列表见 kVariants）
    s_variant_time += dt;
    if (s_variant_time >= s_variant_len) next_variant();
    const int v = s_variant;
    const float vt = s_variant_time;
    if (m == FACE_THINK) {
        if (v == 1) {  // 眼珠慢慢转一圈，手放下
            P.open.t = blink ? 0.02f : 0.85f;
            gx = cosf(vt * 2.2f) * 0.8f;
            gy = sinf(vt * 2.2f) * 0.6f;
            P.tilt.t = 0.08f * sinf(vt * 1.1f);
            P.hr.t = 0;
        } else if (v == 2) {  // 抬头盯着头顶一闪一闪的三个点
            P.open.t = blink ? 0.02f : 0.95f;
            gx = 0;
            gy = -0.95f;
            P.tilt.t = 0;
            P.hr.t = 0;
        } else if (v == 3) {  // 挠头使劲想，眼睛眯起来
            P.open.t = blink ? 0.02f : 0.6f;
            gx = 0.5f;
            gy = -0.5f;
            P.tilt.t = 0.15f;
        }
    } else if (m == FACE_LISTEN && v == 1) {  // 边听边点头
        float nod = fmaxf(0, sinf(vt * 6.5f));
        s_bob += nod * 2.5f;
        gy = 0.2f * nod;
    } else if (m == FACE_IDLE && v == 1) {  // 左右张望
        gx = vt < 0.45f ? -1 : (vt < 0.95f ? 1 : 0);
        gy = 0;
    } else if (m == FACE_IDLE && v == 2) {  // 原地蹦两下
        if (vt < 0.9f) s_bob -= fabsf(sinf(vt * PI_F / 0.45f)) * 5;
    } else if (m == FACE_IDLE && v == 3) {  // 歪一下头
        P.tilt.t = vt < 1.0f ? 0.35f : 0;
    }

    // 表情脚本这一步写了的部分，盖过上面按状态算出来的
    const face_step_t *st = s_play >= 0 ? &s_script.steps[s_play] : NULL;
    int want_eyes = FACE_EYES_CAPSULE;
    if (st) {
        const float pt = s_play_t;
        switch (st->eyes) {
        case FACE_EYES_AUTO: break;
        case FACE_EYES_CAPSULE: P.smile.t = 0; break;
        case FACE_EYES_ARC: P.smile.t = 1; break;
        case FACE_EYES_CLOSED: P.smile.t = 0; P.open.t = 0.02f; break;
        default: P.smile.t = 0; want_eyes = st->eyes; break;
        }
        if (!isnan(st->look_x)) {
            gx = st->look_x;
            gy = st->look_y;
        }
        if (!isnan(st->tilt)) P.tilt.t = st->tilt;
        if (!isnan(st->blush)) P.blush.t = st->blush;
        // 表演期间手归脚本管：没写手势就收起来，免得说话的麦克风手和道具、手势抢
        switch (st->hands) {
        case FACE_HANDS_AUTO:
        case FACE_HANDS_NONE: P.hl.t = P.hr.t = 0; break;
        case FACE_HANDS_CHIN: case FACE_HANDS_EAR: case FACE_HANDS_WAVE: case FACE_HANDS_MIC:
            P.hl.t = 0;
            P.hr.t = 1;
            break;
        case FACE_HANDS_UP: case FACE_HANDS_COVER: case FACE_HANDS_CHEEKS: P.hl.t = P.hr.t = 1; break;
        default: break;
        }
        switch (s_play_hold ? FACE_MOTION_NONE : st->motion) {
        case FACE_MOTION_NOD: {
            float nod = fmaxf(0, sinf(pt * 7));
            s_bob += nod * 3;
            gy += 0.25f * nod;
            break;
        }
        case FACE_MOTION_BOUNCE: s_bob -= fabsf(sinf(pt * PI_F / 0.4f)) * 6; break;
        case FACE_MOTION_SHAKE:
            P.tilt.t = sinf(pt * 14) * 0.22f;
            gx = sinf(pt * 14) * 0.6f;
            break;
        case FACE_MOTION_SWAY: P.tilt.t = sinf(pt * 3.2f) * 0.32f; break;
        default: break;
        }
    }
    P.browu.t = st && st->brow == FACE_BROW_UP;
    P.browd.t = st && st->brow == FACE_BROW_DOWN;
    P.sweat.t = st && st->sweat;
    P.prop.t = st && st->nstrokes ? 1 : 0;
    if (P.prop.t == 0 && P.prop.x < 0.02f) s_prop_step = -1;
    // 换眼型：先压扁，压到最扁时换成新的，再撑开
    if (want_eyes != s_eye_next) {
        s_eye_next = want_eyes;
        if (s_eye_switch < 0) s_eye_switch = 0;
    }
    if (s_eye_switch >= 0) {
        s_eye_switch += dt;
        if (s_eye_switch >= EYE_SWITCH_S / 2) s_eye_kind = s_eye_next;
        if (s_eye_switch >= EYE_SWITCH_S) s_eye_switch = s_eye_kind != s_eye_next ? 0 : -1;
    }
    P.gx.t = gx;
    P.gy.t = gy;
    P.mouth.t = speech_level();

    if (s_sleep) {
        P.open.t = 0.02f;
        P.gx.t = P.gy.t = P.mouth.t = P.tilt.t = 0;
        P.hl.t = P.hr.t = P.smile.t = P.happy.t = P.blush.t = 0;
        P.wide.t = 1;
        s_bob = sinf(s_time * 0.8f) * 2.5f + 3;  // 慢呼吸，整张脸略微下沉
        if (s_fall_t >= 0) {
            // 入睡：眼皮耷拉、视线下垂 → 闭一下又撑开一条缝 → 彻底合上
            s_fall_t += dt;
            float t = s_fall_t;
            if (t < 0.9f) {
                P.open.t = 0.45f;
                P.gy.t = 0.35f;
            } else if (t < 1.15f) {
                P.open.t = 0.02f;
            } else if (t < 1.6f) {
                P.open.t = 0.25f;
                P.gy.t = 0.35f;
            } else if (t >= FALL_ASLEEP_S) {
                s_fall_t = -1;
            }
            s_bob = 3 * (t < FALL_ASLEEP_S ? t / FALL_ASLEEP_S : 1);  // 脸慢慢沉下去
        }
    } else if (s_wake_t >= 0) {
        // 醒来伸懒腰：眯开一条缝 → 用力眨一下 → 睁大眼、举起小手、抬脸晃一下 → 放下回到待机
        s_wake_t += dt;
        float t = s_wake_t;
        if (t < 0.35f) {
            P.open.t = 0.18f;
            s_bob = 3;
        } else if (t < 0.55f) {
            P.open.t = 0.02f;
        } else if (t < 1.4f) {
            P.open.t = 1;
            P.wide.t = 1.18f;
            P.hl.t = P.hr.t = 0.85f;
            P.tilt.t = 0.25f * sinf((t - 0.55f) * 7);
            P.gx.t = P.gy.t = 0;
            s_bob = -4 * sinf((t - 0.55f) / 0.85f * PI_F);
        } else if (t < 1.8f) {
            P.hl.t = P.hr.t = 0;
            P.wide.t = 1;
            P.tilt.t = 0;
        } else {
            s_wake_t = -1;
        }
    }

    spring_t *all[] = {&P.open, &P.gx,    &P.gy,    &P.mouth, &P.wide,  &P.tilt, &P.hl, &P.hr,
                       &P.smile, &P.happy, &P.blush, &P.browu, &P.browd, &P.sweat, &P.prop};
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) spring_step(all[i], dt);
}

// 整圈的圆环（raster_arc 起止角相同时什么都不画，所以分两半）
static void ring(raster_t *r, rxf_t m, float x, float y, float radius, float width, float alpha)
{
    raster_arc(r, m, x, y, radius, 0, PI_F, width, WHITE, alpha);
    raster_arc(r, m, x, y, radius, PI_F, PI_F * 1.999f, width, WHITE, alpha);
}

// 实心爱心：上面两个圆，下面一个转 45° 的方块
static void draw_heart(raster_t *r, rxf_t m, float k, float alpha)
{
    raster_ellipse(r, m, -4 * k, -2.5f * k, 4.7f * k, 4.7f * k, 0, WHITE, alpha);
    raster_ellipse(r, m, 4 * k, -2.5f * k, 4.7f * k, 4.7f * k, 0, WHITE, alpha);
    rxf_t d = rxf_rotate(rxf_translate(m, 0, 1.4f * k), PI_F / 4);
    raster_rrect(r, d, 0, 0, 9.6f * k, 9.6f * k, 1.5f * k, WHITE, alpha);
}

// 螺旋眼：从中心往外绕两圈，整体转着
static void draw_spiral(raster_t *r, rxf_t m, float rot)
{
    float px = 0, py = 0;
    for (int i = 1; i <= 22; i++) {
        float th = i * 0.55f, rr = th * 0.78f;
        float x = cosf(th + rot) * rr, y = sinf(th + rot) * rr;
        raster_line(r, m, px, py, x, y, 2.2f, WHITE, 1);
        px = x;
        py = y;
    }
}

// 把一步的道具线条画在 (x, y)：k 是 0～100 坐标到逻辑坐标的缩放
static void draw_strokes(raster_t *r, rxf_t base, const face_step_t *st, float x, float y, float rot, float k,
                         float alpha)
{
    rxf_t m = rxf_scale(rxf_rotate(rxf_translate(base, x, y), rot), k, k);
    const float lw = 2.0f / k;  // 线宽在缩放前的坐标里给，除回去让屏幕上一样细
    for (int i = 0; i < st->nstrokes; i++) {
        const face_stroke_t *sk = &s_script.strokes[st->stroke0 + i];
        const uint8_t(*p)[2] = &s_script.pts[sk->start];
        switch (sk->kind) {
        case FACE_STROKE_LINE:
            for (int j = 1; j < sk->count; j++) {
                raster_line(r, m, p[j - 1][0] - 50.0f, p[j - 1][1] - 50.0f, p[j][0] - 50.0f, p[j][1] - 50.0f, lw,
                            WHITE, alpha);
            }
            break;
        case FACE_STROKE_RING: ring(r, m, p[0][0] - 50.0f, p[0][1] - 50.0f, sk->r, lw, alpha); break;
        case FACE_STROKE_DOT: raster_ellipse(r, m, p[0][0] - 50.0f, p[0][1] - 50.0f, sk->r, sk->r, 0, WHITE, alpha); break;
        default: break;
        }
    }
}

// 道具：脚本里这一步的线条画，坐标 0～100 缩放到脸上对应的位置，再加上道具自己的小动作
static void draw_prop(raster_t *r, rxf_t base, float cx, float cy, float bob)
{
    const float a = P.prop.x;
    if (s_prop_step < 0 || a < 0.02f) return;
    const face_step_t *st = &s_script.steps[s_prop_step];
    // 相对脸中心的位置、边长；头顶那格要让开最上面的状态文字和眉毛
    static const float anchor[4][3] = {{0, 36, 40}, {-66, 22, 34}, {66, 22, 34}, {0, -45, 28}};
    const float *an = anchor[st->prop_at < 4 ? st->prop_at : 0];
    const float t = s_prop_t;
    float x = cx + an[0], y = cy + an[1] + bob * 0.6f, rot = 0, alpha = fminf(a, 1), sc = 1;
    switch (st->prop_anim) {
    case FACE_PROP_ANIM_BOB: y += sinf(t * 5) * 2.5f; break;
    case FACE_PROP_ANIM_SWING: rot = sinf(t * 4) * 0.35f; break;
    case FACE_PROP_ANIM_SPIN: rot = t * 3; break;
    case FACE_PROP_ANIM_FLOAT: {
        float ph = fmodf(t, 1.8f) / 1.8f;
        y -= ph * 16;
        alpha *= ph < 0.7f ? 1 : (1 - ph) / 0.3f;
        break;
    }
    case FACE_PROP_ANIM_POP: sc = a; break;  // 跟着弹簧略微冲过头
    case FACE_PROP_ANIM_ORBIT:
        // 三个缩小的道具在一个扁椭圆上绕圈：转到前面的大而亮，转到后面的小而暗
        for (int c = 0; c < 3; c++) {
            float ph = t * 2.4f + c * 2 * PI_F / 3, depth = 0.7f + 0.3f * sinf(ph);
            draw_strokes(r, base, st, x + cosf(ph) * 44, y + sinf(ph) * 8, 0, an[2] * 0.42f * depth * fminf(a, 1) / 100,
                         alpha * (0.45f + 0.55f * depth));
        }
        return;
    default: break;
    }
    if (sc < 0.05f || alpha < 0.02f) return;
    draw_strokes(r, base, st, x, y, rot, an[2] * sc / 100, alpha);
}

static void draw_hand(raster_t *r, rxf_t base, float x, float y, float rot, float flip)
{
    rxf_t m = rxf_scale(rxf_rotate(rxf_translate(base, x, y), rot), flip, 1);
    raster_ellipse(r, m, 0, 0, 11, 8, 0, WHITE, 1);
    raster_ellipse(r, m, -9, -6, 3.5f, 5, -0.6f, WHITE, 1);
    for (int i = 0; i < 3; i++) raster_ellipse(r, m, 8, -5 + i * 5, 4.5f, 2.6f, 0, WHITE, 1);
}

// 一个 Z：上横、斜线、下横三笔，和五官一样的白色圆头粗线
static void draw_z(raster_t *r, rxf_t base, float x, float y, float half, float alpha)
{
    const float w = half * 0.42f + 1.2f;  // 越大的 Z 笔画越粗
    raster_line(r, base, x - half, y - half, x + half, y - half, w, WHITE, alpha);
    raster_line(r, base, x + half, y - half, x - half, y + half, w, WHITE, alpha);
    raster_line(r, base, x - half, y + half, x + half, y + half, w, WHITE, alpha);
}

// 睡着时脸的右上方斜着排出 z z Z：由小到大依次冒出来，停一会儿一起淡出，循环
static void draw_zzz(raster_t *r, rxf_t base, float cx, float cy)
{
    static const float pos[3][3] = {{38, -14, 3.5f}, {54, -30, 5}, {75, -52, 7}};  // x, y, 半宽
    const float period = 2.6f, t = fmodf(s_time, period);
    const float fade_out = t < 1.9f ? 1 : (period - t) / (period - 1.9f);
    for (int i = 0; i < 3; i++) {
        float since = t - i * 0.35f;
        float fade_in = since <= 0 ? 0 : (since >= 0.3f ? 1 : since / 0.3f);
        float alpha = fade_in * fade_out;
        if (alpha < 0.02f) continue;
        float bob = 1.5f * sinf(s_time * 2.5f + i);  // 轻轻上下浮动
        draw_z(r, base, cx + pos[i][0], cy + pos[i][1] + bob, pos[i][2], alpha);
    }
}

static void draw_face(raster_t *r)
{
    const float cx = LW / 2, cy = LH / 2;
    const float bob = s_bob, wide = P.wide.x, op = P.open.x, hp = P.happy.x;
    const float gx = P.gx.x, gy = P.gy.x, mo = P.mouth.x, sm = P.smile.x;
    const face_mode_t m = s_mode;

    rxf_t base = rxf_translate(rxf_identity(), cx, cy);
    base = rxf_translate(rxf_rotate(base, P.tilt.x * 0.3f), -cx, -cy);

    // 眼睛：常规是竖胶囊，开心/说话时先压扁再长成 ∩ 弧线；表情脚本能换成别的眼型（换的时候先压扁再撑开）
    float sq = s_eye_switch < 0 ? 1 : fabsf(1 - 2 * s_eye_switch / EYE_SWITCH_S);
    if (sq < 0.08f) sq = 0.08f;
    float eye_x[2], eye_y[2];
    for (int s = -1; s <= 1; s += 2) {
        float x = cx + s * 30 + gx * 8, y = cy - 10 + bob + gy * 4 - mo * 3, w = 9 * wide;
        eye_x[(s + 1) / 2] = x;
        eye_y[(s + 1) / 2] = y;
        rxf_t em = rxf_scale(rxf_translate(base, x, y), 1, sq);
        switch (s_eye_kind) {
        case FACE_EYES_RING: {  // NOMI 的"0"：细线描边的竖向圆角框（底色是黑的，里面再填一块黑就成了描边）
            const float ow = 12 * wide, oh = 21 * wide, lw = 2.4f;
            raster_rrect(r, em, 0, 0, ow, oh, ow / 2, WHITE, 1);
            raster_rrect(r, em, 0, 0, ow - lw * 2, oh - lw * 2, ow / 2 - lw, BLACK, 1);
            break;
        }
        case FACE_EYES_SPIRAL: draw_spiral(r, em, s_time * 5 * s); break;
        case FACE_EYES_DASH: raster_line(r, em, -7, 0, 7, 0, 4, WHITE, 1); break;
        case FACE_EYES_DOT: raster_ellipse(r, em, 0, 0, 3.8f, 3.8f, 0, WHITE, 1); break;
        case FACE_EYES_HEART: draw_heart(r, em, 1.15f, 1); break;
        case FACE_EYES_STAR: raster_star(r, em, 0, 0, 11, WHITE, 1); break;
        case FACE_EYES_X:
            raster_line(r, em, -6, -6, 6, 6, 3.6f, WHITE, 1);
            raster_line(r, em, -6, 6, 6, -6, 3.6f, WHITE, 1);
            break;
        case FACE_EYES_SHADES:  // 墨镜：两片镜片，各带一道反光
            raster_rrect(r, em, 0, 0, 27, 17, 6, WHITE, 1);
            raster_line(r, em, -9, -3, -4, -6, 2, BLACK, 1);
            break;
        default:
            if (sm < 0.5f) {
                float h = 22 * wide * op * (1 - sm * 2);
                if (h < 3) h = 3;
                raster_rrect(r, em, 0, 0, w, h, (w < h ? w : h) / 2, WHITE, 1);
            } else {
                float rad = 9 * (sm - 0.5f) * 2 * (1 + mo * 0.25f);
                raster_arc(r, em, 0, 4, rad, PI_F * 1.1f, PI_F * 1.9f, 4, WHITE, 1);
            }
        }
    }
    if (s_eye_kind == FACE_EYES_SHADES) {
        raster_line(r, base, eye_x[0] + 13, eye_y[0] - 3, eye_x[1] - 13, eye_y[1] - 3, 3, WHITE, 1);
    }

    // 眉毛：上扬是眼睛上方一道弧，皱眉是往中间压下来的斜线
    for (int i = 0; i < 2; i++) {
        float s = i ? 1.0f : -1.0f, x = eye_x[i], y = eye_y[i];
        if (P.browu.x > 0.02f) raster_arc(r, base, x, y - 18, 8, PI_F * 1.25f, PI_F * 1.75f, 2.4f, WHITE, fminf(P.browu.x, 1));
        if (P.browd.x > 0.02f) raster_line(r, base, x + s * 8, y - 22, x - s * 6, y - 17, 2.4f, WHITE, fminf(P.browd.x, 1));
    }

    // 汗滴：右眼外上方，慢慢往下滑
    if (P.sweat.x > 0.02f) {
        float a = fminf(P.sweat.x, 1), x = cx + 50, y = cy - 32 + bob + fmodf(s_time, 2.2f) * 4;
        raster_ellipse(r, base, x, y + 3, 4, 4.6f, 0, WHITE, a);
        raster_line(r, base, x, y - 6, x - 3.2f, y + 1.5f, 2.6f, WHITE, a);
        raster_line(r, base, x, y - 6, x + 3.2f, y + 1.5f, 2.6f, WHITE, a);
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

    if (s_sleep && s_fall_t < 0) draw_zzz(r, base, cx, cy);  // 睡熟了才冒 Z

    // 思考动作 2：头顶三个点依次亮起，像在等大模型回话
    if (m == FACE_THINK && s_variant == 2 && !s_sleep) {
        const float appear = s_variant_time < 0.3f ? s_variant_time / 0.3f : 1;
        for (int i = 0; i < 3; i++) {
            float ph = fmodf(s_variant_time * 1.8f - i * 0.2f + 1, 1);
            float a = 0.3f + 0.7f * (ph < 0.5f ? ph * 2 : (1 - ph) * 2);
            float rad = 3 + 1.2f * a;
            raster_ellipse(r, base, cx - 14 + i * 14, cy - 50 + bob - a * 2, rad, rad, 0, WHITE, a * appear);
        }
    }

    draw_prop(r, base, cx, cy, bob);  // 道具在手后面，手能挡住它

    // 小手：从屏幕下沿滑进来。表情脚本指定了手势就按手势摆，否则按状态
    int pose = s_play >= 0 ? s_script.steps[s_play].hands : FACE_HANDS_AUTO;
    if (s_play >= 0 && pose == FACE_HANDS_AUTO) pose = FACE_HANDS_NONE;  // 表演期间没写手势 = 收起来
    for (int s = -1; s <= 1; s += 2) {
        float v = s < 0 ? P.hl.x : P.hr.x;
        if (v < 0.02f) continue;
        float x = cx + s * 44, y = 178 - v * 62, rot = 0;
        if (pose != FACE_HANDS_AUTO) {  // 收起来（NONE）时就从默认位置往下滑走
            switch (pose) {
            case FACE_HANDS_CHIN: if (s == 1) x = cx + 10; break;
            case FACE_HANDS_EAR:
                x = cx + 64;
                y = cy + 8 + (1 - v) * 90;
                rot = PI_F / 2 * 0.85f + sinf(s_time * 3) * 0.06f;
                break;
            case FACE_HANDS_WAVE:
                x = cx + 60;
                y = cy + 4 + (1 - v) * 100;
                rot = -0.5f + sinf(s_time * 9) * 0.45f;
                break;
            case FACE_HANDS_UP:  // 举起双手，轻轻晃
                x = cx + s * 64;
                y = cy - 6 + (1 - v) * 110;
                rot = -s * 0.3f + sinf(s_time * 6 + s) * 0.12f;
                break;
            case FACE_HANDS_COVER:  // 捂住眼睛
                x = cx + s * 30;
                y = cy - 8 + (1 - v) * 110;
                break;
            case FACE_HANDS_MIC:
                if (s == 1) {
                    x = cx + 30;
                    y -= mo * 3;
                    if (v > 0.3f) {
                        raster_line(r, base, x + 2, y - 17, x, y - 3, 4, WHITE, 1);
                        raster_ellipse(r, base, x + 2, y - 24, 6.5f, 6.5f, 0, WHITE, 1);
                        raster_ellipse(r, base, x + 2, y - 24, 2.2f, 2.2f, 0, BLACK, 1);
                    }
                }
                break;
            case FACE_HANDS_CHEEKS:  // 双手托腮
                x = cx + s * 42;
                y = cy + 16 + (1 - v) * 90;
                rot = s * 0.55f;
                break;
            default: break;
            }
            draw_hand(r, base, x, y, rot, (float)-s);
            continue;
        }
        if (m == FACE_THINK && s == 1) {
            if (s_variant == 3) {  // 挠头：手举到右眼外上方（头侧面的位置），来回蹭
                x = cx + 66;
                y = cy - 40 + (1 - v) * 120;
                rot = -0.7f + sinf(s_time * 16) * 0.22f;
            } else {
                x = cx + 10;  // 托腮
            }
        }
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
