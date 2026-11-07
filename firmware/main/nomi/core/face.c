#include "face.h"

#include <math.h>
#include <stddef.h>

#define PI_F 3.14159265f
#define LW 240.0f  // 逻辑宽
#define LH 160.0f  // 逻辑高
#define SCALE 2.0f

// 眼睛的球面模型：两只眼睛在半径 EYE_R 的球面上，静止时各偏离正前方 EYE_AZ0 弧度，正好是左右各 30（EYE_R * sin(EYE_AZ0)）。
// 视线 -1～1 对应头转的角度：左右最多 EYE_YAW_MAX 弧度，上下最多 EYE_PITCH_MAX 弧度（上下只带来一点点压扁）。
// 以后要调"转头幅度"改 EYE_YAW_MAX；改 EYE_AZ0 时要同时改 EYE_R 保持眼距
#define EYE_AZ0 0.5f
#define EYE_R 62.57f  // 30 / sin(0.5)
#define EYE_YAW_MAX 0.28f
#define EYE_PITCH_MAX 0.35f

#define WHITE RGB565(255, 255, 255)
#define BLACK RGB565(8, 8, 10)
#define BLUSH RGB565(235, 235, 245)

typedef struct {
    float x, v, t, k, d;
} spring_t;

static struct {
    spring_t open, gx, gy, mouth, wide, tilt, hl, hr, smile, happy, blush;
    spring_t browu, browd, sweat, prop;  // 表情脚本用：眉毛、汗滴的淡入淡出，道具弹出来
    spring_t sq;  // 挤压拉伸：0 是原样，正的拉长变窄，负的压扁变宽
    spring_t dx;  // 整张脸左右躲闪的偏移（被戳时往反方向缩一下再弹回来）
} P;

