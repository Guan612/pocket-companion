#include "nomi_mp4.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

namespace {

// 每个表的窗口最多放几个条目（实际按表的大小分配，AAC 常见的 stts、stsc 只有几条，只占几十字节）。
// stsz、stco 跟着节目长度涨：8192 帧在 44.1kHz 下约 3 分钟，放过一个窗口才去读下一段——要先断开正在下的音频，
// 读完再从原位置接着下，中间靠收流缓冲（几秒）垫着
#ifdef NOMI_MP4_TEST_WINDOW  // 电脑上测试用：窗口开得很小，边界多走几遍
constexpr uint32_t kSttsCap = NOMI_MP4_TEST_WINDOW, kStscCap = NOMI_MP4_TEST_WINDOW, kStszCap = NOMI_MP4_TEST_WINDOW;
constexpr uint32_t kStcoBytes = NOMI_MP4_TEST_WINDOW * 8;
#else
constexpr uint32_t kSttsCap = 2048;     // 16KB
constexpr uint32_t kStscCap = 2048;     // 24KB：有的封装每一两块就换一条，定位时要从头数
constexpr uint32_t kStszCap = 8192;     // 32KB
constexpr uint32_t kStcoBytes = 32768;  // 32KB：8192 个块（co64 是 4096 个）
#endif
// moov 里要跳过的部分（大表剩下的、别的轨、udta 里的封面）不到这么多就读掉扔了，
// 多了重新开一段跳过去（网上是一次重新连接）
constexpr uint64_t kSkipByReading = 128 * 1024;
constexpr int kMaxStalls = 3;
constexpr size_t kMaxStsd = 1024;  // stsd 只要第一个条目（mp4a + esds，一般不到 100 字节）

void* AllocBuf(size_t n) {
#ifdef ESP_PLATFORM
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
#else
    return malloc(n);
#endif
}

void FreeBuf(void* p) {
#ifdef ESP_PLATFORM
    heap_caps_free(p);
#else
    free(p);
#endif
}

void FreeTable(Mp4Table& t) {
    FreeBuf(t.buf);
    t = Mp4Table();
}

uint32_t U32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint64_t U64(const uint8_t* p) { return (uint64_t(U32(p)) << 32) | U32(p + 4); }

// 读满 n 字节（一阵子没数据就再等，最多几次）
bool ReadFull(Mp4Source& s, uint8_t* out, size_t n) {
    int stalls = 0;
    while (n > 0) {
        int r = s.Read(out, n);
        if (r == -2 && ++stalls <= kMaxStalls) continue;
        if (r <= 0) return false;
        stalls = 0;
        out += r;
        n -= r;
    }
    return true;
}

// 顺着往后读 moov 的读头：往后跳得近就读掉扔了，跳得远就从那里重新开一段
class Reader {
public:
    explicit Reader(Mp4Source& s) : s_(s) {}
    uint64_t pos = 0;

