#include "nomi_book.h"

#include <algorithm>
#include <cstring>
#include <strings.h>

#ifdef ESP_PLATFORM
#include "assets.h"
#endif

namespace {

constexpr size_t kGbkTableSize = 126 * 191 * 2;
constexpr size_t kDetectBytes = 32 * 1024;   // 认编码看开头多少
constexpr size_t kMaxTitleBytes = 160;       // 章名那行最长多少字节（再长就是正文）
constexpr size_t kMaxTitleChars = 40;

const uint8_t* s_gbk = nullptr;

const uint8_t* Gbk() {
#ifdef ESP_PLATFORM
    if (!s_gbk) {
        void* ptr = nullptr;
        size_t size = 0;
        if (Assets::GetInstance().GetAssetData("gbk.tab", ptr, size) && size == kGbkTableSize) {
            s_gbk = static_cast<const uint8_t*>(ptr);
        }
    }
#endif
    return s_gbk;
}

bool IsSpace(uint32_t cp) { return cp == ' ' || cp == '\t' || cp == 0x3000 || cp == 0xA0 || cp == 0xFEFF; }

void PutUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

int Utf8Len(uint8_t c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return c >= 0xC2 ? 2 : 0;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return c <= 0xF4 ? 4 : 0;
    return 0;
}

// 合法的 UTF-8（末尾被截断的半个字不算错）
bool LooksUtf8(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        int len = Utf8Len(p[i]);
        if (len == 0) return false;
        if (i + len > n) break;
        for (int k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
        }
        i += len;
    }
    return true;
}

std::vector<uint32_t> Codepoints(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        uint8_t c = s[i];
        int len = Utf8Len(c);
        if (len == 0 || i + len > s.size()) {
            out.push_back('?');
            i++;
            continue;
        }
        uint32_t v = len == 1 ? c : c & (0x7F >> len);
        for (int k = 1; k < len; k++) v = (v << 6) | (s[i + k] & 0x3F);
        out.push_back(v);
        i += len;
    }
    return out;
}

bool InSet(uint32_t cp, const std::vector<uint32_t>& set) { return std::find(set.begin(), set.end(), cp) != set.end(); }

bool StartsWith(const std::vector<uint32_t>& cps, const std::vector<uint32_t>& prefix) {
    return cps.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), cps.begin());
}

}  // namespace

void NomiBook::SetGbkTable(const uint8_t* table, size_t size) {
    if (size == kGbkTableSize) s_gbk = table;
}

NomiBook::~NomiBook() {
    if (f_) fclose(f_);
}

bool NomiBook::ReadAt(uint32_t pos, uint8_t* buf, size_t n, size_t& got) {
    got = 0;
    if (!f_ || fseek(f_, static_cast<long>(pos), SEEK_SET) != 0) return false;
    got = fread(buf, 1, n, f_);
    return got > 0 || n == 0;
}

bool NomiBook::Open(const std::string& path) {
    f_ = fopen(path.c_str(), "rb");
    if (!f_) return false;
    setvbuf(f_, nullptr, _IOFBF, 4096);
    if (fseek(f_, 0, SEEK_END) != 0) return false;
    long end = ftell(f_);
    if (end <= 0) return false;
    size_ = static_cast<uint32_t>(end);
    std::vector<uint8_t> head(std::min<size_t>(size_, kDetectBytes));
    size_t got;
    if (!ReadAt(0, head.data(), head.size(), got)) return false;
    head.resize(got);
    if (got >= 2 && ((head[0] == 0xFF && head[1] == 0xFE) || (head[0] == 0xFE && head[1] == 0xFF))) {
        return false;  // UTF-16：不认
    }
    if (got >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) {
        enc_ = kUtf8;
        body_ = 3;
    } else {
        enc_ = LooksUtf8(head.data(), head.size()) ? kUtf8 : kGbk;
    }
    return true;
}

size_t NomiBook::DecodeChar(const uint8_t* p, size_t avail, uint32_t& cp) const {
    uint8_t c = p[0];
    if (c < 0x80) {
        cp = c;
        return 1;
    }
    if (enc_ == kUtf8) {
        int len = Utf8Len(c);
        if (len == 0) {
            cp = '?';
            return 1;
        }
        if (avail < static_cast<size_t>(len)) return 0;
        uint32_t v = c & (0x7F >> len);
        for (int k = 1; k < len; k++) {
            if ((p[k] & 0xC0) != 0x80) {
                cp = '?';
                return 1;
            }
            v = (v << 6) | (p[k] & 0x3F);
        }
        cp = v;
        return len;
    }
    if (c == 0x80 || c == 0xFF) {
        cp = '?';
        return 1;
    }
    if (avail < 2) return 0;
    uint8_t t = p[1];
    if (t < 0x40 || t == 0x7F || t == 0xFF) {
        cp = '?';
        return 1;
    }
    uint32_t v = 0;
    if (const uint8_t* tab = Gbk()) {
        size_t i = (static_cast<size_t>(c - 0x81) * 191 + (t - 0x40)) * 2;
        v = tab[i] | (tab[i + 1] << 8);
    }
    cp = v ? v : '?';
    return 2;
}

