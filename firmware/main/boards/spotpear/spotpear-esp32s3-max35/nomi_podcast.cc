#include "nomi_podcast.h"

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
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#define TAG "NomiPodcast"

namespace {

constexpr const char* kNs = "nomi_pod";
constexpr size_t kMaxSubs = 10;
constexpr int kMaxEpisodes = 10;  // RSS 只读最新这么多期（有的 RSS 有好几 MB）

// 进度存在 "p" + 订阅地址的哈希（NVS 的键最长 15 个字符）
std::string ProgressKey(const std::string& feed) {
    uint32_t h = 2166136261u;
    for (unsigned char c : feed) h = (h ^ c) * 16777619u;
    char key[12];
    snprintf(key, sizeof(key), "p%08lx", static_cast<unsigned long>(h));
    return key;
}

std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// itunes:duration："01:24:06"、"84:06" 或秒数
int ParseDuration(const std::string& s) {
    int parts[3] = {0, 0, 0}, n = 0;
    for (const char* p = s.c_str(); *p && n < 3;) {
        parts[n++] = atoi(p);
        p = strchr(p, ':');
        if (!p) break;
        p++;
    }
    if (n == 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
    if (n == 2) return parts[0] * 60 + parts[1];
    return parts[0];
}

// pubDate（RFC 822："Sat, 01 Nov 2025 07:00:00 GMT"）→ "11月1日"
std::string ShortDate(const std::string& s) {
    static const char* const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int m = 0; m < 12; m++) {
        size_t p = s.find(months[m]);
        if (p == std::string::npos || p < 3) continue;
        int day = atoi(s.c_str() + p - 3);
        if (day <= 0) continue;
        char buf[32];
        snprintf(buf, sizeof(buf), "%d月%d日", m + 1, day);
        return buf;
    }
    return "";
}

// 另开 https 时让播放器先把它那条 https 断开（内部 RAM 只够一条 TLS），用完放回去
// 播客的联网一次只做一件（网页、屏幕、语音可能同时要），做的时候正在放的 https 节目先让出来：
// 内部 RAM 只够一条 TLS，两条同时开时只剩 5～10KB
struct HttpsSlot {
    std::unique_lock<std::recursive_mutex> lock;
    bool held;
    explicit HttpsSlot(std::recursive_mutex& m) : lock(m), held(NomiRadio::GetInstance().HoldForHttps()) {}
    ~HttpsSlot() {
        if (held) NomiRadio::GetInstance().ReleaseHttpsHold();
    }
};

void LogHeap(const char* when) {
    ESP_LOGI(TAG, "Heap %s: internal free=%u largest=%u", when,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

std::string Clock(int seconds) {
    char buf[32];
    if (seconds >= 3600) snprintf(buf, sizeof(buf), "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60);
    else snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
    return buf;
}

}  // namespace

NomiPodcast& NomiPodcast::GetInstance() {
    static NomiPodcast instance;
    return instance;
}

void NomiPodcast::Start() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cJSON* root = cJSON_Parse(Settings(kNs).GetString("list").c_str());
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, root) {
            Podcast p;
            cJSON* n = cJSON_GetObjectItem(item, "n");
            cJSON* f = cJSON_GetObjectItem(item, "f");
            cJSON* a = cJSON_GetObjectItem(item, "a");
            if (!cJSON_IsString(n) || !cJSON_IsString(f)) continue;
            p.name = n->valuestring;
            p.feed = f->valuestring;
            if (cJSON_IsString(a)) p.art = a->valuestring;
            subs_.push_back(std::move(p));
        }
        cJSON_Delete(root);
    }
    for (auto& c : covers_) c = static_cast<uint16_t*>(heap_caps_calloc(kCoverSize * kCoverSize, 2, MALLOC_CAP_SPIRAM));
    jobs_sem_ = xSemaphoreCreateCounting(16, 0);
    // 后台任务：搜索、拉节目单、下封面（TLS 握手和 JPEG 解码都吃栈），栈放 PSRAM。
    // 栈在 PSRAM 的任务不能读写 flash（NVS 也不行：flash 操作时 cache 关掉，栈就访问不了），要写的排到主循环
    xTaskCreatePinnedToCoreWithCaps([](void* arg) { static_cast<NomiPodcast*>(arg)->WorkerLoop(); }, "podcast",
                                    12288, this, 2, nullptr, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);

