#include "nomi_music.h"

#include "application.h"
#include "mcp_server.h"
#include "nomi_radio.h"
#include "nomi_util.h"
#include "settings.h"
#if CONFIG_DIRECT_API
#include "direct_api_protocol.h"
#endif

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>

#include <cstdlib>
#include <cstring>

#define TAG "NomiMusic"

namespace {

constexpr const char* kNs = "nomi_music";
constexpr size_t kMaxResults = 10;
constexpr int kTries = 3;           // 一首放不了（下架、地址失效）就试搜索结果里的下一首，最多试这么多
constexpr size_t kMaxLyrics = 300;  // 歌词最多记这么多句

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// text 里 from 之后 begin 和 end 之间的文字（解开 HTML 实体、去掉首尾空白）；找不到为空
std::string Between(const std::string& text, const char* begin, const char* end, size_t from = 0) {
    size_t b = text.find(begin, from);
    if (b == std::string::npos) return "";
    b += strlen(begin);
    size_t e = text.find(end, b);
    if (e == std::string::npos) return "";
    return Trim(nomi::XmlUnescape(text.substr(b, e - b)));
}

// 音频、封面的候选地址：https 的先试 http（同一个 CDN 一般两种都给，http 不占 TLS 的内部 RAM）
std::vector<std::string> Candidates(const std::string& url) {
    if (url.rfind("https://", 0) != 0) return {url};
    return {"http://" + url.substr(8), url};
}

// 正在放的若是 https，先让出来（内部 RAM 只够一条 TLS）
struct HttpsHold {
    bool held;
    explicit HttpsHold(const std::string& url)
        : held(url.rfind("https://", 0) == 0 && NomiRadio::GetInstance().HoldForHttps()) {}
    ~HttpsHold() {
        if (held) NomiRadio::GetInstance().ReleaseHttpsHold();
    }
};

// 按文件头认格式：扩展名、响应头常对不上（m4a 写成 .aac、类型是 octet-stream）。不是音频返回 kAuto
NomiRadio::Station::Format Sniff(const std::string& head) {
    if (head.size() < 12) return NomiRadio::Station::kAuto;
    const auto* b = reinterpret_cast<const uint8_t*>(head.data());
    if (memcmp(b + 4, "ftyp", 4) == 0) return NomiRadio::Station::kMp4;
    bool sync = b[0] == 0xFF && (b[1] & 0xE0) == 0xE0;
    if (memcmp(b, "ID3", 3) == 0 || (sync && (b[1] & 0x06) != 0)) return NomiRadio::Station::kMp3;
    if (sync) return NomiRadio::Station::kAdts;  // 帧头里 layer 为 0 的是 ADTS
    return NomiRadio::Station::kAuto;
}

}  // namespace

NomiMusic& NomiMusic::GetInstance() {
    static NomiMusic instance;
    return instance;
}

void NomiMusic::Start() {
    site_ = Settings(kNs).GetString("site");
    for (auto& c : covers_) c = static_cast<uint16_t*>(heap_caps_calloc(kCoverSize * kCoverSize, 2, MALLOC_CAP_SPIRAM));
    jobs_sem_ = xSemaphoreCreateCounting(8, 0);
    // 下封面的后台任务（http 不行时退到 https：TLS 握手和 JPEG 解码都吃栈），栈放 PSRAM
    xTaskCreatePinnedToCoreWithCaps([](void* arg) { static_cast<NomiMusic*>(arg)->WorkerLoop(); }, "music", 12288,
                                    this, 2, nullptr, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
}

void NomiMusic::Post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        jobs_.push_back(std::move(job));
    }
    xSemaphoreGive(jobs_sem_);
}

void NomiMusic::WorkerLoop() {
    for (;;) {
        xSemaphoreTake(jobs_sem_, portMAX_DELAY);
        std::function<void()> job;
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            if (jobs_.empty()) continue;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job();
    }
}

std::string NomiMusic::Site() {
    std::lock_guard<std::mutex> lock(mutex_);
    return site_;
}

