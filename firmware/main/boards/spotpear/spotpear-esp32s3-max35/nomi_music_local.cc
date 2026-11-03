// NomiMusic 里应用包放的歌：读标签 / 封面 / 歌词，正在放的这首的显示。见 nomi_music.h
#include "nomi_music.h"

#include "nomi_radio.h"
#include "nomi_sdcard.h"
#include "nomi_util.h"

#include <dirent.h>
#include <esp_log.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#define TAG "NomiMusic"

namespace {

constexpr size_t kMaxCover = 512 * 1024;  // 内嵌封面再大就不要了（解码也吃不消）
constexpr size_t kMaxLrc = 64 * 1024;
constexpr size_t kMaxLyrics = 300;

uint32_t Be32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

std::string Stem(const std::string& path) {
    size_t slash = path.rfind('/'), dot = path.rfind('.');
    size_t b = slash == std::string::npos ? 0 : slash + 1;
    return path.substr(b, dot != std::string::npos && dot > b ? dot - b : std::string::npos);
}

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

// ID3 文字帧：第一个字节是编码。0 是 Latin-1（中文歌这里常是 GBK，认不出就不要）、1 带 BOM 的 UTF-16、
// 2 UTF-16BE、3 UTF-8
std::string Id3Text(const uint8_t* p, size_t n) {
    if (n < 2) return "";
    uint8_t enc = p[0];
    p++, n--;
    std::string out;
    if (enc == 1 || enc == 2) {
        bool be = enc == 2;
        if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE) be = false, p += 2, n -= 2;
        else if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF) be = true, p += 2, n -= 2;
        for (size_t i = 0; i + 1 < n; i += 2) {
            uint32_t u = be ? (p[i] << 8) | p[i + 1] : p[i] | (p[i + 1] << 8);
            if (u == 0) break;
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {  // 代理对
                uint32_t lo = be ? (p[i + 2] << 8) | p[i + 3] : p[i + 2] | (p[i + 3] << 8);
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            PutUtf8(out, u);
        }
    } else {
        out.assign(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), n));
        if (enc == 0 && !std::all_of(out.begin(), out.end(), [](char c) { return (c & 0x80) == 0; })) return "";
    }
    if (!nomi::ValidUtf8(out)) return "";
    while (!out.empty() && (out.back() == ' ' || out.back() == '\0')) out.pop_back();
    return out;
}

// FLAC：元数据块里的 STREAMINFO（时长）、VORBIS_COMMENT（TITLE / ARTIST）、PICTURE（封面）
void ReadFlac(FILE* f, int64_t size, NomiMusic::Track& t, int& duration, std::string* cover) {
    int64_t pos = 4;
    for (bool last = false; !last && pos + 4 <= size;) {
        uint8_t bh[4];
        fseek(f, pos, SEEK_SET);
        if (fread(bh, 1, 4, f) != 4) return;
        last = bh[0] & 0x80;
        int type = bh[0] & 0x7F;
        uint32_t len = (bh[1] << 16) | (bh[2] << 8) | bh[3];
        if (type == 0 && len >= 18) {
            uint8_t si[18];
            if (fread(si, 1, 18, f) == 18) {
                uint32_t rate = (si[10] << 12) | (si[11] << 4) | (si[12] >> 4);
                uint64_t total = (static_cast<uint64_t>(si[13] & 0x0F) << 32) | Be32(si + 14);
                if (rate) duration = total / rate;
            }
        } else if (type == 4 && len < 256 * 1024) {
            std::string block(len, '\0');
            if (fread(block.data(), 1, len, f) == len) {
                const auto* b = reinterpret_cast<const uint8_t*>(block.data());
                size_t p = 4 + Le32(b);  // 跳过 vendor
                uint32_t count = p + 4 <= len ? Le32(b + p) : 0;
                p += 4;
                for (uint32_t i = 0; i < count && p + 4 <= len; i++) {
                    uint32_t n = Le32(b + p);
                    p += 4;
                    if (p + n > len) break;
                    std::string c(block, p, n);
                    p += n;
                    size_t eq = c.find('=');
                    if (eq == std::string::npos) continue;
                    std::string key = c.substr(0, eq), value = c.substr(eq + 1);
                    if (!strcasecmp(key.c_str(), "TITLE") && !value.empty()) t.title = value;
                    else if (!strcasecmp(key.c_str(), "ARTIST") && !value.empty()) t.artist = value;
                }
            }
        } else if (type == 6 && cover && cover->empty() && len < kMaxCover + 1024) {
            std::string block(len, '\0');
            if (fread(block.data(), 1, len, f) == len) {
                const auto* b = reinterpret_cast<const uint8_t*>(block.data());
                size_t p = 4;
                uint32_t mime_len = Be32(b + p);
                std::string mime(block, p + 4, std::min<size_t>(mime_len, len - p - 4));
                p += 4 + mime_len;
                if (p + 4 <= len) p += 4 + Be32(b + p);  // 描述
                p += 16;                                  // 宽、高、色深、调色板
                if (p + 4 <= len) {
                    uint32_t n = Be32(b + p);
                    if (p + 4 + n <= len && mime.find("jpeg") != std::string::npos) cover->assign(block, p + 4, n);
                }
            }
        }
        pos += 4 + len;
    }
}

