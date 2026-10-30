#include "raster.h"

#include <math.h>
#include <stddef.h>

#define TWO_PI 6.28318531f

typedef float (*sdf_fn)(const float *p, float x, float y);

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline float maxf(float a, float b) { return a > b ? a : b; }
static inline float minf(float a, float b) { return a < b ? a : b; }

void raster_begin(raster_t *r, uint16_t *fb, int w, int h, float scale)
{
    r->fb = fb;
    r->w = w;
    r->h = h;
    r->scale = scale;
    r->dirty = (rrect_t){0, 0, 0, 0};
}

rxf_t rxf_identity(void) { return (rxf_t){1, 0, 0, 1, 0, 0}; }

static rxf_t rxf_mul(rxf_t m, rxf_t n)
{
    return (rxf_t){
        m.a * n.a + m.c * n.b,       m.b * n.a + m.d * n.b,
        m.a * n.c + m.c * n.d,       m.b * n.c + m.d * n.d,
        m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f,
    };
}

rxf_t rxf_translate(rxf_t m, float x, float y) { return rxf_mul(m, (rxf_t){1, 0, 0, 1, x, y}); }

rxf_t rxf_rotate(rxf_t m, float rad)
{
    float c = cosf(rad), s = sinf(rad);
    return rxf_mul(m, (rxf_t){c, s, -s, c, 0, 0});
}

rxf_t rxf_scale(rxf_t m, float sx, float sy) { return rxf_mul(m, (rxf_t){sx, 0, 0, sy, 0, 0}); }

int rrect_empty(rrect_t a) { return a.x1 <= a.x0 || a.y1 <= a.y0; }

rrect_t rrect_union(rrect_t a, rrect_t b)
{
    if (rrect_empty(a)) return b;
    if (rrect_empty(b)) return a;
    return (rrect_t){
        a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0,
        a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1,
    };
}

static inline uint16_t blend565(uint16_t dst, uint16_t src, float a)
{
    int ia = (int)(a * 32.0f + 0.5f);
    if (ia >= 32) return src;
    if (ia <= 0) return dst;
    int na = 32 - ia;
    int r = ((src >> 11) * ia + (dst >> 11) * na) >> 5;
    int g = (((src >> 5) & 63) * ia + ((dst >> 5) & 63) * na) >> 5;
    int b = ((src & 31) * ia + (dst & 31) * na) >> 5;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void raster_clear(raster_t *r, rrect_t area, uint16_t color)
{
    if (area.x0 < 0) area.x0 = 0;
    if (area.y0 < 0) area.y0 = 0;
    if (area.x1 > r->w) area.x1 = r->w;
    if (area.y1 > r->h) area.y1 = r->h;
    for (int y = area.y0; y < area.y1; y++) {
        uint16_t *row = r->fb + (size_t)y * r->w;
        for (int x = area.x0; x < area.x1; x++) row[x] = color;
    }
}

// glow_radius > 0 时按光斑处理：透明度 = 1 - 距中心距离 / 半径
static void fill(raster_t *r, rxf_t m, float lcx, float lcy, float reach, sdf_fn fn,
                 const float *p, uint16_t color, float alpha, float glow_radius)
{
    if (alpha <= 0.004f) return;
    const float s = r->scale;
    float wcx = m.a * lcx + m.c * lcy + m.e;
    float wcy = m.b * lcx + m.d * lcy + m.f;
    int x0 = (int)floorf((wcx - reach) * s) - 1, x1 = (int)ceilf((wcx + reach) * s) + 1;
    int y0 = (int)floorf((wcy - reach) * s) - 1, y1 = (int)ceilf((wcy + reach) * s) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > r->w) x1 = r->w;
    if (y1 > r->h) y1 = r->h;
    if (x1 <= x0 || y1 <= y0) return;

    for (int py = y0; py < y1; py++) {
        uint16_t *row = r->fb + (size_t)py * r->w;
        float dy = (py + 0.5f) / s - m.f;
        for (int px = x0; px < x1; px++) {
            float dx = (px + 0.5f) / s - m.e;
            float lx = m.a * dx + m.b * dy;
            float ly = m.c * dx + m.d * dy;
            float sd = fn(p, lx, ly);
            float cov = glow_radius > 0 ? clampf(-sd / glow_radius, 0, 1)
                                        : clampf(0.5f - sd * s, 0, 1);
            if (cov > 0) row[px] = blend565(row[px], color, cov * alpha);
        }
    }
    r->dirty = rrect_union(r->dirty, (rrect_t){x0, y0, x1, y1});
}

static float sdf_rrect(const float *p, float x, float y)
{
    float qx = fabsf(x - p[0]) - (p[2] - p[4]);
    float qy = fabsf(y - p[1]) - (p[3] - p[4]);
    float ox = maxf(qx, 0), oy = maxf(qy, 0);
    return sqrtf(ox * ox + oy * oy) + minf(maxf(qx, qy), 0) - p[4];
}

void raster_rrect(raster_t *r, rxf_t m, float cx, float cy, float w, float h, float radius,
                  uint16_t color, float alpha)
{
    float hw = w / 2, hh = h / 2;
    radius = clampf(radius, 0, minf(hw, hh));
    float p[5] = {cx, cy, hw, hh, radius};
    fill(r, m, cx, cy, sqrtf(hw * hw + hh * hh), sdf_rrect, p, color, alpha, 0);
}

