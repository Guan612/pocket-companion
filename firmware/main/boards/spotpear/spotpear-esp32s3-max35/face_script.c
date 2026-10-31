#include "face_script.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"

// 内置图标：和大模型自己画的道具同一种线条格式（坐标 0～100），解析时一样转成笔画
static const struct {
    const char *name;
    const char *strokes;
} kIcons[] = {
    {"note", "[[60,18,60,70],[60,18,80,28,80,40],[50,72,-10]]"},
    {"music", "[[38,30,38,72],[70,22,70,64],[38,30,70,22],[38,38,70,30],[30,74,-9],[62,66,-9]]"},
    {"heart", "[[50,82,22,56,16,40,22,27,36,22,50,34,64,22,78,27,84,40,78,56,50,82]]"},
    {"star", "[[50,10,61,38,90,40,67,58,75,88,50,71,25,88,33,58,10,40,39,38,50,10]]"},
    {"cup", "[[22,44,74,44],[24,44,30,82,66,82,72,44],[82,58,9],[40,34,44,24,40,14],[56,34,60,24,56,14]]"},
    {"mic", "[[50,30,14],[50,44,50,86],[36,86,64,86]]"},
    {"z", "[[30,30,70,30,30,70,70,70]]"},
    {"question", "[[32,32,36,20,50,14,64,20,68,32,62,42,50,50,50,62],[50,80,-5]]"},
    {"exclaim", "[[50,14,50,62],[50,80,-5]]"},
    {"lightning", "[[58,10,34,52,52,52,42,90,70,42,52,42,58,10]]"},
    {"leaf", "[[20,80,28,42,58,20,86,14,80,46,58,70,20,80],[20,80,62,38]]"},
    {"sun", "[[50,50,16],[50,8,50,20],[50,80,50,92],[8,50,20,50],[80,50,92,50],[20,20,29,29],[71,71,80,80],[20,80,29,71],[71,29,80,20]]"},
    {"cloud", "[[20,70,15,58,24,47,36,48,42,35,58,31,70,42,72,50,82,52,87,62,80,70,20,70]]"},
    {"drop", "[[50,14,34,46,32,58,38,70,50,76,62,70,68,58,66,46,50,14]]"},
    {"sparkle", "[[50,8,57,43,92,50,57,57,50,92,43,57,8,50,43,43,50,8]]"},  // 四角星 ✦
    {"flower", "[[50,50,8],[50,28,11],[50,72,11],[28,50,11],[72,50,11]]"},
};
#define ICON_COUNT ((int)(sizeof(kIcons) / sizeof(kIcons[0])))

