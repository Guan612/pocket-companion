#include "nomi_mp4.h"

#include <cstring>

namespace {

uint32_t U32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t U64(const uint8_t* p) { return (uint64_t(U32(p)) << 32) | U32(p + 4); }

// 盒子：[size 4][type 4]（size 为 1 时后面跟 8 字节大小）；回调拿到类型、内容起止
struct Box {
    const uint8_t* body;
    const uint8_t* end;
    char type[5];
};

bool NextBox(const uint8_t*& p, const uint8_t* end, Box& box) {
    if (end - p < 8) return false;
    uint64_t size = U32(p);
    size_t header = 8;
    if (size == 1) {
        if (end - p < 16) return false;
        size = U64(p + 8);
        header = 16;
    } else if (size == 0) {
        size = end - p;
    }
    if (size < header || size > static_cast<uint64_t>(end - p)) return false;
    memcpy(box.type, p + 4, 4);
    box.type[4] = '\0';
    box.body = p + header;
    box.end = p + size;
    p += size;
    return true;
}

bool FindChild(const uint8_t* body, const uint8_t* end, const char* type, Box& out) {
    const uint8_t* p = body;
    Box box;
    while (NextBox(p, end, box)) {
        if (memcmp(box.type, type, 4) == 0) {
            out = box;
            return true;
        }
    }
    return false;
}

// 描述符长度：每字节低 7 位，最高位为 1 表示后面还有
uint32_t DescLen(const uint8_t*& p, const uint8_t* end) {
    uint32_t len = 0;
    for (int i = 0; i < 4 && p < end; i++) {
        uint8_t b = *p++;
        len = (len << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return len;
}

struct Bits {
    const uint8_t* p;
    size_t len, pos = 0;
    uint32_t Read(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; i++, pos++) {
            v <<= 1;
            if (pos / 8 < len) v |= (p[pos / 8] >> (7 - pos % 8)) & 1;
        }
        return v;
    }
};

// esds → AudioSpecificConfig：取核心编码类型、采样率下标、声道数（HE-AAC 用核心的 LC 配置，解码器自己认 SBR）
bool ParseEsds(const uint8_t* p, const uint8_t* end, Mp4Track& t) {
    p += 4;  // version / flags
    while (p < end) {
        uint8_t tag = *p++;
        uint32_t len = DescLen(p, end);
        if (tag == 0x03) {  // ES_Descriptor
            if (end - p < 3) return false;
            uint8_t flags = p[2];
            p += 3;
            if (flags & 0x80) p += 2;
            if (flags & 0x40) p += 1 + (p < end ? *p : 0);
            if (flags & 0x20) p += 2;
            continue;  // 里面套着 DecoderConfig
        }
        if (tag == 0x04) {  // DecoderConfigDescriptor：13 字节固定字段后面套 DecoderSpecificInfo
            p += 13;
            continue;
        }
        if (tag == 0x05) {
            Bits b{p, len};
            int aot = b.Read(5);
            if (aot == 31) aot = 32 + b.Read(6);
            int sfi = b.Read(4);
            if (sfi == 15) b.Read(24);
            int ch = b.Read(4);
            if (aot == 5 || aot == 29) {  // SBR / PS：后面跟扩展采样率和核心类型
                int ext = b.Read(4);
                if (ext == 15) b.Read(24);
                aot = b.Read(5);
            }
            t.profile = aot >= 1 && aot <= 4 ? aot - 1 : 1;
            t.sf_index = sfi < 13 ? sfi : 4;
            t.channels = ch >= 1 && ch <= 7 ? ch : 2;
            return true;
        }
        p += len;
    }
    return false;
}

}  // namespace

bool Mp4FindMoov(const uint8_t* data, size_t len, uint64_t base, uint64_t& moov_offset, uint64_t& moov_size,
                 uint64_t& next) {
    uint64_t pos = 0;
    while (pos + 8 <= len) {
        uint64_t size = U32(data + pos);
        if (size == 1) {
            if (pos + 16 > len) break;
            size = U64(data + pos + 8);
        }
        if (size < 8) return false;
        if (memcmp(data + pos + 4, "moov", 4) == 0) {
            moov_offset = base + pos;
            moov_size = size;
            return true;
        }
        pos += size;
    }
    next = base + pos;  // 下一个盒子在这段数据外面
    return false;
}

bool Mp4ParseMoov(const uint8_t* moov, size_t size, Mp4Track& t) {
    const uint8_t* p = moov;
    Box root;
    if (!NextBox(p, moov + size, root) || strcmp(root.type, "moov") != 0) return false;
    const uint8_t* q = root.body;
    Box trak;
    while (NextBox(q, root.end, trak)) {
        if (strcmp(trak.type, "trak") != 0) continue;
        Box mdia, hdlr, mdhd, minf, stbl;
        if (!FindChild(trak.body, trak.end, "mdia", mdia)) continue;
        if (!FindChild(mdia.body, mdia.end, "hdlr", hdlr) || hdlr.end - hdlr.body < 12 ||
            memcmp(hdlr.body + 8, "soun", 4) != 0) {
            continue;  // 不是音频轨
        }
        if (!FindChild(mdia.body, mdia.end, "mdhd", mdhd) || !FindChild(mdia.body, mdia.end, "minf", minf) ||
            !FindChild(minf.body, minf.end, "stbl", stbl)) {
            continue;
        }
        t.timescale = mdhd.body[0] == 1 ? U32(mdhd.body + 20) : U32(mdhd.body + 12);
        Box stsd, stts, stsc, stsz, stco;
        if (!FindChild(stbl.body, stbl.end, "stsd", stsd) || !FindChild(stbl.body, stbl.end, "stts", stts) ||
            !FindChild(stbl.body, stbl.end, "stsc", stsc) || !FindChild(stbl.body, stbl.end, "stsz", stsz)) {
            return false;
        }
        t.co64 = !FindChild(stbl.body, stbl.end, "stco", stco);
        if (t.co64 && !FindChild(stbl.body, stbl.end, "co64", stco)) return false;
        // stsd：第一个条目要是 mp4a，36 字节的固定字段后面是 esds
        const uint8_t* entry = stsd.body + 8;
        if (stsd.end - entry < 36 || memcmp(entry + 4, "mp4a", 4) != 0) return false;
        Box esds;
        if (!FindChild(entry + 36, entry + U32(entry), "esds", esds) || !ParseEsds(esds.body, esds.end, t)) {
            return false;
        }
        t.stts_n = U32(stts.body + 4);
        t.stts = stts.body + 8;
        t.stsc_n = U32(stsc.body + 4);
        t.stsc = stsc.body + 8;
        t.stsz_const = U32(stsz.body + 4);
        t.stsz_n = U32(stsz.body + 8);
        t.stsz = stsz.body + 12;
        t.stco_n = U32(stco.body + 4);
        t.stco = stco.body + 8;
        return t.timescale > 0 && t.stco_n > 0 && t.stsc_n > 0;
    }
    return false;
}

uint32_t Mp4SampleAtSecond(const Mp4Track& t, uint32_t second) {
    uint64_t target = uint64_t(second) * t.timescale, acc = 0;
    uint32_t base = 0;
    for (uint32_t i = 0; i < t.stts_n; i++) {
        uint32_t count = U32(t.stts + i * 8), delta = U32(t.stts + i * 8 + 4);
        if (delta && target < acc + uint64_t(count) * delta) return base + static_cast<uint32_t>((target - acc) / delta);
        acc += uint64_t(count) * delta;
        base += count;
    }
    return base;  // 超出结尾
}

uint32_t Mp4DurationSeconds(const Mp4Track& t) {
    if (t.timescale == 0) return 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < t.stts_n; i++) total += uint64_t(U32(t.stts + i * 8)) * U32(t.stts + i * 8 + 4);
    return static_cast<uint32_t>(total / t.timescale);
}