    // 播放器每 30 秒、暂停、放完时报进度（已经在主循环里）：记下这个播客听到哪、最后放的是哪期
    NomiRadio::GetInstance().on_progress = [](const NomiRadio::Station& ep, int position, bool finished) {
        Settings s(kNs, true);
        cJSON* p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "u", ep.url.c_str());
        cJSON_AddNumberToObject(p, "s", finished ? 0 : position);
        cJSON_AddBoolToObject(p, "d", finished);
        char* json = cJSON_PrintUnformatted(p);
        s.SetString(ProgressKey(ep.key), json ? json : "{}");
        cJSON_free(json);
        cJSON_Delete(p);
        cJSON* last = cJSON_CreateObject();  // "接着听播客"用
        cJSON_AddStringToObject(last, "t", ep.name.c_str());
        cJSON_AddStringToObject(last, "u", ep.url.c_str());
        cJSON_AddStringToObject(last, "p", ep.podcast.c_str());
        cJSON_AddStringToObject(last, "f", ep.key.c_str());
        cJSON_AddNumberToObject(last, "s", finished ? 0 : position);
        cJSON_AddNumberToObject(last, "du", ep.duration);
        cJSON_AddNumberToObject(last, "b", ep.bytes_per_sec);
        cJSON_AddBoolToObject(last, "d", finished);
        json = cJSON_PrintUnformatted(last);
        s.SetString("last", json ? json : "{}");
        cJSON_free(json);
        cJSON_Delete(last);
    };
}

void NomiPodcast::Post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(jobs_mutex_);
        jobs_.push_back(std::move(job));
    }
    xSemaphoreGive(jobs_sem_);
}

void NomiPodcast::WorkerLoop() {
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

void NomiPodcast::SaveSubs() {
    std::vector<Podcast> copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        copy = subs_;
    }
    Application::GetInstance().Schedule([copy]() {
        cJSON* root = cJSON_CreateArray();
        for (const auto& p : copy) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "n", p.name.c_str());
            cJSON_AddStringToObject(item, "f", p.feed.c_str());
            if (!p.art.empty()) cJSON_AddStringToObject(item, "a", p.art.c_str());
            cJSON_AddItemToArray(root, item);
        }
        char* json = cJSON_PrintUnformatted(root);
        Settings(kNs, true).SetString("list", json ? json : "[]");
        cJSON_free(json);
        cJSON_Delete(root);
    });
}

/* ---------- 联网：目录、RSS、封面 ---------- */

bool NomiPodcast::SearchApple(const std::string& query, std::vector<Podcast>& out, int limit) {
    std::string body;
    std::string url = "https://itunes.apple.com/search?media=podcast&entity=podcast&limit=" + std::to_string(limit) +
                      "&term=" + nomi::UrlEncode(query);
    {
        HttpsSlot slot(net_mutex_);
        if (!nomi::HttpGet(url, body, 64 * 1024)) return false;
    }
    size_t pos = 0;
    while ((pos = body.find("{\"wrapperType\"", pos)) != std::string::npos) {
        size_t end = body.find("{\"wrapperType\"", pos + 1);
        std::string obj = body.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? body.size() : end;
        Podcast p;
        p.name = nomi::JsonField(obj, "collectionName");
        p.feed = nomi::JsonField(obj, "feedUrl");
        p.art = nomi::JsonField(obj, "artworkUrl100");
        if (!p.name.empty() && !p.feed.empty()) out.push_back(std::move(p));
    }
    return true;
}