// MP3：ID3v2 标签里的 TIT2 / TPE1 / APIC；时长按第一帧的码率估
void ReadMp3(FILE* f, int64_t size, NomiMusic::Track& t, int& duration, std::string* cover) {
    uint8_t h[10];
    fseek(f, 0, SEEK_SET);
    int64_t audio = 0;
    if (fread(h, 1, 10, f) == 10 && !memcmp(h, "ID3", 3)) {
        int version = h[3];
        uint32_t tag = (h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F);
        audio = 10 + tag + ((h[5] & 0x10) ? 10 : 0);
        for (int64_t pos = 10; pos + 10 <= 10 + static_cast<int64_t>(tag);) {
            uint8_t fh[10];
            fseek(f, pos, SEEK_SET);
            if (fread(fh, 1, 10, f) != 10 || fh[0] == 0) break;
            uint32_t n = version >= 4 ? ((fh[4] & 0x7F) << 21 | (fh[5] & 0x7F) << 14 | (fh[6] & 0x7F) << 7 | (fh[7] & 0x7F))
                                      : Be32(fh + 4);
            if (n == 0 || pos + 10 + n > 10 + static_cast<int64_t>(tag)) break;
            bool title = !memcmp(fh, "TIT2", 4), artist = !memcmp(fh, "TPE1", 4), pic = !memcmp(fh, "APIC", 4);
            if ((title || artist) && n < 1024) {
                std::string data(n, '\0');
                if (fread(data.data(), 1, n, f) == n) {
                    std::string text = Id3Text(reinterpret_cast<const uint8_t*>(data.data()), n);
                    if (!text.empty()) (title ? t.title : t.artist) = text;
                }
            } else if (pic && cover && cover->empty() && n < kMaxCover) {
                std::string data(n, '\0');
                if (fread(data.data(), 1, n, f) == n) {
                    // 编码、MIME（0 结尾）、图片类型、描述（按编码 0 结尾）、图片
                    uint8_t enc = data[0];
                    size_t p = data.find('\0', 1);
                    if (p != std::string::npos) {
                        p += 2;  // MIME 的 0 + 图片类型
                        if (enc == 1 || enc == 2) {
                            while (p + 1 < n && (data[p] || data[p + 1])) p += 2;
                            p += 2;
                        } else {
                            p = data.find('\0', p);
                            if (p != std::string::npos) p++;
                        }
                        if (p != std::string::npos && p + 3 < n && (uint8_t)data[p] == 0xFF && (uint8_t)data[p + 1] == 0xD8) {
                            cover->assign(data, p, n - p);
                        }
                    }
                }
            }
            pos += 10 + n;
        }
    }
    static const int v1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int v2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    uint8_t fh[4] = {};
    fseek(f, audio, SEEK_SET);
    fread(fh, 1, 4, f);
    int kbps = fh[0] == 0xFF && (fh[1] & 0xE0) == 0xE0 ? (((fh[1] >> 3) & 3) == 3 ? v1 : v2)[fh[2] >> 4] : 0;
    duration = (size - audio) * 8 / ((kbps > 0 ? kbps : 128) * 1000);
}

}  // namespace

bool NomiMusic::IsAudioFile(const std::string& name) {
    size_t dot = name.rfind('.');
    if (dot == std::string::npos || name[0] == '.') return false;
    std::string ext = name.substr(dot + 1);
    for (const char* e : {"mp3", "flac", "wav", "aac"}) {
        if (!strcasecmp(ext.c_str(), e)) return true;
    }
    return false;
}