bool NomiMusic::Get(const std::string& url, std::string& body, size_t max_bytes) {
    std::lock_guard<std::mutex> lock(net_mutex_);
    HttpsHold hold(url);
    return nomi::HttpGet(url, body, max_bytes);
}

/* ---------- 搜索、取歌曲页 ---------- */

bool NomiMusic::Search(const std::string& query, std::vector<Song>& out) {
    std::string body;
    if (!Get(Site() + "/so/" + nomi::UrlEncode(query) + "/", body, 256 * 1024)) return false;
    // 每条结果：<div class="result-item" data-rid="…"> 里有 result-title、result-artist
    for (size_t pos = body.find("data-rid=\""); pos != std::string::npos && out.size() < kMaxResults;) {
        size_t next = body.find("data-rid=\"", pos + 10);
        std::string item = body.substr(pos, (next == std::string::npos ? body.size() : next) - pos);
        Song s{Between(item, "data-rid=\"", "\""), Between(item, "result-title\">", "<"),
               Between(item, "result-artist\">", "<")};
        if (!s.id.empty() && !s.title.empty()) out.push_back(std::move(s));
        pos = next;
    }
    // 一条都没有：是这种网站的搜索页就是真没搜到，不是（换了格式、跳到了别处）就当网站不能用
    return !out.empty() || body.find("search-results") != std::string::npos;
}

std::string NomiMusic::Play(const Song& song) {
    std::string page;
    if (!Get(Site() + "/song/" + nomi::UrlEncode(song.id) + "/", page, 256 * 1024)) return "!歌曲页打不开";

    // 音频地址：页面脚本里的 let code = "…"，base64
    std::string code = Between(page, "let code = \"", "\"");
    std::string url(code.size(), '\0');
    size_t n = 0;
    if (code.empty() || mbedtls_base64_decode(reinterpret_cast<unsigned char*>(url.data()), url.size(), &n,
                                              reinterpret_cast<const unsigned char*>(code.data()), code.size()) != 0) {
        return "!歌曲页里没找到音频地址（网站可能换了格式）";
    }
    url.resize(n);
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return "!音频地址不对";

    // 先探一下：能走 http 就走 http；文件头不是音频（下架了跳到网页之类）就算放不了
    auto format = NomiRadio::Station::kAuto;
    std::string play_url;
    for (const auto& u : Candidates(url)) {
        std::string head;
        if (Get(u, head, 16) && (format = Sniff(head)) != NomiRadio::Station::kAuto) {
            play_url = u;
            break;
        }
    }
    if (play_url.empty()) return "!这首的音频打不开";

    // 歌词：<div class="lyric-line" data-time="12">…</div>，时间是秒
    std::string lyrics;
    std::vector<Lyric> lines;
    for (size_t pos = page.find("lyrics-container");
         pos != std::string::npos && (pos = page.find("data-time=\"", pos)) != std::string::npos &&
         lines.size() < kMaxLyrics;) {
        pos += 11;
        std::string text = Between(page, ">", "</div>", pos);
        if (text.empty()) continue;
        lines.push_back({atoi(page.c_str() + pos), static_cast<uint32_t>(lyrics.size()), static_cast<uint32_t>(text.size())});
        lyrics += text;
    }
    size_t line_count = lines.size();

    // 封面：<img id="album-cover" … src="…">
    std::string cover;
    size_t img = page.find("id=\"album-cover\"");
    if (img != std::string::npos) {
        size_t tag = page.rfind('<', img), end = page.find('>', img);
        if (tag != std::string::npos && end != std::string::npos) {
            cover = Between(page.substr(tag, end - tag), "src=\"", "\"");
        }
    }

    NomiRadio::Station st;
    st.name = song.title;
    st.podcast = song.artist;
    st.url = play_url;
    st.episode = true;
    st.song = true;
    st.format = format;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        song_url_ = play_url;
        lyrics_ = std::move(lyrics);
        lines_ = std::move(lines);
    }
    NomiRadio::GetInstance().PlayEpisode(st);
    Post([this, cover]() { LoadCover(cover); });
    ESP_LOGI(TAG, "%s - %s: %s, %u lyric lines", song.title.c_str(), song.artist.c_str(),
             format == NomiRadio::Station::kMp4 ? "m4a" : format == NomiRadio::Station::kAdts ? "aac" : "mp3",
             static_cast<unsigned>(line_count));
    return "好的，放《" + song.title + "》" + (song.artist.empty() ? std::string() : " - " + song.artist);
}