bool NomiPodcast::FetchEpisodes(const std::string& feed, std::vector<Episode>& out, std::string* channel) {
    std::string body;
    size_t scanned = 0;
    int items = 0;
    int64_t deadline = esp_timer_get_time() + 20 * 1000000LL;  // 国外的源有时很慢：最多等 20 秒
    auto enough = [&](const std::string& b) {
        if (esp_timer_get_time() > deadline) return true;
        size_t p;
        while ((p = b.find("</item>", scanned)) != std::string::npos) {
            items++;
            scanned = p + 7;
        }
        if (b.size() > scanned + 7) scanned = b.size() - 7;
        return items >= kMaxEpisodes;
    };
    bool ok;
    {
        HttpsSlot slot(net_mutex_);
        LogHeap("before feed");
        ok = nomi::HttpGet(feed, body, 1536 * 1024, nullptr, enough);
    }
    LogHeap("after feed");
    if (!ok) return false;
    size_t first = body.find("<item");
    if (channel) *channel = Trim(nomi::XmlField(body.substr(0, first), "title"));
    for (size_t p = first; p != std::string::npos && static_cast<int>(out.size()) < kMaxEpisodes;) {
        size_t end = body.find("</item>", p);
        if (end == std::string::npos) break;
        std::string item = body.substr(p, end - p);
        p = body.find("<item", end);
        Episode ep;
        ep.title = Trim(nomi::XmlField(item, "title"));
        ep.url = nomi::XmlField(item, "enclosure", "url");
        if (ep.url.empty()) continue;
        ep.duration = ParseDuration(Trim(nomi::XmlField(item, "itunes:duration")));
        long long length = atoll(nomi::XmlField(item, "enclosure", "length").c_str());
        if (length > 0 && ep.duration > 0) ep.bytes_per_sec = static_cast<int>(length / ep.duration);
        ep.date = ShortDate(nomi::XmlField(item, "pubDate"));
        out.push_back(std::move(ep));
    }
    ESP_LOGI(TAG, "Feed %s: %u episodes from %u bytes", feed.c_str(), static_cast<unsigned>(out.size()),
             static_cast<unsigned>(body.size()));
    return !out.empty();
}

// 封面：Apple 的图片地址能直接要指定尺寸（…/100x100bb.jpg → …/88x88bb.jpg）
void NomiPodcast::LoadCover(const std::string& art) {
    if (art == cover_url_) return;
    cover_url_ = art;
    if (art.empty()) {
        cover_active_ = -1;
        cover_version_++;
        return;
    }
    // Apple 的图片服务 http 也能用：不开 TLS
    std::string url = art.rfind("https://", 0) == 0 && art.find(".mzstatic.com/") != std::string::npos
                          ? "http://" + art.substr(8)
                          : art;
    size_t p = url.find("100x100bb");
    if (p != std::string::npos) {
        url.replace(p, 9, std::to_string(kCoverSize) + "x" + std::to_string(kCoverSize) + "bb");
    }
    std::string jpg;
    bool ok;
    {
        HttpsSlot slot(net_mutex_);
        ok = nomi::HttpGet(url, jpg, 256 * 1024);
    }
    LogHeap("after cover");
    if (!ok) return;
    // 拿到的不是 88 见方（不是 Apple 的图）也行，缩放着写进没在显示的那块
    int next = cover_active_ == 0 ? 1 : 0;
    if (!nomi::DecodeCover(jpg, covers_[next], kCoverSize)) {
        ESP_LOGW(TAG, "Cover is not a usable JPEG: %s", url.c_str());
        return;
    }
    cover_active_ = next;
    cover_version_++;
}

const uint16_t* NomiPodcast::Cover(int& version) {
    version = cover_version_;
    int active = cover_active_;
    return active < 0 ? nullptr : covers_[active];
}

/* ---------- 放 ---------- */

int NomiPodcast::SavedPosition(const std::string& feed, const std::string& url) {
    cJSON* p = cJSON_Parse(Settings(kNs).GetString(ProgressKey(feed)).c_str());
    cJSON* u = cJSON_GetObjectItem(p, "u");
    cJSON* s = cJSON_GetObjectItem(p, "s");
    int pos = cJSON_IsString(u) && url == u->valuestring && cJSON_IsNumber(s) && !cJSON_IsTrue(cJSON_GetObjectItem(p, "d"))
                  ? s->valueint
                  : 0;
    cJSON_Delete(p);
    return std::max(0, pos - 3);  // 往回倒几秒，接上前面那句
}