void NomiMusic::ReadInfo(const std::string& path, Track& t, int& duration, std::string* cover) {
    t.path = path;
    t.title = Stem(path);
    t.artist.clear();
    duration = 0;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    int64_t size = ftell(f);
    uint8_t head[12] = {};
    fseek(f, 0, SEEK_SET);
    size_t hn = fread(head, 1, sizeof(head), f);
    std::string title = t.title;
    t.title.clear();
    if (hn >= 4 && !memcmp(head, "fLaC", 4)) {
        ReadFlac(f, size, t, duration, cover);
    } else if (hn >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4)) {
        // WAV：fmt 里的字节率、data 的长度
        uint32_t byte_rate = 0;
        for (int64_t pos = 12; pos + 8 <= size;) {
            uint8_t ch[8], fmt[16];
            fseek(f, pos, SEEK_SET);
            if (fread(ch, 1, 8, f) != 8) break;
            uint32_t len = Le32(ch + 4);
            if (!memcmp(ch, "fmt ", 4) && fread(fmt, 1, 16, f) == 16) byte_rate = Le32(fmt + 8);
            if (!memcmp(ch, "data", 4)) {
                if (byte_rate) duration = std::min<int64_t>(len, size - pos - 8) / byte_rate;
                break;
            }
            pos += 8 + len + (len & 1);
        }
    } else {
        ReadMp3(f, size, t, duration, cover);
    }
    fclose(f);
    if (t.title.empty()) t.title = title;  // 没有标签：用文件名
}

void NomiMusic::ShowLocal(const std::string& path) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        song_url_ = path;  // 歌词、封面对得上的是这首
        lyrics_.clear();
        lines_.clear();
    }
    Post([this, path]() { LoadLocalExtras(path); });
}

void NomiMusic::ShowRemote(const std::string& url, const std::string& cover_url, const std::string& lrc) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        song_url_ = url;
        lyrics_.clear();
        lines_.clear();
    }
    SetLyrics(url, lrc);
    Post([this, cover_url]() { LoadCover(cover_url); });
}

// LRC：[mm:ss.xx]一句，一行可以有好几个时间；不是 UTF-8 的（GBK）认不出，宁可不显示也别显示乱码
void NomiMusic::SetLyrics(const std::string& url, const std::string& lrc) {
    std::string text = lrc;
    if (text.size() >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) {
        text.erase(0, 3);  // UTF-8 的 BOM
    }
    std::vector<std::pair<int, std::string>> found;
    if (nomi::ValidUtf8(text)) {
        for (size_t pos = 0; pos < text.size() && found.size() < kMaxLyrics;) {
            size_t end = text.find('\n', pos);
            std::string line = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            pos = end == std::string::npos ? text.size() : end + 1;
            std::vector<int> times;
            size_t p = 0;
            while (p < line.size() && line[p] == '[') {
                size_t close = line.find(']', p);
                if (close == std::string::npos) break;
                int mm, ss;
                if (sscanf(line.c_str() + p + 1, "%d:%d", &mm, &ss) == 2) times.push_back(mm * 60 + ss);
                p = close + 1;
            }
            std::string words = line.substr(p);
            while (!words.empty() && (words.back() == '\r' || words.back() == ' ')) words.pop_back();
            for (int t : times) found.push_back({t, words});
        }
        std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    }
    std::string lyrics;
    std::vector<Lyric> lines;
    for (const auto& [time, words] : found) {
        lines.push_back({time, static_cast<uint32_t>(lyrics.size()), static_cast<uint32_t>(words.size())});
        lyrics += words;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (song_url_ == url) {
        lyrics_ = std::move(lyrics);
        lines_ = std::move(lines);
    }
}

// 后台任务里：读标签（换掉文件名当的标题）、内嵌封面、同名 .lrc 歌词
void NomiMusic::LoadLocalExtras(const std::string& path) {
    NomiSdCard::Use sd;
    if (!sd) return;
    Track t;
    int duration = 0;
    std::string jpg;
    ReadInfo(path, t, duration, &jpg);
    NomiRadio::GetInstance().UpdateMeta(path, t.title, t.artist);
    std::string lrc;
    if (FILE* f = fopen((path.substr(0, path.rfind('.')) + ".lrc").c_str(), "rb")) {
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && lrc.size() < kMaxLrc) lrc.append(buf, n);
        fclose(f);
    }
    SetLyrics(path, lrc);
    cover_url_ = path;
    SetCover(jpg);
    ESP_LOGI(TAG, "%s - %s, %d s, cover %u bytes, lrc %u bytes", t.title.c_str(), t.artist.c_str(), duration,
             static_cast<unsigned>(jpg.size()), static_cast<unsigned>(lrc.size()));
}