void NomiMusic::LoadCover(const std::string& art) {
    if (art == cover_url_) return;
    cover_url_ = art;
    // 封面地址里带尺寸（…/albumcover/300/…）时换成 120 的小图：88 见方显示够了，下载、解码都省
    std::string url = art;
    size_t p = url.find("/albumcover/300/");
    if (p != std::string::npos) url.replace(p, 16, "/albumcover/120/");
    bool ok = false;
    std::string jpg;
    for (const auto& u : url.empty() ? std::vector<std::string>() : Candidates(url)) {
        if ((ok = Get(u, jpg, 256 * 1024))) break;
    }
    if (!ok) jpg.clear();
    if (!ok && !url.empty()) ESP_LOGW(TAG, "Cover failed: %s", url.c_str());
    SetCover(jpg);
}

void NomiMusic::SetCover(const std::string& jpg) {
    int next = cover_active_ == 0 ? 1 : 0;
    if (!jpg.empty() && nomi::DecodeCover(jpg, covers_[next], kCoverSize)) cover_active_ = next;
    else cover_active_ = -1;  // 这首没有封面：别还显示上一首的
    cover_version_++;
}

/* ---------- 给界面 ---------- */

bool NomiMusic::Playing() { return NomiRadio::GetInstance().Current().song; }

std::string NomiMusic::LyricsWindow() {
    auto& radio = NomiRadio::GetInstance();
    std::string url = radio.Current().url;
    int position = radio.Position();
    std::lock_guard<std::mutex> lock(mutex_);
    if (lines_.empty() || url != song_url_) return "";
    int current = -1;
    for (int i = 0; i < static_cast<int>(lines_.size()) && lines_[i].time <= position; i++) current = i;
    std::string out;
    for (int i = current - 3; i <= current + 3; i++) {
        if (i > current - 3) out += '\n';
        if (i >= 0 && i < static_cast<int>(lines_.size())) out.append(lyrics_, lines_[i].start, lines_[i].len);
    }
    return out;
}

std::string NomiMusic::Hint() {
    return Site().empty() ? "还没设置音乐网站：在网页设置页的「在线音乐」里填一个"
                          : "跟它说「放一首晴天」「来首周杰伦的歌」，这里显示歌词";
}

const uint16_t* NomiMusic::Cover(int& version) {
    version = cover_version_;
    int active = cover_active_;
    return active < 0 ? nullptr : covers_[active];
}

/* ---------- 网页设置页 ---------- */

std::string NomiMusic::InfoJson() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "site", Site().c_str());
    char* json = cJSON_PrintUnformatted(root);
    std::string out = json ? json : "{}";
    cJSON_free(json);
    cJSON_Delete(root);
    return out;
}

std::string NomiMusic::SetSiteJson(const std::string& body) {
    cJSON* req = cJSON_Parse(body.c_str());
    cJSON* v = cJSON_GetObjectItem(req, "site");
    std::string site = cJSON_IsString(v) ? Trim(v->valuestring) : "";
    cJSON_Delete(req);
    if (!site.empty() && site.find("://") == std::string::npos) site = "http://" + site;  // 只填了域名
    while (!site.empty() && site.back() == '/') site.pop_back();
    if (!site.empty() && site.rfind("http://", 0) != 0 && site.rfind("https://", 0) != 0) {
        return "!网址要以 http:// 或 https:// 开头";
    }
    if (site.size() > 200) return "!网址太长";
    Settings(kNs, true).SetString("site", site);
    std::lock_guard<std::mutex> lock(mutex_);
    site_ = site;
    results_.clear();
    result_index_ = -1;
    return "{\"ok\":true}";
}