std::string NomiPodcast::PlayEpisode(const Podcast& podcast, const Episode& episode, bool in_worker) {
    NomiRadio::Station st;
    st.name = episode.title;
    st.url = episode.url;
    st.episode = true;
    st.start = in_worker ? -1 : SavedPosition(podcast.feed, episode.url);
    st.duration = episode.duration;
    st.bytes_per_sec = episode.bytes_per_sec;
    st.podcast = podcast.name;
    st.key = podcast.feed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        playing_ = podcast;
    }
    // 先下封面（很小、走 http），再开始放：两条连接别同时开。开始放排到主循环里：后台任务的栈在 PSRAM，
    // 读写 NVS（查上次听到哪儿）时 flash 操作会关掉 cache，PSRAM 上的栈就访问不了，直接断言重启
    std::string art = podcast.art;
    Post([this, art, st]() {
        LoadCover(art);
        Application::GetInstance().Schedule([this, st]() mutable {
            if (st.start < 0) st.start = SavedPosition(st.key, st.url);
            NomiRadio::GetInstance().PlayEpisode(st);
        });
    });
    std::string text = "开始放《" + podcast.name + "》：" + episode.title;
    if (st.start > 0) text += "（从 " + Clock(st.start) + " 接着放）";
    return text;
}

void NomiPodcast::Open(int index) {
    std::string feed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < 0 || index >= static_cast<int>(subs_.size())) return;
        opened_ = index;
        episodes_.clear();
        feed = subs_[index].feed;
    }
    load_state_ = 1;
    Post([this, index, feed]() {
        std::vector<Episode> eps;
        bool ok = FetchEpisodes(feed, eps);
        std::lock_guard<std::mutex> lock(mutex_);
        if (opened_ != index) return;  // 等的时候已经点了别的
        episodes_ = std::move(eps);
        load_state_ = ok ? 0 : 2;
    });
}

void NomiPodcast::Back() {
    std::lock_guard<std::mutex> lock(mutex_);
    opened_ = -1;
}

void NomiPodcast::PlayOpened(int episode) {
    Podcast podcast;
    Episode ep;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (opened_ < 0 || episode < 0 || episode >= static_cast<int>(episodes_.size())) return;
        podcast = subs_[opened_];
        ep = episodes_[episode];
    }
    PlayEpisode(podcast, ep);
}

std::string NomiPodcast::ListText() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text;
    for (const auto& p : subs_) text += (text.empty() ? "" : "\n") + p.name;
    return text;
}

int NomiPodcast::Opened() {
    std::lock_guard<std::mutex> lock(mutex_);
    return opened_;
}

int NomiPodcast::LoadState() { return load_state_; }

std::string NomiPodcast::EpisodesText() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text;
    for (const auto& ep : episodes_) {
        std::string meta = ep.date;
        if (ep.duration > 0) meta += (meta.empty() ? "" : " · ") + std::to_string((ep.duration + 30) / 60) + " 分钟";
        text += (text.empty() ? "" : "\n") + ep.title + "\t" + meta;
    }
    return text;
}

int NomiPodcast::FindSub(const std::string& query) {
    std::string q = nomi::Lower(query);
    for (size_t i = 0; i < subs_.size(); i++) {
        if (nomi::Lower(subs_[i].name) == q) return i;
    }
    for (size_t i = 0; i < subs_.size(); i++) {
        std::string n = nomi::Lower(subs_[i].name);
        if (n.find(q) != std::string::npos || q.find(n) != std::string::npos) return i;
    }
    return -1;
}

std::string NomiPodcast::PromptContext() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (subs_.empty()) return "还没订阅播客。";
    std::string ctx = "订阅的播客：";
    for (size_t i = 0; i < subs_.size(); i++) ctx += (i ? "、" : "") + subs_[i].name;
    return ctx + "。";
}

/* ---------- 网页设置页 ---------- */

