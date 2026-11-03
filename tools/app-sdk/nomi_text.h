// 应用里处理文字的小工具（没有 C 库）：查找、拷贝、XML / HTML 字段、JSON 字段、URL 编码、base64、哈希。
// 全是纯 C，编进应用里，不调系统接口。范围都写成 [s, end)：网页、RSS 很大时只在一段里找。
#pragma once

#include <stdint.h>

static inline int32_t nomi_len(const char *s)
{
    int32_t n = 0;
    while (s[n]) n++;
    return n;
}

// [s, end) 里找 needle，找不到返回 0（NULL）
static inline const char *nomi_find(const char *s, const char *end, const char *needle)
{
    int32_t m = nomi_len(needle);
    if (m == 0) return s;
    for (; s + m <= end; s++) {
        int32_t k = 0;
        while (k < m && s[k] == needle[k]) k++;
        if (k == m) return s;
    }
    return 0;
}

static inline int nomi_starts(const char *s, const char *prefix)
{
    while (*prefix && *s == *prefix) s++, prefix++;
    return *prefix == 0;
}

static inline int nomi_equal(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

// 拷一段（[s, end)，end 为 0 时拷到 '\0'），一定以 '\0' 结尾，返回长度
static inline int32_t nomi_copy_n(char *dst, int32_t size, const char *s, const char *end)
{
    int32_t n = 0;
    if (size <= 0) return 0;
    while ((end ? s < end : *s != 0) && n < size - 1) dst[n++] = *s++;
    dst[n] = 0;
    return n;
}

// 接在 dst 后面，返回新长度（放不下的截掉）
static inline int32_t nomi_append(char *dst, int32_t size, const char *src)
{
    int32_t n = nomi_len(dst);
    while (*src && n < size - 1) dst[n++] = *src++;
    if (size > 0) dst[n < size ? n : size - 1] = 0;
    return n;
}

static inline int32_t nomi_put_utf8(char *out, int32_t size, int32_t n, uint32_t cp)
{
    char b[4];
    int32_t k;
    if (cp < 0x80) {
        b[0] = (char)cp, k = 1;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6)), b[1] = (char)(0x80 | (cp & 0x3F)), k = 2;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12)), b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)), b[2] = (char)(0x80 | (cp & 0x3F)), k = 3;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18)), b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)), b[3] = (char)(0x80 | (cp & 0x3F)), k = 4;
    }
    for (int32_t i = 0; i < k && n < size - 1; i++) out[n++] = b[i];
    return n;
}

static inline int nomi_hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// XML / HTML 的文字：解开实体（&amp; &lt; &#39; &#x4E2D; …）、CDATA 原样、去掉首尾空白、标签去掉。返回长度
static inline int32_t nomi_xml_text(const char *s, const char *end, char *out, int32_t size)
{
    int32_t n = 0;
    if (size <= 0) return 0;
    while (s < end && (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')) s++;
    while (s < end && n < size - 1) {
        if (*s == '<') {
            if (s + 9 <= end && nomi_starts(s, "<![CDATA[")) {
                const char *e = nomi_find(s + 9, end, "]]>");
                if (!e) e = end;
                for (s += 9; s < e && n < size - 1;) out[n++] = *s++;
                s = e + 3 <= end ? e + 3 : end;
            } else {  // 文字里夹的标签（<p>、<br/>）去掉
                while (s < end && *s != '>') s++;
                if (s < end) s++;
            }
            continue;
        }
        if (*s == '&') {
            const char *semi = s + 1;
            while (semi < end && semi - s < 10 && *semi != ';') semi++;
            if (semi < end && *semi == ';') {
                uint32_t cp = 0;
                const char *name = s + 1;
                if (*name == '#') {
                    int hex = name[1] == 'x' || name[1] == 'X';
                    for (const char *p = name + 1 + hex; p < semi; p++) {
                        int d = hex ? nomi_hex_digit(*p) : (*p >= '0' && *p <= '9' ? *p - '0' : -1);
                        if (d < 0) break;
                        cp = cp * (hex ? 16 : 10) + (uint32_t)d;
                    }
                } else if (nomi_starts(name, "amp;")) cp = '&';
                else if (nomi_starts(name, "lt;")) cp = '<';
                else if (nomi_starts(name, "gt;")) cp = '>';
                else if (nomi_starts(name, "quot;")) cp = '"';
                else if (nomi_starts(name, "apos;")) cp = '\'';
                else if (nomi_starts(name, "nbsp;")) cp = ' ';
                if (cp) {
                    n = nomi_put_utf8(out, size, n, cp);
                    s = semi + 1;
                    continue;
                }
            }
        }
        out[n++] = *s++;
    }
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == '\t')) n--;
    out[n] = 0;
    return n;
}