static uint64_t ChunkOffset(const Mp4Track& t, uint32_t chunk) {
    return t.co64 ? U64(t.stco + chunk * 8) : U32(t.stco + chunk * 4);
}

static uint32_t SampleSize(const Mp4Track& t, uint32_t sample) {
    return t.stsz_const ? t.stsz_const : U32(t.stsz + sample * 4);
}

bool Mp4Seek(const Mp4Track& t, uint32_t sample, Mp4Cursor& c) {
    if (sample >= t.stsz_n) return false;
    uint32_t first = 0;  // 当前 stsc 条目覆盖的第一个样本
    for (uint32_t i = 0; i < t.stsc_n; i++) {
        uint32_t first_chunk = U32(t.stsc + i * 12) - 1, per = U32(t.stsc + i * 12 + 4);
        uint32_t next_chunk = i + 1 < t.stsc_n ? U32(t.stsc + (i + 1) * 12) - 1 : t.stco_n;
        uint64_t samples = uint64_t(next_chunk - first_chunk) * per;
        if (per && sample < first + samples) {
            uint32_t k = sample - first;
            c.chunk = first_chunk + k / per;
            c.in_chunk = k % per;
            c.per_chunk = per;
            c.stsc_i = i;
            c.sample = sample;
            c.offset = ChunkOffset(t, c.chunk);
            for (uint32_t s = sample - c.in_chunk; s < sample; s++) c.offset += SampleSize(t, s);
            return c.chunk < t.stco_n;
        }
        first += static_cast<uint32_t>(samples);
    }
    return false;
}

bool Mp4SampleSize(const Mp4Track& t, const Mp4Cursor& c, uint32_t& size) {
    if (c.sample >= t.stsz_n || c.chunk >= t.stco_n) return false;
    size = SampleSize(t, c.sample);
    return true;
}

void Mp4Advance(const Mp4Track& t, Mp4Cursor& c, uint32_t size) {
    c.sample++;
    c.offset += size;
    if (++c.in_chunk < c.per_chunk) return;
    // 下一个块：位置从块偏移表里取，每块样本数可能换了一个 stsc 条目
    c.chunk++;
    c.in_chunk = 0;
    if (c.chunk >= t.stco_n) return;
    if (c.stsc_i + 1 < t.stsc_n && c.chunk + 1 >= U32(t.stsc + (c.stsc_i + 1) * 12)) c.stsc_i++;
    c.per_chunk = U32(t.stsc + c.stsc_i * 12 + 4);
    c.offset = ChunkOffset(t, c.chunk);
}

void Mp4AdtsHeader(const Mp4Track& t, uint32_t payload, uint8_t out[7]) {
    uint32_t len = payload + 7;
    out[0] = 0xFF;
    out[1] = 0xF1;  // MPEG-4、无 CRC
    out[2] = static_cast<uint8_t>(((t.profile & 3) << 6) | ((t.sf_index & 0xF) << 2) | ((t.channels >> 2) & 1));
    out[3] = static_cast<uint8_t>(((t.channels & 3) << 6) | ((len >> 11) & 3));
    out[4] = static_cast<uint8_t>((len >> 3) & 0xFF);
    out[5] = static_cast<uint8_t>(((len & 7) << 5) | 0x1F);
    out[6] = 0xFC;
}