// pos 是不是段落开头：往前只有空白，再往前是换行（或文件开头）
bool NomiBook::AtParagraphStart(uint32_t pos) {
    if (pos <= body_) return true;
    uint8_t buf[64];
    uint32_t n = std::min<uint32_t>(sizeof(buf), pos - body_);
    size_t got;
    if (!ReadAt(pos - n, buf, n, got) || got != n) return true;
    size_t i = n;
    while (i > 0) {
        uint8_t b = buf[i - 1];
        if (b == '\n') return true;
        if (b == ' ' || b == '\t' || b == '\r') {  // 这几个不会是 GBK 的第二个字节（都小于 0x40）
            i--;
        } else if (enc_ == kUtf8 && i >= 3 && buf[i - 3] == 0xE3 && buf[i - 2] == 0x80 && buf[i - 1] == 0x80) {
            i -= 3;  // 全角空格
        } else if (enc_ == kGbk && i >= 2 && buf[i - 2] == 0xA1 && buf[i - 1] == 0xA1) {
            i -= 2;
        } else {
            return false;
        }
    }
    return true;
}

bool NomiBook::Decode(uint32_t from, uint32_t to, std::string& text, std::vector<uint32_t>& src) {
    text.clear();
    src.clear();
    from = std::max(from, body_);
    to = std::min(to, size_);
    if (from >= to) {
        src.push_back(std::max(from, to));
        return true;
    }
    std::vector<uint8_t> raw(to - from);
    size_t got;
    if (!ReadAt(from, raw.data(), raw.size(), got)) return false;
    raw.resize(got);

    struct Ch {
        uint32_t cp, at;
    };
    std::vector<Ch> line;
    std::string tmp;
    // 一行收齐了再决定：空行不要；段落开头去掉开头空白、缩进两格（章名不缩进）；完整的行去掉行尾空白
    auto flush = [&](bool para, bool complete, uint32_t newline_at) {
        size_t b = 0, e = line.size();
        if (para) {
            while (b < e && IsSpace(line[b].cp)) b++;
        }
        if (complete) {
            while (e > b && IsSpace(line[e - 1].cp)) e--;
        }
        if (b < e) {
            bool indent = para;
            if (indent && e - b <= kMaxTitleChars) {
                tmp.clear();
                for (size_t k = b; k < e; k++) PutUtf8(tmp, line[k].cp);
                indent = !IsChapterTitle(tmp);
            }
            if (indent) {
                for (int k = 0; k < 2; k++) {
                    PutUtf8(text, 0x3000);
                    src.resize(text.size(), line[b].at);
                }
            }
            for (size_t k = b; k < e; k++) {
                PutUtf8(text, line[k].cp);
                src.resize(text.size(), line[k].at);
            }
            if (complete) {
                text += '\n';
                src.push_back(newline_at);
            }
        }
        line.clear();
    };

    bool para = AtParagraphStart(from);  // 书签可能落在段落中间：那一行不缩进
    size_t i = 0;
    while (i < raw.size()) {
        uint32_t cp;
        size_t n = DecodeChar(&raw[i], raw.size() - i, cp);
        if (n == 0) break;  // 末尾半个字
        uint32_t at = from + static_cast<uint32_t>(i);
        i += n;
        if (cp == '\r') continue;
        if (cp == '\n') {
            flush(para, true, at);
            para = true;
            continue;
        }
        if (cp == '\t') cp = ' ';
        line.push_back({cp, at});
    }
    flush(para, false, 0);
    src.push_back(from + static_cast<uint32_t>(i));
    return true;
}

uint32_t NomiBook::AlignChar(uint32_t pos) {
    if (pos <= body_) return body_;
    if (pos >= size_) return size_;
    if (enc_ != kUtf8) return pos;  // GBK 从中间看不出字的边界：位置都是排版给的，本来就对齐
    uint8_t buf[4];
    size_t got;
    if (!ReadAt(pos, buf, sizeof(buf), got)) return pos;
    size_t k = 0;
    while (k < got && (buf[k] & 0xC0) == 0x80) k++;
    return pos + static_cast<uint32_t>(k);
}