const char *face_script_icon_names(void)
{
    static char names[256];
    if (!names[0]) {
        for (int i = 0; i < ICON_COUNT; i++) {
            if (i) strncat(names, ",", sizeof(names) - strlen(names) - 1);
            strncat(names, kIcons[i].name, sizeof(names) - strlen(names) - 1);
        }
    }
    return names;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static float num(const cJSON *item, float def)
{
    return cJSON_IsNumber(item) ? (float)item->valuedouble : def;
}

// 名字在表里的下标 + base，找不到返回 fallback
static int pick(const cJSON *item, const char *const *names, int n, int base, int fallback)
{
    if (!cJSON_IsString(item)) return fallback;
    for (int i = 0; i < n; i++) {
        if (strcmp(item->valuestring, names[i]) == 0) return base + i;
    }
    return fallback;
}

// 解析一组笔画追加进 out，返回加了几笔
static int parse_strokes(const cJSON *arr, face_script_t *out)
{
    int added = 0;
    const cJSON *s = NULL;
    cJSON_ArrayForEach(s, arr) {
        if (!cJSON_IsArray(s) || out->nstrokes >= FACE_SCRIPT_MAX_STROKES) continue;
        int n = cJSON_GetArraySize(s);
        face_stroke_t *k = &out->strokes[out->nstrokes];
        if (n == 3) {  // 圆：[x, y, r]，r 为负画实心点
            if (out->npts >= FACE_SCRIPT_MAX_POINTS) break;
            float rr = num(cJSON_GetArrayItem(s, 2), 0);
            k->kind = rr < 0 ? FACE_STROKE_DOT : FACE_STROKE_RING;
            k->r = (uint8_t)clampf(fabsf(rr), 1, 50);
            k->start = (uint8_t)out->npts;
            k->count = 1;
            out->pts[out->npts][0] = (uint8_t)clampf(num(cJSON_GetArrayItem(s, 0), 50), 0, 100);
            out->pts[out->npts][1] = (uint8_t)clampf(num(cJSON_GetArrayItem(s, 1), 50), 0, 100);
            out->npts++;
        } else if (n >= 4) {  // 折线：[x1, y1, x2, y2, …]
            int pts = n / 2;
            if (pts > FACE_SCRIPT_MAX_POINTS - out->npts) pts = FACE_SCRIPT_MAX_POINTS - out->npts;
            if (pts < 2) break;
            k->kind = FACE_STROKE_LINE;
            k->start = (uint8_t)out->npts;
            k->count = (uint8_t)pts;
            for (int j = 0; j < pts; j++) {
                out->pts[out->npts][0] = (uint8_t)clampf(num(cJSON_GetArrayItem(s, j * 2), 50), 0, 100);
                out->pts[out->npts][1] = (uint8_t)clampf(num(cJSON_GetArrayItem(s, j * 2 + 1), 50), 0, 100);
                out->npts++;
            }
        } else {
            continue;
        }
        out->nstrokes++;
        added++;
    }
    return added;
}

static int fail(char *err, size_t err_size, const char *msg)
{
    if (err && err_size) snprintf(err, err_size, "%s", msg);
    return 0;
}

int face_script_parse(const char *json, face_script_t *out, char *err, size_t err_size)
{
    static const char *const kEyes[] = {"capsule", "arc", "ring", "dash", "dot", "closed", "heart", "star", "x", "shades", "spiral"};
    static const char *const kHands[] = {"none", "chin", "ear", "wave", "up", "cover", "mic", "cheeks"};
    static const char *const kMotion[] = {"nod", "bounce", "shake", "sway"};
    static const char *const kBrow[] = {"up", "down"};
    static const char *const kAnim[] = {"bob", "swing", "spin", "float", "pop", "orbit"};
    static const char *const kAt[] = {"mouth", "left", "right", "top"};

    memset(out, 0, sizeof(*out));
    if (err && err_size) err[0] = 0;
    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (root == NULL) return fail(err, err_size, "表情脚本不是合法的 JSON");

    // 接受 [步骤…]、{"steps":[…]} 或单个步骤对象
    const cJSON *steps = root;
    cJSON *single = NULL;
    if (cJSON_IsObject(root)) {
        const cJSON *s = cJSON_GetObjectItem(root, "steps");
        if (cJSON_IsArray(s)) {
            steps = s;
        } else {
            single = cJSON_CreateArray();
            cJSON_AddItemReferenceToArray(single, root);
            steps = single;
        }
    }
    if (!cJSON_IsArray(steps)) {
        cJSON_Delete(single);
        cJSON_Delete(root);
        return fail(err, err_size, "表情脚本应该是步骤数组");
    }

    int icon_stroke0[ICON_COUNT], icon_nstrokes[ICON_COUNT];  // 同一个图标在几步里重复用时共享笔画，不会每步重新弹出
    memset(icon_nstrokes, 0, sizeof(icon_nstrokes));
    float total = 0;
    const cJSON *it = NULL;
    cJSON_ArrayForEach(it, steps) {
        if (out->nsteps >= FACE_SCRIPT_MAX_STEPS || !cJSON_IsObject(it)) continue;
        face_step_t *st = &out->steps[out->nsteps];
        st->look_x = st->look_y = st->tilt = st->blush = NAN;

        float dur = clampf(num(cJSON_GetObjectItem(it, "dur"), 1.5f), 0.3f, 4.0f);
        if (total + dur > FACE_SCRIPT_MAX_SECONDS) dur = FACE_SCRIPT_MAX_SECONDS - total;
        if (dur < 0.3f) break;
        st->dur = dur;

        st->eyes = (uint8_t)pick(cJSON_GetObjectItem(it, "eyes"), kEyes, 11, FACE_EYES_CAPSULE, FACE_EYES_AUTO);
        st->hands = (uint8_t)pick(cJSON_GetObjectItem(it, "hands"), kHands, 8, FACE_HANDS_NONE, FACE_HANDS_AUTO);
        st->motion = (uint8_t)pick(cJSON_GetObjectItem(it, "motion"), kMotion, 4, FACE_MOTION_NOD, FACE_MOTION_NONE);
        st->brow = (uint8_t)pick(cJSON_GetObjectItem(it, "brow"), kBrow, 2, FACE_BROW_UP, FACE_BROW_NONE);
        st->sweat = cJSON_IsTrue(cJSON_GetObjectItem(it, "sweat"));

        const cJSON *look = cJSON_GetObjectItem(it, "look");
        if (cJSON_IsArray(look) && cJSON_GetArraySize(look) >= 2) {
            st->look_x = clampf(num(cJSON_GetArrayItem(look, 0), 0), -1, 1);
            st->look_y = clampf(num(cJSON_GetArrayItem(look, 1), 0), -1, 1);
        }
        const cJSON *tilt = cJSON_GetObjectItem(it, "tilt");
        if (cJSON_IsNumber(tilt)) st->tilt = clampf((float)tilt->valuedouble, -1, 1);
        const cJSON *blush = cJSON_GetObjectItem(it, "blush");
        if (cJSON_IsNumber(blush)) st->blush = clampf((float)blush->valuedouble, 0, 1);
        else if (cJSON_IsBool(blush)) st->blush = cJSON_IsTrue(blush) ? 0.8f : 0;

        const cJSON *prop = cJSON_GetObjectItem(it, "prop");
        if (cJSON_IsObject(prop)) {
            st->prop_anim = (uint8_t)pick(cJSON_GetObjectItem(prop, "anim"), kAnim, 6, FACE_PROP_ANIM_BOB, FACE_PROP_ANIM_NONE);
            st->prop_at = (uint8_t)pick(cJSON_GetObjectItem(prop, "at"), kAt, 4, FACE_PROP_AT_MOUTH, FACE_PROP_AT_MOUTH);
            const cJSON *icon = cJSON_GetObjectItem(prop, "icon");
            const cJSON *strokes = cJSON_GetObjectItem(prop, "strokes");
            int idx = -1;
            for (int i = 0; cJSON_IsString(icon) && i < ICON_COUNT; i++) {
                if (strcmp(icon->valuestring, kIcons[i].name) == 0) idx = i;
            }
            if (idx >= 0 && icon_nstrokes[idx]) {
                st->stroke0 = (uint8_t)icon_stroke0[idx];
                st->nstrokes = (uint8_t)icon_nstrokes[idx];
            } else {
                st->stroke0 = (uint8_t)out->nstrokes;
                if (idx >= 0) {
                    cJSON *def = cJSON_Parse(kIcons[idx].strokes);
                    st->nstrokes = (uint8_t)parse_strokes(def, out);
                    cJSON_Delete(def);
                    icon_stroke0[idx] = st->stroke0;
                    icon_nstrokes[idx] = st->nstrokes;
                } else if (cJSON_IsArray(strokes)) {
                    st->nstrokes = (uint8_t)parse_strokes(strokes, out);
                }
            }
        }
        total += dur;
        out->nsteps++;
    }
    cJSON_Delete(single);
    cJSON_Delete(root);
    if (out->nsteps == 0) return fail(err, err_size, "表情脚本里没有能播的步骤");
    return 1;
}