std::string NomiMusic::SearchJson(const std::string& body) {
    cJSON* req = cJSON_Parse(body.c_str());
    cJSON* q = cJSON_GetObjectItem(req, "q");
    std::string query = cJSON_IsString(q) ? Trim(q->valuestring) : "";
    cJSON_Delete(req);
    if (Site().empty()) return "!先填音乐网站并保存";
    if (query.empty()) return "!先填歌名";
    std::vector<Song> songs;
    if (!Search(query, songs)) return "!网站连不上，或者不是这种格式的网站";
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON* arr = cJSON_AddArrayToObject(root, "results");
    for (const auto& s : songs) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "title", s.title.c_str());
        cJSON_AddStringToObject(item, "artist", s.artist.c_str());
        cJSON_AddItemToArray(arr, item);
    }
    char* json = cJSON_PrintUnformatted(root);
    std::string out = json ? json : "{}";
    cJSON_free(json);
    cJSON_Delete(root);
    std::lock_guard<std::mutex> lock(mutex_);
    results_ = std::move(songs);
    result_index_ = -1;
    return out;
}

std::string NomiMusic::PlayJson(const std::string& body) {
    cJSON* req = cJSON_Parse(body.c_str());
    cJSON* index = cJSON_GetObjectItem(req, "index");
    int i = cJSON_IsNumber(index) ? index->valueint : -1;
    cJSON_Delete(req);
    Song song;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (i < 0 || i >= static_cast<int>(results_.size())) return "!先搜索";
        song = results_[i];
    }
    // 在这里等着放起来（取歌曲页、探格式一两秒），放不了好告诉网页
    std::string result = Play(song);
    if (result[0] == '!') return result;
    std::lock_guard<std::mutex> lock(mutex_);
    result_index_ = i;
    return "{\"ok\":true}";
}

/* ---------- 大模型的工具 ---------- */

void NomiMusic::RegisterTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool(
        "self.music.play",
        "点歌：主人点名要听哪首歌、哪个歌手的歌时调用，比如\"放一首晴天\"\"来首周杰伦的歌\"\"我想听孤勇者\"；"
        "没点名、只说\"放点音乐\"\"来点轻音乐\"用 self.radio.play。\n"
        "query：歌名，知道歌手就带上（\"晴天 周杰伦\"），只说了歌手就填歌手名；\"下一首\" = 放上次搜到的下一首（主人说\"换一首\"）。"
        "调用后把这句回答说完，设备就安静下来开始放；快进快退用 self.podcast.seek，停用 self.radio.stop。",
        PropertyList({Property("query", kPropertyTypeString).SetMaxLength(60)}),
        [this](const PropertyList& props) -> ToolResult {
            std::string q = Trim(props["query"].value<std::string>());
            if (Site().empty()) {
                return std::unexpected(std::string("还没设置音乐网站，请主人在网页设置页的「在线音乐」里填一个"));
            }
            std::vector<Song> songs;
            int from = 0;
            if (q == "下一首" || q == "换一首" || q == "next") {
                std::lock_guard<std::mutex> lock(mutex_);
                songs = results_;
                from = result_index_ + 1;
            } else {
                if (q.empty()) return std::unexpected(std::string("要说想听哪首歌"));
                if (!Search(q, songs)) {
                    return std::unexpected(std::string("音乐网站连不上，或者页面格式变了（可能失效了），可以换一个同款网站填到设置页"));
                }
                if (songs.empty()) return std::unexpected("没搜到「" + q + "」");
                std::lock_guard<std::mutex> lock(mutex_);
                results_ = songs;
                result_index_ = -1;
            }
            std::string result = songs.empty() ? "!还没点过歌，先说想听哪首" : "!搜到的都放过了，换个歌名吧";
            for (int i = from; i < static_cast<int>(songs.size()) && i < from + kTries; i++) {
                result = Play(songs[i]);
                if (result[0] != '!') {
                    std::lock_guard<std::mutex> lock(mutex_);
                    result_index_ = i;
                    break;
                }
                ESP_LOGW(TAG, "%s: %s", songs[i].title.c_str(), result.c_str() + 1);
            }
            if (result[0] == '!') return std::unexpected(result.substr(1));
#if CONFIG_DIRECT_API
            DirectApiProtocol::ExitAfterReply();  // 这句说完就回待机，歌才放得出来
#endif
            return result;
        });
}
