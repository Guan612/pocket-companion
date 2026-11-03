#include "nomi_music.h"

#include "nomi_radio.h"
#include "nomi_util.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <cstring>

#define TAG "NomiMusic"

namespace {

// 封面的候选地址：https 的先试 http（同一个 CDN 一般两种都给，http 不占 TLS 的内部 RAM）
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

}  // namespace

NomiMusic& NomiMusic::GetInstance() {
    static NomiMusic instance;
    return instance;
}

void NomiMusic::Start() {
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

bool NomiMusic::Get(const std::string& url, std::string& body, size_t max_bytes) {
    std::lock_guard<std::mutex> lock(net_mutex_);
    HttpsHold hold(url);
    return nomi::HttpGet(url, body, max_bytes);
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
    return "装好音乐应用，跟它说「放点音乐」";
}

const uint16_t* NomiMusic::Cover(int& version) {
    version = cover_version_;
    int active = cover_active_;
    return active < 0 ? nullptr : covers_[active];
}