std::string NomiPodcast::ListJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(root, "podcasts");
    for (const auto& p : subs_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", p.name.c_str());
        cJSON_AddStringToObject(item, "feed", p.feed.c_str());
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddNumberToObject(root, "max", kMaxSubs);
    char* json = cJSON_PrintUnformatted(root);
    std::string out = json ? json : "{}";
    cJSON_free(json);
    cJSON_Delete(root);
    return out;
}

std::string NomiPodcast::SearchJson(const std::string& body) {
    cJSON* req = cJSON_Parse(body.c_str());
    cJSON* q = cJSON_GetObjectItem(req, "q");
    std::string query = cJSON_IsString(q) ? q->valuestring : "";
    cJSON_Delete(req);
    if (query.empty()) return "!先填要搜的播客名";
    std::vector<Podcast> found;
    if (!SearchApple(query, found, 6)) return "!搜索失败，检查设备网络后再试";
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON* arr = cJSON_AddArrayToObject(root, "results");
    for (const auto& p : found) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", p.name.c_str());
        cJSON_AddStringToObject(item, "feed", p.feed.c_str());
        cJSON_AddStringToObject(item, "art", p.art.c_str());
        cJSON_AddItemToArray(arr, item);
    }
    char* json = cJSON_PrintUnformatted(root);
    std::string out = json ? json : "{}";
    cJSON_free(json);
    cJSON_Delete(root);
    return out;
}

std::string NomiPodcast::SubscribeJson(const std::string& body) {
    cJSON* req = cJSON_Parse(body.c_str());
    Podcast p;
    for (auto [key, field] : {std::pair{"name", &p.name}, std::pair{"feed", &p.feed}, std::pair{"art", &p.art}}) {
        cJSON* v = cJSON_GetObjectItem(req, key);
        if (cJSON_IsString(v)) *field = Trim(v->valuestring);
    }
    cJSON_Delete(req);
    if (p.feed.rfind("http://", 0) != 0 && p.feed.rfind("https://", 0) != 0) return "!RSS 地址要以 http:// 或 https:// 开头";
    if (p.name.empty()) {  // 只填了 RSS 地址：去读一下节目名，顺便看看能不能用
        std::vector<Episode> eps;
        if (!FetchEpisodes(p.feed, eps, &p.name) || p.name.empty()) return "!这个地址读不到节目，确认是播客的 RSS 地址";
    }
    if (p.name.size() > 90) p.name.resize(90);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& s : subs_) {
            if (s.feed == p.feed) return "!已经订阅了";
        }
        if (subs_.size() >= kMaxSubs) return "!最多订阅 10 个，先删掉几个";
        subs_.push_back(p);
    }
    SaveSubs();
    return "{\"ok\":true}";
}

std::string NomiPodcast::DeleteJson(const std::string& body) {
    cJSON* req = cJSON_Parse(body.c_str());
    cJSON* index = cJSON_GetObjectItem(req, "index");
    int i = cJSON_IsNumber(index) ? index->valueint : -1;
    cJSON_Delete(req);
    Podcast removed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (i < 0 || i >= static_cast<int>(subs_.size())) return "!没有这个订阅";
        removed = subs_[i];
        subs_.erase(subs_.begin() + i);
        opened_ = -1;
    }
    SaveSubs();
    Settings(kNs, true).EraseKey(ProgressKey(removed.feed));  // 播放进度没人管了，跟着订阅一起清掉
    return "{\"ok\":true}";
}

std::string NomiPodcast::PlayJson(const std::string& body) {
    cJSON* req = cJSON_Parse(body.c_str());
    cJSON* index = cJSON_GetObjectItem(req, "index");
    int i = cJSON_IsNumber(index) ? index->valueint : -1;
    cJSON_Delete(req);
    Podcast p;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (i < 0 || i >= static_cast<int>(subs_.size())) return "!没有这个订阅";
        p = subs_[i];
    }
    // 交给后台任务：拉节目单可能要十几秒，别占着网页设置页的服务
    Post([this, p]() {
        std::vector<Episode> eps;
        if (FetchEpisodes(p.feed, eps)) PlayEpisode(p, eps[0], true);
    });
    return "{\"ok\":true}";
}