// [s, end) 里第一个 <tag …>…</tag> 的文字；attr 不空时取 <tag … attr="…"> 的属性值。没有返回 -1
static inline int32_t nomi_xml_field(const char *s, const char *end, const char *tag, const char *attr, char *out,
                                     int32_t size)
{
    char open[64] = "<";
    nomi_append(open, sizeof(open), tag);
    int32_t tl = nomi_len(open);
    if (size > 0) out[0] = 0;
    for (const char *p = s; (p = nomi_find(p, end, open)) != 0; p += tl) {
        char c = p + tl < end ? p[tl] : 0;
        if (c != '>' && c != ' ' && c != '/' && c != '\n' && c != '\t' && c != '\r') continue;  // <titleX> 不算
        const char *gt = nomi_find(p, end, ">");
        if (!gt) return -1;
        if (attr) {
            char key[48];
            key[0] = 0;
            nomi_append(key, sizeof(key), " ");
            nomi_append(key, sizeof(key), attr);
            nomi_append(key, sizeof(key), "=");
            const char *a = nomi_find(p, gt, key);
            if (!a) continue;
            a += nomi_len(key);
            char q = *a;
            if (q != '"' && q != '\'') continue;
            const char *e = a + 1;
            while (e < gt && *e != q) e++;
            return nomi_xml_text(a + 1, e, out, size);
        }
        if (gt[-1] == '/') return nomi_xml_text(gt, gt, out, size);  // <tag/>
        char close[64] = "</";
        nomi_append(close, sizeof(close), tag);
        const char *e = nomi_find(gt + 1, end, close);
        if (!e) return -1;
        return nomi_xml_text(gt + 1, e, out, size);
    }
    return -1;
}

// JSON 文本 [s, end) 里 "key": 后面的值：字符串解开转义（含 \uXXXX、代理对），数字、true 之类原样。没有返回 -1
static inline int32_t nomi_json_field(const char *s, const char *end, const char *key, char *out, int32_t size)
{
    char pat[64] = "\"";
    nomi_append(pat, sizeof(pat), key);
    nomi_append(pat, sizeof(pat), "\"");
    int32_t pl = nomi_len(pat);
    if (size > 0) out[0] = 0;
    for (const char *p = s; (p = nomi_find(p, end, pat)) != 0; p += pl) {
        const char *v = p + pl;
        while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
        if (v >= end || *v != ':') continue;  // 是个值，不是键
        v++;
        while (v < end && (*v == ' ' || *v == '\t' || *v == '\n' || *v == '\r')) v++;
        int32_t n = 0;
        if (v < end && *v == '"') {
            for (v++; v < end && *v != '"' && n < size - 1; v++) {
                if (*v != '\\' || v + 1 >= end) {
                    out[n++] = *v;
                    continue;
                }
                char e = *++v;
                if (e == 'n') out[n++] = '\n';
                else if (e == 't') out[n++] = '\t';
                else if (e == 'r' || e == 'b' || e == 'f') continue;
                else if (e == 'u' && v + 4 < end) {
                    uint32_t cp = 0;
                    for (int i = 1; i <= 4; i++) cp = cp * 16 + (uint32_t)(nomi_hex_digit(v[i]) & 15);
                    v += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && v + 6 < end && v[1] == '\\' && v[2] == 'u') {
                        uint32_t lo = 0;
                        for (int i = 3; i <= 6; i++) lo = lo * 16 + (uint32_t)(nomi_hex_digit(v[i]) & 15);
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        v += 6;
                    }
                    n = nomi_put_utf8(out, size, n, cp);
                } else {
                    out[n++] = e;  // \" \\ \/
                }
            }
        } else {
            while (v < end && *v != ',' && *v != '}' && *v != ']' && *v != ' ' && *v != '\n' && n < size - 1) {
                out[n++] = *v++;
            }
        }
        if (size > 0) out[n] = 0;
        return n;
    }
    return -1;
}

// URL 编码（查询参数、路径里的一段）
static inline int32_t nomi_url_encode(const char *s, char *out, int32_t size)
{
    static const char hex[] = "0123456789ABCDEF";
    int32_t n = 0;
    for (; *s && n < size - 3; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    if (size > 0) out[n] = 0;
    return n;
}

// base64 解码（跳过空白），返回字节数（结果后面补 '\0'），不是 base64 返回 -1
static inline int32_t nomi_base64_decode(const char *s, int32_t len, char *out, int32_t size)
{
    uint32_t acc = 0;
    int bits = 0;
    int32_t n = 0;
    for (int32_t i = 0; i < len; i++) {
        char c = s[i];
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else if (c == '=' || c == ' ' || c == '\n' || c == '\r') continue;
        else return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= size - 1) return -1;
            out[n++] = (char)((acc >> bits) & 0xFF);
        }
    }
    if (size > 0) out[n] = 0;
    return n;
}

// FNV-1a：拿来给长地址起个短名字（存 kv 的键）
static inline uint32_t nomi_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static inline void nomi_hex32(uint32_t v, char out[9])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 7; i >= 0; i--, v >>= 4) out[i] = hex[v & 15];
    out[8] = 0;
}