static float s_bob, s_time, s_mode_time;
// 挤压拉伸不用每个动作单独写：看 s_bob（脸上下的位置）动得多快、速度变得多急。
// 动得快就顺着拉长；往下动突然变成往上（落地、点头到底）就压扁一下再弹回来
static float s_bob_prev, s_bob_v;
static int s_bob_fresh = 1;  // 刚初始化：上一帧的位置还没有，这一帧不算速度
// 比这还快（逻辑像素 / 秒）就是瞬移，不是在动：换状态、入睡醒来时 s_bob 会直接跳几个像素，不算撞了一下。
// 现有动作最快的是开心刚进来连蹦（约 63），表情脚本的 bounce 约 47
#define BOB_V_MAX 90.0f
#define SQ_KICK 0.09f        // 速度每变 1 像素 / 秒，挤压拉伸弹簧挨多大一下
#define SQ_STRETCH 0.0016f   // 每 1 像素 / 秒的速度拉长多少
#define SQ_STRETCH_MAX 0.12f
#define SQ_LIMIT 0.3f  // 画的时候最多压扁 / 拉长到这个程度
// 被戳（face_poke）：第一下吓一跳；连着戳两三下觉得痒、笑；戳个没完就不高兴
static float s_poke_t = -1;     // 这次反应演了多久，<0 表示没在演
static float s_poke_since = 99;  // 离上次被戳多久，用来数是不是连着戳
static float s_poke_x, s_poke_y;
static int s_poke_n;  // 连着戳了第几下
#define POKE_STREAK_S 1.6f  // 这么久之内再戳算连着戳
static float s_next_blink, s_blink_t, s_next_look, s_look_x, s_look_y;
// 待机的眼神有目标：大部分时间看着你（正前方），偶尔瞟开一下再看回来，不是满屏随便挑点看
static int s_look_away;  // 1 = 正瞟着别处
// 盯着一个地方时眼睛也不是死的：每零点几秒挪一点点（注视微动）
static float s_next_micro, s_micro_x, s_micro_y;
static float s_breath_ph;    // 呼吸的相位：快慢带随机起伏，不是固定频率
static float s_variant_rate = 1;  // 这一次动作的快慢（0.85～1.15），同一个动作每次演都不完全一样
// 对话前后的过渡（face_set_mode 里起头）：
//   ATTEND 被叫到（待机 → 听 / 想）：「嗯？」愣一下、眨眼、眉毛一挑、眼睛看过来、往上一提，小手晚一点才放到耳边
//   AFTER  说完了（对话 → 待机）：不马上自己玩，看着你停一会儿、点个头；刚才在说话 / 开心就保持笑眼
enum { TRANS_NONE, TRANS_ATTEND, TRANS_AFTER };
static int s_trans, s_trans_smile;
static float s_trans_t = -1;
#define ATTEND_S 0.6f
#define AFTER_S 1.7f
static float s_blink_since = 10;  // 距离上一次眨眼开始多久，给"顺带眨一下"限频用
static int s_blink_again, s_blink_second;  // 这一下眨完要不要紧接着再眨一下 / 现在正是紧接着的那一下
static float s_last_gx, s_last_gy;         // 上一帧的目标视线，用来发现视线突然跳开
static float s_touch_x, s_touch_y, s_touch_age = -1;  // 手指指的方向；age < 0 表示没在跟
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
//   待机：0 看着你、偶尔瞟开（大部分时间）｜1 左右张望 2 原地蹦两下 3 歪一下头 4 偷笑一下（偶尔插一个）
//   聆听：0 手放耳边｜1 边听边点头 2 歪着头认真听
//   思考：0 托腮看左上｜1 眼珠慢慢转一圈 2 抬头看头顶闪烁的三个点（在问大模型）3 挠头使劲想
//   说话：0 平常讲｜1 跟着话的起伏点头 2 歪头晃着讲 3 眼睛往上看、像在想词
//   开心：0 刚开心时连蹦几下｜1 左右摇摆 2 再蹦几下
//   疑惑：0 歪头左右看｜1 慢慢摇头 2 眼神游移、冒一滴汗
// 抽下一个动作用"洗牌袋"：一轮里每个动作各出现一次再重新洗牌，不会连着重复，也不会老是抽到同几个
static const int kVariants[FACE_MODE_COUNT] = {5, 3, 4, 4, 3, 3};
#define VARIANT_MAX 8
static int s_variant;
static float s_variant_time, s_variant_len;
typedef struct {
    uint8_t order[VARIANT_MAX];
    uint8_t n, pos;  // 这一袋有 n 个，已经抽了 pos 个
    int8_t last;     // 最近抽出（或演过）的一个
} bag_t;
static bag_t s_bags[FACE_MODE_COUNT];  // 每个状态一袋，离开再回来接着抽

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

static float hash1(int i)
{
    uint32_t h = (uint32_t)i * 0x9E3779B1u;
    h ^= h >> 15;
    h *= 0x85EBCA77u;
    h ^= h >> 13;
    return (h & 0xFFFF) / 32767.5f - 1;
}

// 平滑的随机起伏（一维梯度噪声），大约 -1～1，x 每走 1 换一个拐点。拿来代替固定频率的正弦：
// 正弦看几遍就知道是在循环，这个不重复。不动 s_rng，不影响别的随机数
static float noise1(float x)
{
    const float fl = floorf(x), f = x - fl;
    const int i = (int)fl;
    const float a = hash1(i) * f, b = hash1(i + 1) * (f - 1), u = f * f * (3 - 2 * f);
    return (a + (b - a) * u) * 2;
}

// 请求眨一下眼，prob 是概率。正在眨、或离上一次眨眼太近（不到 0.9 秒）就算了，免得视线乱跳时眨个不停
static void request_blink(float prob)
{
    if (s_sleep || s_blink_t >= 0 || s_blink_since < 0.9f || frand() >= prob) return;
    s_blink_t = 0;
    s_blink_since = 0;
    s_next_blink = 2.2f + frand() * 3.5f;  // 刚眨过，定时的那个重新数
}