/* ---------- 大模型的工具 ---------- */

void NomiPodcast::RegisterTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool(
        "self.podcast.play",
        "放播客。主人说\"放《忽左忽右》最新一期\"\"听播客\"\"接着听\"时调用。\n"
        "podcast：播客名（订阅的见系统提示；没订阅的也行，会去 Apple 播客目录搜），空字符串 = 接着放上次那期；"
        "episode：哪一期，空 = 最新一期，\"上一期\" = 倒数第二期，也可以是标题里的关键词。"
        "上次没听完的会从停下的地方接着放。调用后把这句回答说完，设备就安静下来开始放。",
        PropertyList({Property("podcast", kPropertyTypeString, std::string("")).SetMaxLength(90),
                      Property("episode", kPropertyTypeString, std::string("")).SetMaxLength(90)}),
        [this](const PropertyList& props) -> ToolResult {
            std::string name = Trim(props["podcast"].value<std::string>());
            std::string which = Trim(props["episode"].value<std::string>());
            std::string result;
            if (name.empty()) {
                // 接着放：正在暂停的那期，或者存着的最后一期
                auto& radio = NomiRadio::GetInstance();
                if (radio.IsEpisode() && !radio.Current().song) {
                    if (!radio.Wanted()) radio.Toggle();
                    result = "接着放《" + radio.Current().podcast + "》：" + radio.Current().name;
                } else {
                    cJSON* last = cJSON_Parse(Settings(kNs).GetString("last").c_str());
                    auto str = [last](const char* k) {
                        cJSON* v = cJSON_GetObjectItem(last, k);
                        return std::string(cJSON_IsString(v) ? v->valuestring : "");
                    };
                    auto num = [last](const char* k) {
                        cJSON* v = cJSON_GetObjectItem(last, k);
                        return cJSON_IsNumber(v) ? v->valueint : 0;
                    };
                    if (str("u").empty()) {
                        cJSON_Delete(last);
                        return std::unexpected(std::string("还没听过播客，先说想听哪个"));
                    }
                    Podcast p;
                    p.name = str("p");
                    p.feed = str("f");
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        int i = FindSub(p.name);
                        if (i >= 0 && subs_[i].feed == p.feed) p.art = subs_[i].art;
                    }
                    Episode ep;
                    ep.title = str("t");
                    ep.url = str("u");
                    ep.duration = num("du");
                    ep.bytes_per_sec = num("b");
                    bool done = cJSON_IsTrue(cJSON_GetObjectItem(last, "d"));
                    cJSON_Delete(last);
                    result = PlayEpisode(p, ep) + (done ? "（上次已经听完了，从头放）" : "");
                }
            } else {
                Podcast p;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    int i = FindSub(name);
                    if (i >= 0) p = subs_[i];
                }
                if (p.feed.empty()) {
                    std::vector<Podcast> found;
                    if (!SearchApple(name, found, 1) || found.empty()) {
                        return std::unexpected("订阅里没有，Apple 播客目录也没搜到「" + name + "」");
                    }
                    p = found[0];
                }
                std::vector<Episode> eps;
                if (!FetchEpisodes(p.feed, eps)) return std::unexpected("《" + p.name + "》的节目单读不到");
                size_t pick = 0;
                if (which == "上一期" || which == "前一期") pick = std::min<size_t>(1, eps.size() - 1);
                else if (!which.empty() && which != "最新" && which != "最新一期" && which != "latest") {
                    for (size_t i = 0; i < eps.size(); i++) {
                        if (eps[i].title.find(which) != std::string::npos) {
                            pick = i;
                            break;
                        }
                    }
                }
                result = PlayEpisode(p, eps[pick]);
                bool subscribed;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    subscribed = FindSub(p.name) >= 0;
                }
                if (!subscribed) result += "；这个播客还没订阅，主人想订阅可以说\"订阅这个播客\"";
            }
