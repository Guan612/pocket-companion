#include "nomi_image.h"

#include "miniz.h"  // ROM 里的 tinfl（PNG 的 zlib 解压）
#include "tjpgd.h"  // esp_jpeg 组件带的 TJpgDec R0.03（sdkconfig：不用 ROM 版、开查表解 Huffman）

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>

#define TAG "NomiImage"

namespace {

constexpr size_t kWork = 16 * 1024;     // TJpgDec 的工作区（查表解 Huffman 要 10KB 左右），放内部 RAM 快
constexpr size_t kMaxExif = 64 * 1024;  // APP1 段最长（EXIF 小图在里面）
constexpr uint32_t kMaxPngSide = 16384;

uint16_t Rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// 源图（缩着解出来的 sw × sh，EXIF 方向 orient）→ 转正后 ow × oh → 缩到 dw × dh、放在目标的 (dx, dy)
struct Mapper {
    uint16_t* dst = nullptr;
    int tw = 0, th = 0;
    int sw = 0, sh = 0, orient = 1;
    int ow = 0, oh = 0, dw = 0, dh = 0, dx = 0, dy = 0;

    bool Inverse(int tx, int ty, int& sx, int& sy) const {
        if (tx < dx || ty < dy) return false;
        int ox = static_cast<int>(static_cast<int64_t>(tx - dx) * ow / dw);
        int oy = static_cast<int>(static_cast<int64_t>(ty - dy) * oh / dh);
        if (ox >= ow || oy >= oh) return false;
        switch (orient) {
        case 3: sx = sw - 1 - ox; sy = sh - 1 - oy; break;
        case 6: sx = oy; sy = sh - 1 - ox; break;
        case 8: sx = sw - 1 - oy; sy = ox; break;
        default: sx = ox; sy = oy; break;
        }
        return true;
    }
    void Forward(int sx, int sy, int& tx, int& ty) const {
        int ox, oy;
        switch (orient) {
        case 3: ox = sw - 1 - sx; oy = sh - 1 - sy; break;
        case 6: ox = sh - 1 - sy; oy = sx; break;
        case 8: ox = sy; oy = sw - 1 - sx; break;
        default: ox = sx; oy = sy; break;
        }
        tx = dx + static_cast<int>(static_cast<int64_t>(ox) * dw / ow);
        ty = dy + static_cast<int>(static_cast<int64_t>(oy) * dh / oh);
    }
    // 源图里 [x0, x1] × [y0, y1] 这块落到目标的哪个范围（多算一圈，逐个像素再用 Inverse 核对）
    void Box(int x0, int y0, int x1, int y1, int& a0, int& b0, int& a1, int& b1) const {
        a0 = tw, b0 = th, a1 = -1, b1 = -1;
        const int xs[2] = {x0, x1 + 1}, ys[2] = {y0, y1 + 1};
        for (int x : xs) {
            for (int y : ys) {
                int tx, ty;
                Forward(x, y, tx, ty);
                a0 = std::min(a0, tx), a1 = std::max(a1, tx), b0 = std::min(b0, ty), b1 = std::max(b1, ty);
            }
        }
        a0 = std::max(0, a0 - 1), b0 = std::max(0, b0 - 1), a1 = std::min(tw - 1, a1 + 1), b1 = std::min(th - 1, b1 + 1);
    }
};

// 原图（转正后）多大、目标多大 → 画多大、放哪
void Place(Mapper& m, int full_ow, int full_oh, NomiImage::Fit fit) {
    bool wider = static_cast<int64_t>(full_ow) * m.th > static_cast<int64_t>(full_oh) * m.tw;
    if ((fit == NomiImage::kContain) == wider) {  // 宽度顶满
        m.dw = m.tw;
        m.dh = static_cast<int>(static_cast<int64_t>(full_oh) * m.tw / full_ow);
    } else {
        m.dh = m.th;
        m.dw = static_cast<int>(static_cast<int64_t>(full_ow) * m.th / full_oh);
    }
    m.dw = std::max(1, m.dw);
    m.dh = std::max(1, m.dh);
    m.dx = (m.tw - m.dw) / 2;
    m.dy = (m.th - m.dh) / 2;
}

struct Ctx {
    FILE* f = nullptr;
    const uint8_t* mem = nullptr;
    size_t mem_size = 0, mem_pos = 0;
    Mapper map;
};

size_t InFunc(JDEC* jd, uint8_t* buf, size_t n) {
    auto* c = static_cast<Ctx*>(jd->device);
    if (c->f) {
        if (buf) return fread(buf, 1, n, c->f);
        return fseek(c->f, static_cast<long>(n), SEEK_CUR) == 0 ? n : 0;
    }
    size_t k = std::min<size_t>(n, c->mem_size - c->mem_pos);
    if (buf) memcpy(buf, c->mem + c->mem_pos, k);
    c->mem_pos += k;
    return k;
}

int OutFunc(JDEC* jd, void* bitmap, JRECT* rect) {
    auto* c = static_cast<Ctx*>(jd->device);
    const Mapper& m = c->map;
    const auto* rgb = static_cast<const uint8_t*>(bitmap);
    int left = rect->left, top = rect->top, right = rect->right, bottom = rect->bottom;
    int rw = right - left + 1;
    int a0, b0, a1, b1;
    m.Box(left, top, right, bottom, a0, b0, a1, b1);
    for (int ty = b0; ty <= b1; ty++) {
        for (int tx = a0; tx <= a1; tx++) {
            int sx, sy;
            if (!m.Inverse(tx, ty, sx, sy) || sx < left || sx > right || sy < top || sy > bottom) continue;
            const uint8_t* p = rgb + (static_cast<size_t>(sy - top) * rw + (sx - left)) * 3;
            m.dst[ty * m.tw + tx] = Rgb565(p[0], p[1], p[2]);
        }
    }
    return 1;
}

uint16_t Get16(const uint8_t* p, bool le) { return le ? p[0] | (p[1] << 8) : (p[0] << 8) | p[1]; }
uint32_t Get32(const uint8_t* p, bool le) {
    return le ? p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24)
              : (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

// EXIF（APP1 里的 TIFF）：方向、自带小图在 tiff 里的位置
struct Exif {
    int orient = 1;
    std::vector<uint8_t> app1;  // 整个 APP1 段的内容
    size_t thumb_at = 0, thumb_len = 0;  // 小图在 app1 里的位置
};

void ParseTiff(Exif& e, size_t tiff) {
    const std::vector<uint8_t>& d = e.app1;
    if (d.size() < tiff + 8) return;
    bool le = d[tiff] == 'I';
    auto ifd_entries = [&](size_t ifd, auto&& each) -> size_t {  // 返回下一个 IFD 的位置（0 = 没有）
        if (ifd == 0 || tiff + ifd + 2 > d.size()) return 0;
        size_t at = tiff + ifd;
        int n = Get16(&d[at], le);
        at += 2;
        for (int i = 0; i < n && at + 12 <= d.size(); i++, at += 12) each(Get16(&d[at], le), &d[at]);
        return at + 4 <= d.size() ? Get32(&d[at], le) : 0;
    };
    size_t ifd1 = ifd_entries(Get32(&d[tiff + 4], le), [&](uint16_t tag, const uint8_t* ent) {
        if (tag == 0x0112) e.orient = Get16(ent + 8, le);
    });
    uint32_t off = 0, len = 0;
    ifd_entries(ifd1, [&](uint16_t tag, const uint8_t* ent) {
        if (tag == 0x0201) off = Get32(ent + 8, le);
        if (tag == 0x0202) len = Get32(ent + 8, le);
    });
    if (off && len && tiff + off + len <= d.size()) {
        e.thumb_at = tiff + off;
        e.thumb_len = len;
    }
}

// 从文件开头一段段往后找 APP1（到 SOS 为止）
void ReadExif(FILE* f, Exif& e) {
    uint8_t h[4];
    if (fseek(f, 2, SEEK_SET) != 0) return;
    for (int seg = 0; seg < 32 && fread(h, 1, 4, f) == 4; seg++) {
        if (h[0] != 0xFF || h[1] == 0xDA || h[1] == 0xD9) return;
        size_t len = (h[2] << 8) | h[3];
        if (len < 2) return;
        if (h[1] == 0xE1 && len - 2 <= kMaxExif) {
            e.app1.resize(len - 2);
            if (fread(e.app1.data(), 1, e.app1.size(), f) != e.app1.size()) return;
            if (e.app1.size() > 14 && memcmp(e.app1.data(), "Exif\0\0", 6) == 0) {
                ParseTiff(e, 6);
                return;
            }
            e.app1.clear();
            continue;
        }
        if (fseek(f, static_cast<long>(len - 2), SEEK_CUR) != 0) return;
    }
}

// 用 TJpgDec 解（文件或内存里的一段），按 map 落到目标图上。need_w / need_h 是在源图方向上至少要多大
JRESULT DecodeJpeg(Ctx& c, int full_w, int full_h, NomiImage::Fit fit, JDEC& jd, void* work) {
    int orient = c.map.orient;
    bool turn = orient == 6 || orient == 8;
    int full_ow = turn ? full_h : full_w, full_oh = turn ? full_w : full_h;
    Place(c.map, full_ow, full_oh, fit);
    int need_w = turn ? c.map.dh : c.map.dw, need_h = turn ? c.map.dw : c.map.dh;
    int scale = 0;
    while (scale < 3 && (full_w >> (scale + 1)) >= need_w && (full_h >> (scale + 1)) >= need_h) scale++;
    c.map.sw = (full_w + (1 << scale) - 1) >> scale;
    c.map.sh = (full_h + (1 << scale) - 1) >> scale;
    c.map.ow = turn ? c.map.sh : c.map.sw;
    c.map.oh = turn ? c.map.sw : c.map.sh;
    (void)work;
    return jd_decomp(&jd, OutFunc, static_cast<uint8_t>(scale));
}

bool LoadJpeg(FILE* f, int w, int h, NomiImage::Fit fit, NomiImage::Source source, NomiImage::Image& out,
              std::string& error) {
    Exif exif;
    ReadExif(f, exif);
    void* work = heap_caps_malloc(kWork, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);  // 查表在内部 RAM 快很多
    if (!work) work = heap_caps_malloc(kWork, MALLOC_CAP_SPIRAM);
    if (!work) {
        error = "内存不够";
        return false;
    }
    Ctx c;
    c.map.dst = out.pixels;
    c.map.tw = w;
    c.map.th = h;
    c.map.orient = exif.orient == 3 || exif.orient == 6 || exif.orient == 8 ? exif.orient : 1;
    JDEC jd;
    JRESULT r = JDR_FMT1;
    bool done = false;
    // EXIF 里自带的小图（手机照片一般有 160×120）：缩略图够大就用；预览多小都用
    if (source != NomiImage::kMain && exif.thumb_len) {
        c.mem = exif.app1.data() + exif.thumb_at;
        c.mem_size = exif.thumb_len;
        if (jd_prepare(&jd, InFunc, work, kWork, &c) == JDR_OK &&
            (source == NomiImage::kThumbOnly || static_cast<int>(std::min(jd.width, jd.height)) >= std::min(w, h))) {
            r = DecodeJpeg(c, jd.width, jd.height, fit, jd, work);
            done = r == JDR_OK;
        }
        c.mem = nullptr;
    }
    if (!done && source == NomiImage::kThumbOnly) {
        heap_caps_free(work);
        error = "没有自带的小图";
        return false;
    }
    if (!done) {
        // 主图：从文件开头边读边解
        c.f = f;
        fseek(f, 0, SEEK_SET);
        r = jd_prepare(&jd, InFunc, work, kWork, &c);
        if (r == JDR_OK) {
            out.src_w = c.map.orient == 6 || c.map.orient == 8 ? jd.height : jd.width;
            out.src_h = c.map.orient == 6 || c.map.orient == 8 ? jd.width : jd.height;
            if (out.src_w * out.src_h > 50 * 1000 * 1000) r = JDR_FMT2;  // 超过 5000 万像素：解太久，不解了
            r = DecodeJpeg(c, jd.width, jd.height, fit, jd, work);
        }
    }
    heap_caps_free(work);
    if (r == JDR_OK) return true;
    error = r == JDR_FMT3   ? "不支持这种 JPEG（渐进式的）"
            : r == JDR_FMT2 ? "图太大了（超过 5000 万像素）"
            : r == JDR_INP  ? "读文件出错"
            : r == JDR_MEM1 ? "内存不够"
                            : "图片坏了，解不开";
    ESP_LOGW(TAG, "JPEG decode failed: %d", static_cast<int>(r));
    return false;
}

/* ---------- PNG：一块块解压（ROM 的 tinfl）、一行行还原，只留两行在内存里 ---------- */

uint32_t Be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

struct Png {
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = 0, channels = 1, bpp = 1;  // bpp：还原滤波时往左看几个字节
    size_t rowbytes = 0;
    uint8_t pal[256][4] = {};
    int pal_n = 0;
    bool has_trns = false;
    uint16_t trns[3] = {};
    std::vector<uint8_t> cur, prev;  // 一行：第一个字节是滤波方式
    size_t fill = 0;
    uint32_t y = 0;
    std::vector<int32_t> row_of, col_of;  // 目标的第几行 / 列来自原图的第几行 / 列（-1 = 黑边）
    int next_ty = 0;
    uint16_t* dst = nullptr;
    int tw = 0, th = 0;
};

uint16_t Sample(const Png& p, const uint8_t* row, int index) {
    if (p.depth == 8) return row[index];
    if (p.depth == 16) return static_cast<uint16_t>((row[index * 2] << 8) | row[index * 2 + 1]);
    int bit = index * p.depth;
    return (row[bit >> 3] >> (8 - p.depth - (bit & 7))) & ((1 << p.depth) - 1);
}

uint8_t To8(const Png& p, uint16_t v) {
    if (p.depth == 16) return v >> 8;
    if (p.depth == 8) return static_cast<uint8_t>(v);
    return static_cast<uint8_t>(v * 255 / ((1 << p.depth) - 1));
}

uint16_t PngPixel(const Png& p, const uint8_t* row, int x) {
    uint16_t r = 0, g = 0, b = 0;
    uint8_t R = 0, G = 0, B = 0, A = 255;
    switch (p.ctype) {
    case 0:
        r = Sample(p, row, x);
        if (p.has_trns && r == p.trns[0]) A = 0;
        R = G = B = To8(p, r);
        break;
    case 2:
        r = Sample(p, row, x * 3), g = Sample(p, row, x * 3 + 1), b = Sample(p, row, x * 3 + 2);
        if (p.has_trns && r == p.trns[0] && g == p.trns[1] && b == p.trns[2]) A = 0;
        R = To8(p, r), G = To8(p, g), B = To8(p, b);
        break;
    case 3: {
        uint16_t i = Sample(p, row, x);
        if (i < p.pal_n) R = p.pal[i][0], G = p.pal[i][1], B = p.pal[i][2], A = p.pal[i][3];
        break;
    }
    case 4:
        R = G = B = To8(p, Sample(p, row, x * 2));
        A = To8(p, Sample(p, row, x * 2 + 1));
        break;
    default:  // 6
        R = To8(p, Sample(p, row, x * 4)), G = To8(p, Sample(p, row, x * 4 + 1));
        B = To8(p, Sample(p, row, x * 4 + 2)), A = To8(p, Sample(p, row, x * 4 + 3));
        break;
    }
    if (A != 255) R = R * A / 255, G = G * A / 255, B = B * A / 255;  // 透明的压在黑上
    return Rgb565(R, G, B);
}

bool Unfilter(Png& p) {
    uint8_t* c = &p.cur[1];
    const uint8_t* u = &p.prev[1];
    size_t n = p.rowbytes, bpp = p.bpp;
    switch (p.cur[0]) {
    case 0:
        break;
    case 1:
        for (size_t i = bpp; i < n; i++) c[i] += c[i - bpp];
        break;
    case 2:
        for (size_t i = 0; i < n; i++) c[i] += u[i];
        break;
    case 3:
        for (size_t i = 0; i < n; i++) c[i] += ((i >= bpp ? c[i - bpp] : 0) + u[i]) >> 1;
        break;
    case 4:
        for (size_t i = 0; i < n; i++) {
            int a = i >= bpp ? c[i - bpp] : 0, b = u[i], cc = i >= bpp ? u[i - bpp] : 0;
            int pp = a + b - cc, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - cc);
            c[i] += pa <= pb && pa <= pc ? a : pb <= pc ? b : cc;
        }
        break;
    default:
        return false;
    }
    return true;
}

// 解压出来的一段：凑够一行就还原、落到目标图上
bool PngFeed(Png& p, const uint8_t* data, size_t n) {
    while (n > 0 && p.y < p.h) {
        size_t take = std::min(n, p.rowbytes + 1 - p.fill);
        memcpy(&p.cur[p.fill], data, take);
        p.fill += take;
        data += take;
        n -= take;
        if (p.fill < p.rowbytes + 1) break;
        if (!Unfilter(p)) return false;
        while (p.next_ty < p.th && p.row_of[p.next_ty] < static_cast<int32_t>(p.y)) p.next_ty++;
        for (int ty = p.next_ty; ty < p.th && p.row_of[ty] == static_cast<int32_t>(p.y); ty++) {
            uint16_t* out = p.dst + ty * p.tw;
            for (int tx = 0; tx < p.tw; tx++) {
                if (p.col_of[tx] >= 0) out[tx] = PngPixel(p, &p.cur[1], p.col_of[tx]);
            }
        }
        std::swap(p.cur, p.prev);
        p.fill = 0;
        p.y++;
    }
    return true;
}

bool PngHeader(Png& p, const uint8_t* d, int w, int h, NomiImage::Fit fit, std::string& error) {
    p.w = Be32(d);
    p.h = Be32(d + 4);
    p.depth = d[8];
    p.ctype = d[9];
    if (d[12] != 0) {
        error = "不支持隔行扫描的 PNG";
        return false;
    }
    static const int kChannels[] = {1, 0, 3, 1, 2, 0, 4};
    p.channels = p.ctype <= 6 ? kChannels[p.ctype] : 0;
    bool depth_ok = p.depth == 1 || p.depth == 2 || p.depth == 4 || p.depth == 8 || p.depth == 16;
    if (!p.channels || !depth_ok || p.w == 0 || p.h == 0 || p.w > kMaxPngSide || p.h > kMaxPngSide) {
        error = "不认识的 PNG";
        return false;
    }
    p.rowbytes = (static_cast<size_t>(p.w) * p.channels * p.depth + 7) / 8;
    p.bpp = std::max(1, p.channels * p.depth / 8);
    p.cur.assign(p.rowbytes + 1, 0);
    p.prev.assign(p.rowbytes + 1, 0);
    Mapper m;
    m.tw = w;
    m.th = h;
    m.sw = m.ow = p.w;
    m.sh = m.oh = p.h;
    Place(m, p.w, p.h, fit);
    p.row_of.assign(h, -1);
    p.col_of.assign(w, -1);
    for (int ty = 0; ty < h; ty++) {
        int sx, sy;
        if (m.Inverse(m.dx > 0 ? m.dx : 0, ty, sx, sy)) p.row_of[ty] = sy;
    }
    for (int tx = 0; tx < w; tx++) {
        int sx, sy;
        if (m.Inverse(tx, m.dy > 0 ? m.dy : 0, sx, sy)) p.col_of[tx] = sx;
    }
    return true;
}

bool LoadPng(FILE* f, int w, int h, NomiImage::Fit fit, NomiImage::Image& out, std::string& error) {
    Png p;
    p.dst = out.pixels;
    p.tw = w;
    p.th = h;
    auto* inf = static_cast<tinfl_decompressor*>(heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM));
    auto* dict = static_cast<uint8_t*>(heap_caps_malloc(TINFL_LZ_DICT_SIZE, MALLOC_CAP_SPIRAM));
    auto* in = static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
    bool header = false, done = false;
    if (!inf || !dict || !in) error = "内存不够";
    tinfl_init(inf);
    size_t dict_ofs = 0;
    fseek(f, 8, SEEK_SET);
    while (error.empty() && !done) {
        uint8_t hd[8];
        if (fread(hd, 1, 8, f) != 8) {
            error = "PNG 不完整";
            break;
        }
        uint32_t len = Be32(hd);
        const char* type = reinterpret_cast<const char*>(hd + 4);
        if (!memcmp(type, "IHDR", 4) && len == 13) {
            uint8_t d[13];
            if (fread(d, 1, 13, f) != 13 || !PngHeader(p, d, w, h, fit, error)) break;
            header = true;
            out.src_w = p.w;
            out.src_h = p.h;
            len = 0;
        } else if (!memcmp(type, "PLTE", 4) && len <= 768) {
            uint8_t d[768];
            if (fread(d, 1, len, f) != len) break;
            p.pal_n = len / 3;
            for (int i = 0; i < p.pal_n; i++) {
                p.pal[i][0] = d[i * 3], p.pal[i][1] = d[i * 3 + 1], p.pal[i][2] = d[i * 3 + 2], p.pal[i][3] = 255;
            }
            len = 0;
        } else if (!memcmp(type, "tRNS", 4) && len <= 256) {
            uint8_t d[256];
            if (fread(d, 1, len, f) != len) break;
            if (p.ctype == 3) {
                for (uint32_t i = 0; i < len; i++) p.pal[i][3] = d[i];
            } else if (p.ctype == 0 && len >= 2) {
                p.has_trns = true, p.trns[0] = (d[0] << 8) | d[1];
            } else if (p.ctype == 2 && len >= 6) {
                p.has_trns = true;
                for (int i = 0; i < 3; i++) p.trns[i] = (d[i * 2] << 8) | d[i * 2 + 1];
            }
            len = 0;
        } else if (!memcmp(type, "IDAT", 4)) {
            if (!header) {
                error = "不认识的 PNG";
                break;
            }
            while (len > 0 && error.empty() && !done) {
                size_t k = std::min<size_t>(len, 4096);
                if (fread(in, 1, k, f) != k) {
                    error = "PNG 不完整";
                    break;
                }
                len -= k;
                const uint8_t* ip = in;
                size_t avail = k;
                for (;;) {
                    size_t in_bytes = avail, out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
                    tinfl_status st = tinfl_decompress(inf, ip, &in_bytes, dict, dict + dict_ofs, &out_bytes,
                                                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
                    ip += in_bytes;
                    avail -= in_bytes;
                    if (out_bytes && !PngFeed(p, dict + dict_ofs, out_bytes)) {
                        error = "图片坏了，解不开";
                        break;
                    }
                    dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
                    if (st == TINFL_STATUS_DONE || p.y >= p.h) {
                        done = true;
                        break;
                    }
                    if (st < 0) {
                        error = "图片坏了，解不开";
                        break;
                    }
                    if (st == TINFL_STATUS_NEEDS_MORE_INPUT && avail == 0) break;
                }
            }
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        if (len > 0 && fseek(f, static_cast<long>(len), SEEK_CUR) != 0) break;  // 不认识的块、没读完的
        if (fseek(f, 4, SEEK_CUR) != 0) break;                                    // CRC
    }
    heap_caps_free(inf);
    heap_caps_free(dict);
    heap_caps_free(in);
    if (error.empty() && (!header || p.y == 0)) error = "图片坏了，解不开";
    return error.empty();
}

}  // namespace

namespace NomiImage {

Image::~Image() { heap_caps_free(pixels); }

Image::Image(Image&& o) noexcept : pixels(o.pixels), w(o.w), h(o.h), src_w(o.src_w), src_h(o.src_h) {
    o.pixels = nullptr;
}

Image& Image::operator=(Image&& o) noexcept {
    if (this != &o) {
        heap_caps_free(pixels);
        pixels = o.pixels;
        w = o.w, h = o.h, src_w = o.src_w, src_h = o.src_h;
        o.pixels = nullptr;
    }
    return *this;
}

bool Load(const std::string& path, int w, int h, Fit fit, Source source, Image& out, std::string& error) {
    out = Image();
    if (w <= 0 || h <= 0) return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        error = "打不开文件";
        return false;
    }
    setvbuf(f, nullptr, _IOFBF, 4096);
    uint8_t sig[8] = {};
    size_t got = fread(sig, 1, sizeof(sig), f);
    out.pixels = static_cast<uint16_t*>(heap_caps_calloc(static_cast<size_t>(w) * h, 2, MALLOC_CAP_SPIRAM));
    if (!out.pixels) {
        fclose(f);
        error = "内存不够";
        return false;
    }
    out.w = w;
    out.h = h;
    bool ok = false;
    if (got >= 3 && sig[0] == 0xFF && sig[1] == 0xD8 && sig[2] == 0xFF) {
        ok = LoadJpeg(f, w, h, fit, source, out, error);
    } else if (got == 8 && memcmp(sig, "\x89PNG\r\n\x1a\n", 8) == 0) {
        if (source == kThumbOnly) error = "没有自带的小图";
        else ok = LoadPng(f, w, h, fit, out, error);
    } else {
        error = "不认识的图片格式（只认 JPEG、PNG）";
    }
    fclose(f);
    if (!ok) out = Image();
    return ok;
}

}  // namespace NomiImage