// 从这个状态的洗牌袋里抽下一个动作。待机的 0 是平常张望，穿插在特殊动作之间，不进袋子
static int bag_pick(face_mode_t m)
{
    bag_t *b = &s_bags[m];
    const int lo = m == FACE_IDLE ? 1 : 0, cnt = kVariants[m] - lo;
    if (cnt <= 0) return 0;
    if (b->pos >= b->n) {  // 一轮抽完了，重新洗牌
        b->n = (uint8_t)cnt;
        b->pos = 0;
        for (int i = 0; i < cnt; i++) b->order[i] = (uint8_t)(lo + i);
        for (int i = cnt - 1; i > 0; i--) {
            const int j = (int)(frand() * (i + 1));
            const uint8_t t = b->order[i];
            b->order[i] = b->order[j];
            b->order[j] = t;
        }
        // 新一轮的第一个不能和上一轮最后演的相同
        if (cnt > 1 && b->order[0] == b->last) {
            const int j = 1 + (int)(frand() * (cnt - 1));
            const uint8_t t = b->order[0];
            b->order[0] = b->order[j];
            b->order[j] = t;
        }
    }
    b->last = (int8_t)b->order[b->pos++];
    return b->last;
}

void face_init(uint32_t seed)
{
    s_rng = seed ? seed : 0x9e3779b9u;
    for (int i = 0; i < FACE_MODE_COUNT; i++) s_bags[i] = (bag_t){.last = -1};
    s_blink_since = 10;
    s_blink_again = s_blink_second = 0;
    s_last_gx = s_last_gy = 0;
    s_touch_age = -1;
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
    P.sq = spring(0, 260, 18);    // 欠阻尼：压扁后会回弹一点点再停
    P.dx = spring(0, 200, 13);
    s_bob_fresh = 1;
    s_bob_v = 0;
    s_poke_t = -1;
    s_poke_since = 99;
    s_poke_n = 0;
    s_time = s_mode_time = 0;
    s_next_blink = 2;
    s_blink_t = -1;
    s_next_look = 1;
    s_look_away = 0;
    s_look_x = s_look_y = 0;
    s_next_micro = 0;
    s_micro_x = s_micro_y = 0;
    s_breath_ph = 0;
    s_variant_rate = 1;
    s_trans = TRANS_NONE;
    s_trans_t = -1;
    s_first = 1;
}

void face_set_mode(face_mode_t mode)
{
    const face_mode_t prev = s_mode;
    s_mode = mode;
    s_mode_time = 0;
    s_variant_rate = 0.85f + frand() * 0.3f;
    // 对话前后的过渡：被叫到先「嗯？」一下；说完看着你停一会儿。睡着时不演（醒来另有伸懒腰）
    s_trans_t = -1;
    if (!s_sleep && prev == FACE_IDLE && (mode == FACE_LISTEN || mode == FACE_THINK)) {
        s_trans = TRANS_ATTEND;
        s_trans_t = 0;
    } else if (!s_sleep && prev != FACE_IDLE && mode == FACE_IDLE) {
        s_trans = TRANS_AFTER;
        s_trans_t = 0;
        s_trans_smile = prev == FACE_SPEAK || prev == FACE_HAPPY;
        // 演完接着看你一会儿再开始瞟
        s_look_away = 0;
        s_look_x = s_look_y = 0;
        s_next_look = AFTER_S + 1.5f + frand() * 2.5f;
    }
    // 一醒来就要听 / 想 / 说（比如睡着时直接按住 BOOT）：不伸懒腰，直接进这个动作，免得两套动作混在一起
    if (mode != FACE_IDLE) s_wake_t = -1;
    s_variant = 0;
    s_variant_time = 0;
    s_variant_len = mode == FACE_IDLE ? 6 + frand() * 6 : 2.4f;
    if (mode != FACE_IDLE) {
        // 进来先演了第 0 个：袋里接着抽的不能马上又是它
        bag_t *b = &s_bags[mode];
        b->last = 0;
        if (b->pos < b->n && b->order[b->pos] == 0) {
            if (b->n - b->pos > 1) {
                const uint8_t t = b->order[b->pos];
                b->order[b->pos] = b->order[b->n - 1];
                b->order[b->n - 1] = t;
            } else {
                b->pos = b->n;  // 袋里只剩它，丢掉，下回重新洗牌
            }
        }
    }
    request_blink(0.4f);  // 换了状态，常顺带眨一下
}