uint32_t NomiBook::ParagraphStartBefore(uint32_t pos, uint32_t back) {
    if (pos <= body_ || pos - body_ <= back) return body_;
    uint32_t target = pos - back;
    uint32_t limit = target - body_ > 65536 ? target - 65536 : body_;
    std::vector<uint8_t> buf(2048);
    for (uint32_t end = target; end > limit;) {
        uint32_t start = end - limit > buf.size() ? end - static_cast<uint32_t>(buf.size()) : limit;
        size_t got;
        if (!ReadAt(start, buf.data(), end - start, got)) break;
        for (size_t k = got; k > 0; k--) {
            if (buf[k - 1] == '\n') return start + static_cast<uint32_t>(k);
        }
        end = start;
    }
    return limit == body_ ? body_ : AlignChar(target);  // 64KB 一段都没换行：只好从中间开始
}

bool NomiBook::IsChapterTitle(const std::string& line) {
    if (line.empty() || line.size() > kMaxTitleBytes) return false;
    if (line.size() >= 9 && strncasecmp(line.c_str(), "chapter", 7) == 0 && line[7] == ' ' &&
        line[8] >= '0' && line[8] <= '9') {
        return true;
    }
    std::vector<uint32_t> cps = Codepoints(line);
    if (cps.size() > kMaxTitleChars) return false;
    static const std::vector<std::vector<uint32_t>> kSpecial = [] {
        std::vector<std::vector<uint32_t>> v;
        for (const char* s : {"序章", "序言", "楔子", "引子", "前言", "尾声", "尾聲", "后记", "後記", "番外", "终章", "終章",
                              "结局", "結局", "完本感言"}) {
            v.push_back(Codepoints(s));
        }
        return v;
    }();
    if (cps.size() <= 30) {
        for (const auto& s : kSpecial) {
            if (StartsWith(cps, s)) return true;
        }
    }
    if (cps[0] != 0x7B2C) return false;  // 第
    static const std::vector<uint32_t> kNum =
        Codepoints("0123456789０１２３４５６７８９零〇一二三四五六七八九十百千万萬两兩壹贰貳叁參肆伍陆陸柒捌玖拾佰仟");
    static const std::vector<uint32_t> kUnit = Codepoints("章回节節卷集部篇幕话話册冊");
    static const std::vector<uint32_t> kSep = Codepoints(" 　\t:：、.．-—–_·(（[【《");
    size_t i = 1, digits = 0;
    while (i < cps.size() && (cps[i] == ' ' || cps[i] == 0x3000)) i++;
    while (i < cps.size() && digits < 12 && InSet(cps[i], kNum)) {
        i++;
        digits++;
    }
    while (i < cps.size() && (cps[i] == ' ' || cps[i] == 0x3000)) i++;
    if (digits == 0 || i >= cps.size() || !InSet(cps[i], kUnit)) return false;
    i++;
    if (i == cps.size() || InSet(cps[i], kSep)) return true;
    // 「第」「回」后面紧跟着字：短、没有句子里的标点才算（「第三回合他赢了。」不算）
    if (cps.size() > 30) return false;
    for (uint32_t c : cps) {
        if (c == 0x3002 || c == 0xFF0C || c == 0xFF01 || c == 0xFF1F || c == 0xFF1B) return false;  // 。，！？；
    }
    return true;
}

bool NomiBook::ScanChapters(std::vector<Chapter>& out) {
    out.clear();
    if (!f_) return false;
    std::vector<uint8_t> buf(32 * 1024);
    std::vector<uint8_t> line;
    bool too_long = false;
    uint32_t line_start = body_;
    std::string title;
    auto check = [&]() {
        if (too_long || line.empty()) return;
        // 解出来、去掉首尾空白、中间连着的空白并成一个
        title.clear();
        size_t i = 0, lead = 0;
        bool started = false, space = false;
        while (i < line.size()) {
            uint32_t cp;
            size_t n = DecodeChar(&line[i], line.size() - i, cp);
            if (n == 0) break;
            if (cp == '\r' || IsSpace(cp)) {
                if (!started) lead += n;
                space = started;
            } else {
                if (space) title += ' ';
                space = false;
                started = true;
                PutUtf8(title, cp);
            }
            i += n;
        }
        if (started && IsChapterTitle(title)) out.push_back({line_start + static_cast<uint32_t>(lead), title});
    };
    for (uint32_t pos = body_; pos < size_;) {
        size_t got;
        if (!ReadAt(pos, buf.data(), std::min<size_t>(buf.size(), size_ - pos), got) || got == 0) return false;
        for (size_t k = 0; k < got; k++) {
            uint8_t b = buf[k];
            if (b == '\n') {
                check();
                line.clear();
                too_long = false;
                line_start = pos + static_cast<uint32_t>(k) + 1;
            } else if (!too_long) {
                if (line.size() < kMaxTitleBytes + 16) line.push_back(b);
                else too_long = true;
            }
        }
        pos += static_cast<uint32_t>(got);
    }
    check();
    return true;
}