    bool Start(uint64_t at) {
        pos = at;
        return s_.Open(at, Mp4Source::kToEnd);
    }
    bool Read(void* out, size_t n) {
        if (!ReadFull(s_, static_cast<uint8_t*>(out), n)) return false;
        pos += n;
        return true;
    }
    bool SkipTo(uint64_t to) {
        if (to < pos) return false;
        if (to == pos) return true;
        if (to - pos > kSkipByReading) return Start(to);
        uint8_t junk[512];
        while (pos < to) {
            if (!Read(junk, static_cast<size_t>(std::min<uint64_t>(sizeof(junk), to - pos)))) return false;
        }
        return true;
    }

private:
    Mp4Source& s_;
};

// 盒子：[size 4][type 4]（size 为 1 时后面跟 8 字节大小，为 0 时一直到外层的结尾）
struct Box {
    char type[4];
    uint64_t start, end;  // 整个盒子在文件里的起止
};

bool Is(const Box& b, const char* type) { return memcmp(b.type, type, 4) == 0; }

// 在 r.pos 读一个盒子头；limit 是外层盒子的结尾（顶层为 kToEnd）
bool ReadBox(Reader& r, uint64_t limit, Box& b) {
    uint8_t h[16];
    b.start = r.pos;
    if (!r.Read(h, 8)) return false;
    uint64_t size = U32(h);
    uint64_t header = 8;
    memcpy(b.type, h + 4, 4);
    if (size == 1) {
        if (!r.Read(h + 8, 8)) return false;
        size = U64(h + 8);
        header = 16;
    } else if (size == 0) {
        if (limit == Mp4Source::kToEnd) return false;  // 顶层一直到文件结尾的（只能是 mdat），moov 不会在它后面
        size = limit - b.start;
    }
    if (size < header || size > Mp4Source::kToEnd - b.start) return false;
    if (limit != Mp4Source::kToEnd && b.start + size > limit) return false;
    b.end = b.start + size;
    return true;
}

/* ---------- 内存里的小盒子（stsd 里的 esds） ---------- */

struct MemBox {
    const uint8_t* body;
    const uint8_t* end;
    char type[5];
};

bool NextBox(const uint8_t*& p, const uint8_t* end, MemBox& box) {
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

bool FindChild(const uint8_t* body, const uint8_t* end, const char* type, MemBox& out) {
    const uint8_t* p = body;
    MemBox box;
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

// 一条轨读下来的东西：是音频轨、表都齐了才用
struct TrakInfo {
    bool audio = false, skip = false, esds = false;
    uint32_t timescale = 0;
    uint64_t duration = 0;
    int profile = 1, sf_index = 4, channels = 2;
    Mp4Table stts, stsc, stsz, stco;
    bool have_stts = false, have_stsc = false, have_stsz = false, have_stco = false;
    uint32_t stsz_const = 0, stsz_count = 0;

    ~TrakInfo() {
        FreeTable(stts);
        FreeTable(stsc);
        FreeTable(stsz);
        FreeTable(stco);
    }
    bool Usable() const {
        return audio && esds && timescale > 0 && have_stts && have_stsc && have_stsz && have_stco &&
               stco.count > 0 && stsc.count > 0;
    }
};

// esds → AudioSpecificConfig：取核心编码类型、采样率下标、声道数（HE-AAC 用核心的 LC 配置，解码器自己认 SBR）
bool ParseEsds(const uint8_t* p, const uint8_t* end, TrakInfo& t) {
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
            Bits b{p, std::min<size_t>(len, end > p ? end - p : 0)};
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

// stsd：第一个条目要是 mp4a，36 字节的固定字段后面是 esds
bool ReadStsd(Reader& r, const Box& b, TrakInfo& t) {
    size_t n = static_cast<size_t>(std::min<uint64_t>(b.end - r.pos, kMaxStsd));
    auto* buf = static_cast<uint8_t*>(AllocBuf(n > 0 ? n : 1));
    if (!buf) return false;
    bool ok = r.Read(buf, n);
    const uint8_t* entry = buf + 8;
    const uint8_t* end = buf + n;
    if (ok && n >= 8 + 36 && memcmp(entry + 4, "mp4a", 4) == 0) {
        const uint8_t* entry_end = entry + std::min<size_t>(U32(entry), end - entry);
        MemBox esds;
        t.esds = entry_end > entry + 36 && FindChild(entry + 36, entry_end, "esds", esds) &&
                 ParseEsds(esds.body, esds.end, t);
    }
    if (ok && !t.esds) t.skip = true;  // 不是 AAC（ALAC、AC-3 之类）：这条轨放不了，看下一条
    FreeBuf(buf);
    return ok;
}

// 表的条目从 r.pos 开始：记下位置，开头一个窗口顺手读进来（从头放的时候就不用再读）
bool SetupTable(Reader& r, const Box& b, uint32_t count, uint32_t entry, uint32_t cap, Mp4Table& t) {
    uint64_t room = (b.end - r.pos) / entry;
    if (count > room) count = static_cast<uint32_t>(room);  // 坏文件：别读出盒子外面
    t.offset = r.pos;
    t.count = count;
    t.entry = entry;
    t.cap = std::min(cap, count);
    t.first = 0;
    t.n = 0;
    if (count == 0) return true;
    t.buf = static_cast<uint8_t*>(AllocBuf(size_t(t.cap) * entry));
    if (!t.buf || !r.Read(t.buf, size_t(t.cap) * entry)) return false;
    t.n = t.cap;
    return true;
}

bool ReadStblChild(Reader& r, const Box& b, TrakInfo& t) {
    uint8_t h[12];
    uint64_t body = b.end - r.pos;
    if (Is(b, "stsd") && !t.esds) return ReadStsd(r, b, t);
    if (Is(b, "stts") && !t.have_stts) {
        if (body < 8 || !r.Read(h, 8)) return false;
        return (t.have_stts = SetupTable(r, b, U32(h + 4), 8, kSttsCap, t.stts));
    }
    if (Is(b, "stsc") && !t.have_stsc) {
        if (body < 8 || !r.Read(h, 8)) return false;
        return (t.have_stsc = SetupTable(r, b, U32(h + 4), 12, kStscCap, t.stsc));
    }
    if (Is(b, "stsz") && !t.have_stsz) {
        if (body < 12 || !r.Read(h, 12)) return false;
        t.stsz_const = U32(h + 4);
        t.stsz_count = U32(h + 8);
        if (t.stsz_const) return (t.have_stsz = true);  // 所有样本一样大：没有逐个的表
        return (t.have_stsz = SetupTable(r, b, t.stsz_count, 4, kStszCap, t.stsz));
    }
    if ((Is(b, "stco") || Is(b, "co64")) && !t.have_stco) {
        uint32_t entry = Is(b, "co64") ? 8 : 4;
        if (body < 8 || !r.Read(h, 8)) return false;
        return (t.have_stco = SetupTable(r, b, U32(h + 4), entry, kStcoBytes / entry, t.stco));
    }
    return true;  // 别的（stss、sgpd 之类）不用
}

// moov 里一条轨往下一层层找：level 0 在 trak 里，1 在 mdia 里（mdhd、hdlr），2 在 minf 里，3 在 stbl 里
bool Walk(Reader& r, uint64_t end, int level, TrakInfo& t) {
    while (!t.skip && r.pos + 8 <= end) {
        Box b;
        if (!ReadBox(r, end, b)) return false;
        bool ok = true;
        uint64_t body = b.end - r.pos;
        uint8_t h[32];
        if (level == 0 && Is(b, "mdia")) {
            ok = Walk(r, b.end, 1, t);
        } else if (level == 1 && Is(b, "mdhd")) {
            // version 0：creation 4、modification 4、timescale 4、duration 4；version 1 的时间都是 8 字节
            ok = body >= 20 && r.Read(h, 20);
            if (ok && h[0] == 1) ok = body >= 32 && r.Read(h + 20, 12);
            if (ok && h[0] == 1) {
                t.timescale = U32(h + 20);
                t.duration = U64(h + 24);
            } else if (ok) {
                t.timescale = U32(h + 12);
                t.duration = U32(h + 16) == UINT32_MAX ? 0 : U32(h + 16);
            }
        } else if (level == 1 && Is(b, "hdlr")) {
            ok = body >= 12 && r.Read(h, 12);
            t.audio = ok && memcmp(h + 8, "soun", 4) == 0;
            t.skip = ok && !t.audio;  // 不是音频轨：这条轨剩下的都跳过
        } else if (level == 1 && Is(b, "minf")) {
            ok = Walk(r, b.end, 2, t);
        } else if (level == 2 && Is(b, "stbl")) {
            ok = Walk(r, b.end, 3, t);
        } else if (level == 3) {
            ok = ReadStblChild(r, b, t);
        }
        if (!ok) return false;
        if (t.skip) return true;  // 外面一层层跳到轨的结尾
        if (!r.SkipTo(b.end)) return false;
    }
    return true;
}

}  // namespace

Mp4Track::~Mp4Track() {
    FreeTable(stts_);
    FreeTable(stsc_);
    FreeTable(stsz_);
    FreeTable(stco_);
}

bool Mp4Track::Open(Mp4Source& src) {
    src_ = &src;
    Reader r(src);
    bool found = false;
    if (r.Start(0)) {
        // 顶层：ftyp、moov、mdat……moov 在文件末尾（没做 faststart）时跳过 mdat 去那里
        for (int i = 0; i < 64 && !found; i++) {
            Box b;
            if (!ReadBox(r, Mp4Source::kToEnd, b)) break;
            if (!Is(b, "moov")) {
                if (!r.SkipTo(b.end)) break;
                continue;
            }
            while (!found && r.pos + 8 <= b.end) {
                Box trak;
                if (!ReadBox(r, b.end, trak)) break;
                if (Is(trak, "trak")) {
                    TrakInfo t;
                    if (!Walk(r, trak.end, 0, t)) break;
                    if (t.Usable()) {  // 第一条能放的 AAC 音频轨：表交给自己，后面的不用读了
                        found = true;
                        timescale_ = t.timescale;
                        mdhd_duration_ = t.duration;
                        profile_ = t.profile;
                        sf_index_ = t.sf_index;
                        channels_ = t.channels;
                        stsz_const_ = t.stsz_const;
                        stsz_count_ = t.stsz_count;
                        std::swap(stts_, t.stts);
                        std::swap(stsc_, t.stsc);
                        std::swap(stsz_, t.stsz);
                        std::swap(stco_, t.stco);
                        break;
                    }
                }
                if (!r.SkipTo(trak.end)) break;
            }
            break;  // 只有一个 moov
        }
    }
    src.Close();
    return found;
}

bool Mp4Track::Load(Mp4Table& t, uint32_t i) {
    if (i >= t.first && i - t.first < t.n) return true;
    if (i >= t.count || !src_ || !t.buf) return false;
    // 往前留一点：快退一小段不用再读
    uint32_t back = std::min(i, t.cap / 16);
    uint32_t first = i - back;
    uint32_t n = std::min(t.cap, t.count - first);
    t.n = 0;
    uint64_t from = t.offset + uint64_t(first) * t.entry;
    bool ok = src_->Open(from, from + uint64_t(n) * t.entry) && ReadFull(*src_, t.buf, size_t(n) * t.entry);
    src_->Close();
    fetches_++;
    if (!ok) return false;
    t.first = first;
    t.n = n;
    return true;
}

bool Mp4Track::ChunkOffset(uint32_t chunk, uint64_t& offset) {
    const uint8_t* p = At(stco_, chunk);
    if (!p) return false;
    offset = stco_.entry == 8 ? U64(p) : U32(p);
    return true;
}

bool Mp4Track::SampleSize(uint32_t sample, uint32_t& size) {
    if (stsz_const_) {
        size = stsz_const_;
        return sample < stsz_count_;
    }
    const uint8_t* p = At(stsz_, sample);
    if (!p) return false;
    size = U32(p);
    return true;
}

bool Mp4Track::StscEntry(uint32_t i, uint32_t& first_chunk, uint32_t& per) {
    const uint8_t* p = At(stsc_, i);
    if (!p) return false;
    first_chunk = U32(p);  // 从 1 数
    per = U32(p + 4);
    return true;
}

uint32_t Mp4Track::DurationSeconds() {
    if (timescale_ == 0) return 0;
    // stts 全在内存里（AAC 一般就一两条）就加起来；不然用 mdhd 里写的总长，免得为这个从头读一遍
    if (stts_.n == stts_.count || mdhd_duration_ == 0) {
        uint64_t total = 0;
        for (uint32_t i = 0; i < stts_.count; i++) {
            const uint8_t* p = At(stts_, i);
            if (!p) return static_cast<uint32_t>(mdhd_duration_ / timescale_);
            total += uint64_t(U32(p)) * U32(p + 4);
        }
        return static_cast<uint32_t>(total / timescale_);
    }
    return static_cast<uint32_t>(mdhd_duration_ / timescale_);
}

bool Mp4Track::SampleAtSecond(uint32_t second, uint32_t& sample) {
    uint64_t target = uint64_t(second) * timescale_, acc = 0;
    uint32_t base = 0;
    for (uint32_t i = 0; i < stts_.count; i++) {
        const uint8_t* p = At(stts_, i);
        if (!p) return false;
        uint32_t count = U32(p), delta = U32(p + 4);
        if (delta && target < acc + uint64_t(count) * delta) {
            sample = base + static_cast<uint32_t>((target - acc) / delta);
            return true;
        }
        acc += uint64_t(count) * delta;
        base += count;
    }
    sample = base;  // 超出结尾
    return true;
}

int Mp4Track::Seek(uint32_t sample, Mp4Cursor& c) {
    if (sample >= samples()) return 0;
    uint32_t fc, per;
    if (!StscEntry(0, fc, per)) return -1;
    uint64_t first = 0;  // 当前 stsc 条目覆盖的第一个样本
    for (uint32_t i = 0; i < stsc_.count; i++) {
        uint32_t next_fc = 0, next_per = 0, next_chunk = stco_.count;
        if (i + 1 < stsc_.count) {
            if (!StscEntry(i + 1, next_fc, next_per)) return -1;
            next_chunk = next_fc - 1;
        }
        uint32_t first_chunk = fc - 1;
        if (fc == 0 || next_chunk < first_chunk) return 0;  // 坏表：当放完了
        uint64_t n = uint64_t(next_chunk - first_chunk) * per;
        if (per && sample < first + n) {
            uint64_t k = sample - first;
            c = Mp4Cursor();
            c.chunk = first_chunk + static_cast<uint32_t>(k / per);
            c.in_chunk = static_cast<uint32_t>(k % per);
            c.per_chunk = per;
            c.stsc_i = i;
            c.sample = sample;
            if (c.chunk >= stco_.count) return 0;
            if (!ChunkOffset(c.chunk, c.offset)) return -1;
            for (uint32_t s = sample - c.in_chunk; s < sample; s++) {
                uint32_t size;
                if (!SampleSize(s, size)) return -1;
                c.offset += size;
            }
            return 1;
        }
        first += n;
        fc = next_fc;
        per = next_per;
    }
    return 0;
}

int Mp4Track::Sample(Mp4Cursor& c, uint32_t& size) {
    if (c.sample >= samples()) return 0;
    if (c.new_chunk) {
        for (;;) {
            if (c.chunk >= stco_.count) return 0;
            // 下一个 stsc 条目从这个块开始的话换过去（每块几个样本变了）
            while (c.stsc_i + 1 < stsc_.count) {
                uint32_t fc, per;
                if (!StscEntry(c.stsc_i + 1, fc, per)) return -1;
                if (fc > c.chunk + 1) break;
                c.stsc_i++;
                c.per_chunk = per;
            }
            if (c.per_chunk) break;
            c.chunk++;  // 空块
        }
        if (!ChunkOffset(c.chunk, c.offset)) return -1;
        c.new_chunk = false;
    }
    return SampleSize(c.sample, size) ? 1 : -1;
}

void Mp4Track::Advance(Mp4Cursor& c, uint32_t size) {
    c.sample++;
    c.offset += size;
    if (++c.in_chunk < c.per_chunk) return;
    c.chunk++;  // 下一个块：位置从块偏移表里取，用到时（Sample）再查
    c.in_chunk = 0;
    c.new_chunk = true;
}

void Mp4Track::AdtsHeader(uint32_t payload, uint8_t out[7]) const {
    uint32_t len = payload + 7;
    out[0] = 0xFF;
    out[1] = 0xF1;  // MPEG-4、无 CRC
    out[2] = static_cast<uint8_t>(((profile_ & 3) << 6) | ((sf_index_ & 0xF) << 2) | ((channels_ >> 2) & 1));
    out[3] = static_cast<uint8_t>(((channels_ & 3) << 6) | ((len >> 11) & 3));
    out[4] = static_cast<uint8_t>((len >> 3) & 0xFF);
    out[5] = static_cast<uint8_t>(((len & 7) << 5) | 0x1F);
    out[6] = 0xFC;
}

size_t Mp4Track::MemoryBytes() const {
    size_t total = 0;
    for (const Mp4Table* t : {&stts_, &stsc_, &stsz_, &stco_}) {
        if (t->buf) total += size_t(t->cap) * t->entry;
    }
    return total;
}