// 换下一个动作：待机多数时间回到张望，偶尔插个短动作；其他状态从洗牌袋里抽
static void next_variant(void)
{
    if (s_mode == FACE_IDLE) {
        if (s_variant == 0) {
            s_variant = bag_pick(s_mode);
            s_variant_len = s_variant == 4 ? 1.9f : 1.4f;
        } else {
            s_variant = 0;
            s_variant_len = 6 + frand() * 6;
        }
    } else if (kVariants[s_mode] > 1) {
        s_variant = bag_pick(s_mode);
        s_variant_len = 2.6f + frand() * 1.2f;
    }
    s_variant_rate = 0.85f + frand() * 0.3f;
    s_variant_time = 0;
    request_blink(0.35f);  // 换了动作，常顺带眨一下
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

// 手指一直按着时，触摸事件每 30 毫秒来一次；万一松手事件丢了（比如触摸被别的控件抢走），超时也会松开
#define TOUCH_LOOK_TIMEOUT_S 1.2f
#define TOUCH_LOOK_LINGER_S 0.45f  // 手指离开后，眼睛还盯着那里多久

void face_look_at(float x, float y)
{
    s_touch_x = x < -1 ? -1 : (x > 1 ? 1 : x);
    s_touch_y = y < -1 ? -1 : (y > 1 ? 1 : y);
    s_touch_age = 0;
}

void face_look_release(void)
{
    if (s_touch_age >= 0 && s_touch_age < TOUCH_LOOK_TIMEOUT_S - TOUCH_LOOK_LINGER_S)
        s_touch_age = TOUCH_LOOK_TIMEOUT_S - TOUCH_LOOK_LINGER_S;
}

void face_poke(float x, float y)
{
    if (s_sleep || s_wake_t >= 0) return;  // 睡着、正在醒都不理（点屏幕叫醒它那一下不算戳）
    x = x < -1 ? -1 : (x > 1 ? 1 : x);
    y = y < -1 ? -1 : (y > 1 ? 1 : y);
    s_poke_n = s_poke_since < POKE_STREAK_S ? s_poke_n + 1 : 1;
    if (s_poke_n > 9) s_poke_n = 9;
    s_poke_since = 0;
    s_poke_t = 0;
    s_poke_x = x;
    s_poke_y = y;
    P.sq.v -= s_poke_n == 1 ? 4.5f : 2.5f;  // 被戳得一扁
    // 戳在一侧：往反方向躲一下（戳在正中间就只是往后缩）
    if (fabsf(x) > 0.15f) P.dx.v -= (x > 0 ? 1 : -1) * (s_poke_n == 1 ? 70 : 40);
}

void face_set_sleep(int sleeping)
{
    if (s_sleep && !sleeping) s_wake_t = s_mode == FACE_IDLE ? 0 : -1;  // 醒来回到待机才伸懒腰
    if (!s_sleep && sleeping) s_fall_t = 0;  // 从醒着到睡着，先犯困再闭眼
    if (sleeping) s_poke_t = s_trans_t = -1;
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

    // 眨眼：定时的兜底（2～5.5 秒一次，偶尔连眨两下）；视线突然跳开、换动作、换状态时另外会顺带眨一下
    s_blink_since += dt;
    s_next_blink -= dt;
    if (s_next_blink < 0) {
        s_blink_t = 0;
        s_blink_since = 0;
        s_next_blink = 2 + frand() * 3.5f;
        if (s_blink_second) s_blink_second = s_blink_again = 0;  // 这就是连眨的第二下，不再接第三下
        else s_blink_again = frand() < 0.15f;
    }
    int blink = 0;
    if (s_blink_t >= 0) {
        s_blink_t += dt;
        blink = s_blink_t < 0.11f;
        if (s_blink_t > 0.22f) {
            s_blink_t = -1;
            if (s_blink_again) {
                s_blink_again = 0;
                s_blink_second = 1;
                s_next_blink = 0.08f;  // 隔一小会儿再眨一下
            }
        }
    }
    if (s_touch_age >= 0 && (s_touch_age += dt) > TOUCH_LOOK_TIMEOUT_S) s_touch_age = -1;

    P.open.t = blink ? 0.02f : (m == FACE_THINK ? 0.72f : 1);
    P.wide.t = m == FACE_LISTEN ? 1.15f : 1;
    P.happy.t = P.blush.t = m == FACE_HAPPY ? 1 : 0;
    P.smile.t = (m == FACE_HAPPY || m == FACE_SPEAK) ? 1 : 0;
    // 头一直有一点点不规则的晃（几乎看不出来，但整张脸不再是死的）；疑惑时歪头左右看，停顿也不规则
    const float sway = 0.08f * noise1(s_time * 0.35f + 3.1f);
    P.tilt.t = m == FACE_CONFUSED ? 0.32f * noise1(s_time * 0.9f) : (m == FACE_THINK ? -0.12f : 0) + sway;
    P.hl.t = (m == FACE_SPEAK || m == FACE_HAPPY) ? 1 : 0;
    P.hr.t = m == FACE_IDLE ? 0 : 1;

    // 待机眼神：看着你 2～6 秒 → 瞟开 0.5～1.5 秒（三成会接着再瞟另一处）→ 看回来，看你的时候也不是正正的死盯
    s_next_look -= dt;
    if (s_next_look < 0) {
        if (!s_look_away || frand() < 0.3f) {
            const float side = frand() < 0.5f ? -1.0f : 1.0f;
            s_look_away = 1;
            s_look_x = side * (0.45f + frand() * 0.55f);
            s_look_y = (frand() * 2 - 1) * 0.6f;
            s_next_look = 0.5f + frand() * 1.0f;
        } else {
            s_look_away = 0;
            s_look_x = (frand() * 2 - 1) * 0.12f;
            s_look_y = (frand() * 2 - 1) * 0.1f;
            s_next_look = 2 + frand() * 4;
        }
    }
    // 注视微动：每 0.25～0.7 秒挪一点点，弹簧会把它抹成很轻的漂移
    s_next_micro -= dt;
    if (s_next_micro < 0) {
        s_micro_x = (frand() * 2 - 1) * 0.05f;
        s_micro_y = (frand() * 2 - 1) * 0.1f;
        s_next_micro = 0.25f + frand() * 0.45f;
    }
    float gx = 0, gy = 0;
    if (m == FACE_IDLE) {
        gx = s_look_x;
        gy = s_look_y;
    } else if (m == FACE_THINK) {
        gx = -0.7f + 0.12f * noise1(s_time * 0.5f);
        gy = -0.8f + 0.08f * noise1(s_time * 0.4f + 7);
    } else if (m == FACE_SPEAK) {  // 边说边看你，眼神不规则地飘一点
        gx = 0.3f * noise1(s_time * 0.45f + 11);
        gy = 0.15f * noise1(s_time * 0.35f + 13);
    } else if (m == FACE_CONFUSED) {  // 左看看右看看，在两边各停一会儿
        gx = 0.8f * tanhf(2.5f * noise1(s_time * 0.8f + 5));
    }
    gx += s_micro_x;
    gy += s_micro_y;

    // 呼吸：快慢、深浅都带一点随机起伏
    s_breath_ph += dt * 1.7f * (1 + 0.25f * noise1(s_time * 0.15f + 21));
    s_bob = sinf(s_breath_ph) * 1.5f * (1 + 0.3f * noise1(s_time * 0.11f + 17)) - (m == FACE_LISTEN ? 2 : 0);
    if (m == FACE_HAPPY) s_bob -= fabsf(sinf(s_mode_time * 9)) * 7 * expf(-s_mode_time * 1.2f);

    // 当前动作对上面默认姿势的改动（动作列表见 kVariants）。同一个动作每次快慢略有不同（s_variant_rate）
    s_variant_time += dt;
    if (s_variant_time >= s_variant_len) next_variant();
    const int v = s_variant;
    const float vt = s_variant_time * s_variant_rate;
    int want_sweat = 0;
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
    } else if (m == FACE_LISTEN && v == 2) {  // 歪着头认真听，眼睛睁得更大
        P.tilt.t = 0.4f;
        P.wide.t = 1.2f;
        gx = 0.35f;
        gy = -0.15f;
    } else if (m == FACE_IDLE && v == 3) {  // 歪一下头
        P.tilt.t = vt < 1.0f ? 0.35f : 0;
    } else if (m == FACE_IDLE && v == 4) {  // 偷笑一下：眼睛弯成弧线，脸微微泛红，眼神往下躲
        const int on = vt < 1.5f;
        P.smile.t = on;
        P.blush.t = on ? 0.7f : 0;
        P.tilt.t = on ? 0.12f : 0;
        gx = 0;
        gy = 0.25f;
    } else if (m == FACE_SPEAK && v == 1) {  // 跟着话的起伏点头
        const float nod = fmaxf(0, sinf(vt * 5.5f)) * (0.35f + P.mouth.x);
        s_bob += nod * 2.2f;
        gy = 0.2f * nod;
    } else if (m == FACE_SPEAK && v == 2) {  // 歪头晃着讲
        P.tilt.t = 0.25f * sinf(vt * 1.6f);
        gx = 0.3f + sinf(s_time * 0.9f) * 0.2f;
    } else if (m == FACE_SPEAK && v == 3) {  // 眼睛往上看，像在想怎么说
        P.open.t = blink ? 0.02f : 0.92f;
        gx = 0.5f;
        gy = -0.7f;
    } else if (m == FACE_HAPPY && v == 1) {  // 开心地左右摇摆
        P.tilt.t = 0.35f * sinf(vt * 4.5f);
        s_bob -= fabsf(sinf(vt * 4.5f)) * 2;
    } else if (m == FACE_HAPPY && v == 2) {  // 再蹦几下，越蹦越小
        s_bob -= fabsf(sinf(vt * 8.5f)) * 6 * expf(-vt * 0.9f);
    } else if (m == FACE_CONFUSED && v == 1) {  // 慢慢摇头
        gx = sinf(vt * 4.2f) * 0.9f;
        P.tilt.t = 0.12f * sinf(vt * 4.2f + 1.5f);
    } else if (m == FACE_CONFUSED && v == 2) {  // 眼神游移，冒一滴汗
        static const float dart[4][2] = {{-0.9f, -0.2f}, {0.8f, 0.3f}, {-0.3f, 0.7f}, {0.9f, -0.4f}};
        const int k = (int)(vt * 2.4f) % 4;
        gx = dart[k][0];
        gy = dart[k][1];
        want_sweat = 1;
    }

    // 对话前后的过渡（见 TRANS_*）：盖过上面的状态和动作；手指、表情脚本、被戳还是在它上面
    int brow_up = 0;
    if (s_trans_t >= 0) {
        s_trans_t += dt;
        const float t = s_trans_t;
        if (s_trans == TRANS_ATTEND) {
            P.open.t = t < 0.1f ? 0.02f : (blink ? 0.02f : 1);  // 先眨一下
            P.wide.t = t < 0.1f ? 1 : 1.2f;                      // 再睁大
            gx = s_micro_x;  // 看过来（正前方 = 看着你）
            gy = -0.05f + s_micro_y;
            brow_up = t < 0.5f;
            if (t < 0.32f) P.hl.t = P.hr.t = 0;  // 手晚一点才上来
            if (t >= 0.08f && t < 0.38f) s_bob -= 3 * sinf((t - 0.08f) / 0.3f * PI_F);  // 往上一提（会自动拉长）
            if (t >= ATTEND_S) s_trans_t = -1;
        } else {
            gx = s_micro_x;  // 说完了，看着你
            gy = 0.05f + s_micro_y;
            if (s_trans_smile && t < 1.0f) P.smile.t = 1;
            if (t >= 0.35f && t < 0.8f) {  // 点个头
                const float nod = sinf((t - 0.35f) / 0.45f * PI_F);
                s_bob += 2.5f * nod;
                gy += 0.2f * nod;
            }
            if (t >= AFTER_S) s_trans_t = -1;
        }
    }

    // 手指按在屏幕上：眼睛看向手指（表情脚本明确写了视线的话，下面还是脚本说了算）
    if (s_touch_age >= 0 && !s_sleep) {
        gx = s_touch_x;
        gy = s_touch_y;
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
    P.browu.t = (st && st->brow == FACE_BROW_UP) || brow_up;
    P.browd.t = st && st->brow == FACE_BROW_DOWN;
    P.sweat.t = (st && st->sweat) || want_sweat;
    P.prop.t = st && st->nstrokes ? 1 : 0;
    if (P.prop.t == 0 && P.prop.x < 0.02f) s_prop_step = -1;

    // 被戳了（face_poke）：盖过状态和表情脚本，演完回去。被戳那一下的压扁、躲闪在 face_poke 里已经弹出去了
    s_poke_since += dt;
    if (s_poke_t >= 0) {
        s_poke_t += dt;
        const float t = s_poke_t;
        if (s_poke_n == 1) {  // 吓一跳：眼睛闭紧、被戳扁（压扁时五官自己会往下沉），然后睁大眼看是谁戳的
            P.smile.t = 0;
            if (t < 0.16f) {
                P.open.t = 0.02f;
            } else {
                P.open.t = blink ? 0.02f : 1;
                P.wide.t = 1.14f;
                gx = s_poke_x;
                gy = s_poke_y;
            }
            if (t > 0.9f) s_poke_t = -1;
        } else if (s_poke_n <= 3) {  // 痒：眯眼笑、脸红、扭一扭、小蹦一下
            P.smile.t = 1;
            P.blush.t = 0.7f;
            P.tilt.t = 0.2f * sinf(t * 16) * expf(-t * 2.5f);
            gx = 0;
            gy = 0.3f;
            if (t < 0.6f) s_bob -= fabsf(sinf(t * PI_F / 0.3f)) * 3;
            if (t > 1.3f) s_poke_t = -1;
        } else {  // 戳个没完：皱眉、眯眼，扭头不看你
            const float away = s_poke_x > 0 ? -1.0f : 1.0f;
            P.smile.t = 0;
            P.browd.t = 1;
            P.open.t = blink ? 0.02f : 0.6f;
            P.tilt.t = -away * 0.15f;
            gx = away * 0.9f;
            gy = 0.1f;
            if (t > 1.8f) s_poke_t = -1;
        }
    }
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
    // 视线突然跳开（左右张望、换了新的看点、手指刚按下来……）时顺带眨一下，像真人扫视时那样
    if (fabsf(gx - s_last_gx) + fabsf(gy - s_last_gy) * 1.5f > 0.7f) request_blink(0.35f);
    s_last_gx = gx;
    s_last_gy = gy;
    // 手指跟着走时弹簧硬一点，眼睛跟得上
    const int following = s_touch_age >= 0 && !s_sleep;
    P.gx.k = P.gy.k = following ? 170 : 90;
    P.gx.d = P.gy.d = following ? 16 : 12;
    P.gx.t = gx;
    P.gy.t = gy;
    P.mouth.t = speech_level();

    float stretch_extra = 0;  // 不靠速度、动作自己要的拉长（伸懒腰）
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
        // 醒来伸懒腰：眯开一条缝 → 用力眨一下 → 睁大眼、举起小手、抬脸晃一下（整张脸往上拉长） → 放下回到待机
        s_wake_t += dt;
        float t = s_wake_t;
        if (t >= 0.55f && t < 1.4f) stretch_extra = 0.12f * sinf((t - 0.55f) / 0.85f * PI_F);
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

    // 挤压拉伸：见 s_bob_prev 的说明。往上的加速度（往下动被顶住、落地）压扁，动得快顺着拉长
    float vb = s_bob_fresh ? 0 : (s_bob - s_bob_prev) / dt;
    s_bob_fresh = 0;
    s_bob_prev = s_bob;
    if (fabsf(vb) > BOB_V_MAX) {
        vb = s_bob_v;  // 瞬移：当没发生，速度接着用上一帧的
    } else {
        P.sq.v += (vb - s_bob_v) * SQ_KICK;
        s_bob_v = vb;
    }
    P.sq.t = fminf(fabsf(vb) * SQ_STRETCH, SQ_STRETCH_MAX) + stretch_extra;

    spring_t *all[] = {&P.open,  &P.gx,    &P.gy,    &P.mouth, &P.wide,  &P.tilt, &P.hl, &P.hr, &P.smile,
                       &P.happy, &P.blush, &P.browu, &P.browd, &P.sweat, &P.prop, &P.sq, &P.dx};
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

    rxf_t base = rxf_translate(rxf_identity(), cx + P.dx.x, cy);  // dx：被戳时整张脸往旁边躲
    base = rxf_translate(rxf_rotate(base, P.tilt.x * 0.3f), -cx, -cy);

    // 挤压拉伸：体积不变，拉长就变窄、压扁就变宽；脸像立在地上，压扁时五官往下沉、两眼分开一点
    const float stretch = P.sq.x < -SQ_LIMIT ? -SQ_LIMIT : (P.sq.x > SQ_LIMIT ? SQ_LIMIT : P.sq.x);
    const float body_y = 1 + stretch, body_x = 1 / body_y;

    // 眼睛：常规是竖胶囊，开心/说话时先压扁再长成 ∩ 弧线；表情脚本能换成别的眼型（换的时候先压扁再撑开）
    float sq = s_eye_switch < 0 ? 1 : fabsf(1 - 2 * s_eye_switch / EYE_SWITCH_S);
    if (sq < 0.08f) sq = 0.08f;
    // 视线不是把两只眼睛一起平移：两只眼睛长在一个球面上，视线是头转的方向（见 EYE_* 的说明）。
    // 转向一侧时，那一侧的眼睛被"转到侧面"变窄、挪得少，另一侧的变宽、挪得多，两眼也靠近一点
    const float yaw = gx * EYE_YAW_MAX, pitch_squash = cosf(gy * EYE_PITCH_MAX);
    float eye_x[2], eye_y[2], eye_fore[2];
    for (int s = -1; s <= 1; s += 2) {
        const float az = s * EYE_AZ0 + yaw;
        const float fore = cosf(az) / cosf(EYE_AZ0);  // 相对静止时的正对程度：1 不变，<1 被转到侧面
        float x = cx + EYE_R * sinf(az) * (1 + (body_x - 1) * 0.5f);
        float y = cy - 10 + bob + gy * 4 - mo * 3 - stretch * 20, w = 9 * wide;
        eye_x[(s + 1) / 2] = x;
        eye_y[(s + 1) / 2] = y;
        eye_fore[(s + 1) / 2] = fore;
        rxf_t em = rxf_scale(rxf_translate(base, x, y), fore * body_x, sq * pitch_squash * body_y);
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
        raster_line(r, base, eye_x[0] + 13.5f * eye_fore[0], eye_y[0] - 3, eye_x[1] - 13.5f * eye_fore[1], eye_y[1] - 3, 3,
                    WHITE, 1);
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