static float sdf_ellipse(const float *p, float x, float y)
{
    float dx = x - p[0], dy = y - p[1];
    float ux = p[4] * dx + p[5] * dy;
    float uy = -p[5] * dx + p[4] * dy;
    float rx = p[2], ry = p[3];
    float k2 = (ux * ux) / (rx * rx) + (uy * uy) / (ry * ry);
    float gx = ux / (rx * rx), gy = uy / (ry * ry);
    float g = sqrtf(gx * gx + gy * gy);
    if (g < 1e-6f) return -minf(rx, ry);
    return (k2 - 1) / (2 * g);
}

void raster_ellipse(raster_t *r, rxf_t m, float cx, float cy, float rx, float ry, float rot,
                    uint16_t color, float alpha)
{
    if (rx <= 0.05f || ry <= 0.05f) return;
    float p[6] = {cx, cy, rx, ry, cosf(rot), sinf(rot)};
    fill(r, m, cx, cy, maxf(rx, ry), sdf_ellipse, p, color, alpha, 0);
}

static float wrap_angle(float a)
{
    a = fmodf(a, TWO_PI);
    return a < 0 ? a + TWO_PI : a;
}

static float sdf_arc(const float *p, float x, float y)
{
    float dx = x - p[0], dy = y - p[1];
    float ang = wrap_angle(atan2f(dy, dx));
    float a0 = p[3], a1 = p[4];
    int inside = a0 <= a1 ? (ang >= a0 && ang <= a1) : (ang >= a0 || ang <= a1);
    if (inside) return fabsf(sqrtf(dx * dx + dy * dy) - p[2]) - p[5];
    float e0x = p[2] * cosf(a0) - dx, e0y = p[2] * sinf(a0) - dy;
    float e1x = p[2] * cosf(a1) - dx, e1y = p[2] * sinf(a1) - dy;
    return sqrtf(minf(e0x * e0x + e0y * e0y, e1x * e1x + e1y * e1y)) - p[5];
}

// 角度和 canvas 一致：y 轴向下，顺时针为正
void raster_arc(raster_t *r, rxf_t m, float cx, float cy, float radius, float a0, float a1,
                float width, uint16_t color, float alpha)
{
    if (radius <= 0.05f) return;
    float p[6] = {cx, cy, radius, wrap_angle(a0), wrap_angle(a1), width / 2};
    fill(r, m, cx, cy, radius + width, sdf_arc, p, color, alpha, 0);
}

static float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float len2 = vx * vx + vy * vy;
    float t = len2 > 0 ? clampf((wx * vx + wy * vy) / len2, 0, 1) : 0;
    float ex = wx - vx * t, ey = wy - vy * t;
    return sqrtf(ex * ex + ey * ey);
}

static float sdf_line(const float *p, float x, float y)
{
    return seg_dist(x, y, p[0], p[1], p[2], p[3]) - p[4];
}

void raster_line(raster_t *r, rxf_t m, float x0, float y0, float x1, float y1, float width,
                 uint16_t color, float alpha)
{
    float p[5] = {x0, y0, x1, y1, width / 2};
    float hx = (x1 - x0) / 2, hy = (y1 - y0) / 2;
    fill(r, m, x0 + hx, y0 + hy, sqrtf(hx * hx + hy * hy) + width, sdf_line, p, color, alpha, 0);
}

// 四角星：8 个顶点，长短半径交替
static float sdf_star(const float *p, float x, float y)
{
    float vx[8], vy[8];
    for (int i = 0; i < 8; i++) {
        float a = i * (TWO_PI / 8) - TWO_PI / 4;
        float rr = (i & 1) ? p[2] * 0.28f : p[2];
        vx[i] = p[0] + cosf(a) * rr;
        vy[i] = p[1] + sinf(a) * rr;
    }
    float d = 1e9f;
    int inside = 0;
    for (int i = 0, j = 7; i < 8; j = i++) {
        d = minf(d, seg_dist(x, y, vx[j], vy[j], vx[i], vy[i]));
        if ((vy[i] > y) != (vy[j] > y) &&
            x < (vx[j] - vx[i]) * (y - vy[i]) / (vy[j] - vy[i]) + vx[i]) {
            inside = !inside;
        }
    }
    return inside ? -d : d;
}

void raster_star(raster_t *r, rxf_t m, float cx, float cy, float radius, uint16_t color,
                 float alpha)
{
    float p[3] = {cx, cy, radius};
    fill(r, m, cx, cy, radius, sdf_star, p, color, alpha, 0);
}

static float sdf_circle(const float *p, float x, float y)
{
    float dx = x - p[0], dy = y - p[1];
    return sqrtf(dx * dx + dy * dy) - p[2];
}

void raster_glow(raster_t *r, rxf_t m, float cx, float cy, float radius, uint16_t color,
                 float alpha)
{
    float p[3] = {cx, cy, radius};
    fill(r, m, cx, cy, radius, sdf_circle, p, color, alpha, radius);
}