#if CONFIG_DIRECT_API
            DirectApiProtocol::ExitAfterReply();  // 这句说完就回待机，节目才放得出来
#endif
            return result;
        });

    mcp.AddTool("self.podcast.episodes", "列出一个播客最新几期的标题（主人问\"XX 最近更新了什么\"时用）。podcast：播客名。",
                PropertyList({Property("podcast", kPropertyTypeString).SetMaxLength(90)}),
                [this](const PropertyList& props) -> ToolResult {
                    std::string name = props["podcast"].value<std::string>();
                    Podcast p;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        int i = FindSub(name);
                        if (i >= 0) p = subs_[i];
                    }
                    if (p.feed.empty()) {
                        std::vector<Podcast> found;
                        if (!SearchApple(name, found, 1) || found.empty()) {
                            return std::unexpected("没找到「" + name + "」");
                        }
                        p = found[0];
                    }
                    std::vector<Episode> eps;
                    if (!FetchEpisodes(p.feed, eps)) return std::unexpected(std::string("节目单读不到"));
                    std::string out = "《" + p.name + "》最新几期：";
                    for (size_t i = 0; i < eps.size() && i < 5; i++) {
                        out += "\n" + std::to_string(i + 1) + ". " + eps[i].title + "（" + eps[i].date + "）";
                    }
                    return out;
                });

    mcp.AddTool("self.podcast.subscribe",
                "订阅播客。podcast：播客名；空字符串 = 订阅正在放的这个。",
                PropertyList({Property("podcast", kPropertyTypeString, std::string("")).SetMaxLength(90)}),
                [this](const PropertyList& props) -> ToolResult {
                    std::string name = Trim(props["podcast"].value<std::string>());
                    Podcast p;
                    if (name.empty()) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        p = playing_;
                    } else {
                        std::vector<Podcast> found;
                        if (!SearchApple(name, found, 1) || found.empty()) {
                            return std::unexpected("Apple 播客目录没搜到「" + name + "」");
                        }
                        p = found[0];
                    }
                    if (p.feed.empty()) return std::unexpected(std::string("现在没在放播客"));
                    cJSON* req = cJSON_CreateObject();
                    cJSON_AddStringToObject(req, "name", p.name.c_str());
                    cJSON_AddStringToObject(req, "feed", p.feed.c_str());
                    cJSON_AddStringToObject(req, "art", p.art.c_str());
                    char* json = cJSON_PrintUnformatted(req);
                    std::string r = SubscribeJson(json ? json : "{}");
                    cJSON_free(json);
                    cJSON_Delete(req);
                    if (!r.empty() && r[0] == '!') return std::unexpected(r.substr(1));
                    return "已订阅《" + p.name + "》";
                });

    mcp.AddTool("self.podcast.unsubscribe", "取消订阅播客。podcast：播客名。",
                PropertyList({Property("podcast", kPropertyTypeString).SetMaxLength(90)}),
                [this](const PropertyList& props) -> ToolResult {
                    int i;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        i = FindSub(props["podcast"].value<std::string>());
                    }
                    if (i < 0) return std::unexpected(std::string("订阅里没有这个播客"));
                    DeleteJson("{\"index\":" + std::to_string(i) + "}");
                    return std::string("已取消订阅");
                });

    mcp.AddTool("self.podcast.seek",
                "播客 / 歌曲快进、快退。seconds：正数快进、负数倒回（比如 30、-15）。主人说\"快进一分钟\"\"倒回去一点\"时调用。",
                PropertyList({Property("seconds", kPropertyTypeInteger, -7200, 7200)}),
                [](const PropertyList& props) -> ToolResult {
                    auto& radio = NomiRadio::GetInstance();
                    if (!radio.IsEpisode()) return std::unexpected(std::string("现在没在放播客或歌"));
                    radio.Seek(props["seconds"].value<int>());
                    if (!radio.Wanted()) radio.Toggle();
#if CONFIG_DIRECT_API
                    DirectApiProtocol::ExitAfterReply();
#endif
                    return "好的，到 " + Clock(radio.Position());
                });
}
