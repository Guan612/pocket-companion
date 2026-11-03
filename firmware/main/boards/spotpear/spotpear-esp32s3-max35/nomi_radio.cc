#include "nomi_radio.h"

#include "application.h"
#include "board.h"
#include "mcp_server.h"
#include "nomi_mp4.h"
#include "nomi_util.h"
#include "settings.h"
#if CONFIG_DIRECT_API
#include "direct_api_protocol.h"
#endif

#include <cJSON.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <strings.h>

#define TAG "NomiRadio"

namespace {

constexpr const char* kNs = "nomi_radio";
constexpr size_t kMaxStations = 20;
constexpr size_t kBufferBytes = 64 * 1024;    // 网络数据缓冲：64kbps 约 8 秒
constexpr size_t kPrebufferBytes = 12 * 1024;  // 攒够这么多再开始放：64kbps 约 1.5 秒
constexpr size_t kReadChunk = 2048;
constexpr int64_t kResumeDelayUs = 800 * 1000;  // 回到待机多久后接着放（连续对话一轮轮之间别忽停忽放）
enum { kCodecMp3 = 1, kCodecAac = 2 };

// 默认台单：千听（蜻蜓）的直播 MP3 走 http 不占 TLS 内存；AsiaFM 是 AAC，流里带歌名
const NomiRadio::Station kDefaults[] = {
    {"华语经典", "http://lhttp.qingting.fm/live/5022308/64k.mp3"},
    {"AsiaFM 高清", "http://asiafm.hk:8000/asiahd"},
    {"怀集音乐", "http://lhttp.qingting.fm/live/4804/64k.mp3"},
    {"两广音乐", "http://lhttp.qingting.fm/live/20500149/64k.mp3"},
    {"中国之声", "http://lhttp.qingting.fm/live/15318317/64k.mp3"},
    {"第一财经", "http://lhttp.qingting.fm/live/276/64k.mp3"},
    {"故事广播", "http://lhttp.qingting.fm/live/20500182/64k.mp3"},
    {"评书", "http://lhttp.qingting.fm/live/1951/64k.mp3"},
};

// 千听的 https 地址换成 http：同一个 CDN，http 不用 TLS，省内部 RAM
std::string PreferHttp(std::string url) {
    for (const char* host : {"https://lhttp.qtfm.cn/", "https://lhttp.qingting.fm/"}) {
        if (url.rfind(host, 0) == 0) return "http://lhttp.qingting.fm/" + url.substr(strlen(host));
    }
    return url;
}

bool IsMp4(const std::string& url, const std::string& type) {
    std::string path = nomi::Lower(url.substr(0, url.find('?')));
    for (const char* ext : {".m4a", ".mp4", ".m4b"}) {
        if (path.size() >= 4 && path.compare(path.size() - 4, 4, ext) == 0) return true;
    }
    return type.find("mp4") != std::string::npos || type.find("m4a") != std::string::npos;
}

// 打开一个地址，跟着 301/302/307 走；from / to 不为 -1 时带 Range。url 改成跳转后的最终地址
std::unique_ptr<Http> OpenUrl(std::string& url, int64_t from, int64_t to, bool icy, int& code) {
    for (int hop = 0; hop < 4; hop++) {
        auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
        http->SetTimeout(8000);
        http->SetHeader("User-Agent", "PocketCompanion/1.0");
        if (icy) http->SetHeader("Icy-MetaData", "1");  // 要流里夹带的歌名
        if (from > 0 || to >= 0) {
            http->SetHeader("Range", "bytes=" + std::to_string(from < 0 ? 0 : from) + "-" +
                                         (to >= 0 ? std::to_string(to) : std::string()));
        }
        if (!http->Open("GET", url)) {
            ESP_LOGW(TAG, "Open failed: %s", url.c_str());
            code = 0;
            return nullptr;
        }
        auto status = http->GetStatusCode();
        code = status ? *status : 0;
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            std::string location = http->GetResponseHeader("location");
            http->Close();
            if (location.empty()) return nullptr;
            if (location[0] == '/') location = url.substr(0, url.find('/', url.find("://") + 3)) + location;
            url = location;
            continue;
        }
        return http;
    }
    return nullptr;
}

// 播客节目用 ESP-IDF 的 esp_http_client：Board 的网络接口连 https 时没设超时，服务器卡在握手就一直
// 挂着，收流任务卡住后播放器再也起不来；而且它每条连接另开一个 4KB 内部 RAM 栈的接收任务。
// esp_http_client 连接、握手、读都有超时，数据在本任务（PSRAM 栈）里同步读。
// 电台直播流还用 Board 的接口：有的老电台回 "ICY 200 OK"，esp_http_client 认不了
class EpisodeHttp {
public:
    ~EpisodeHttp() { Close(); }

    // from >= 0 时带 Range（to < 0 表示到结尾），跟着跳转走。返回 HTTP 状态码，连不上为 0
    int Open(const std::string& url, int64_t from, int64_t to = -1) {
        esp_http_client_config_t cfg = {};
        cfg.url = url.c_str();
        cfg.timeout_ms = 4000;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.buffer_size = 4096;  // 大于 1KB 的缓冲走 PSRAM
        cfg.buffer_size_tx = 2048;
        cfg.user_agent = "PocketCompanion/1.0";
        cfg.event_handler = OnEvent;
        cfg.user_data = this;
        client_ = esp_http_client_init(&cfg);
        if (client_ == nullptr) return 0;
        if (from >= 0) {
            char range[48];
            if (to >= 0) snprintf(range, sizeof(range), "bytes=%lld-%lld", static_cast<long long>(from), static_cast<long long>(to));
            else snprintf(range, sizeof(range), "bytes=%lld-", static_cast<long long>(from));
            esp_http_client_set_header(client_, "Range", range);
        }
        for (int hop = 0; hop < 5; hop++) {
            type_.clear();
            range_.clear();
            if (esp_http_client_open(client_, 0) != ESP_OK) return 0;
            esp_http_client_fetch_headers(client_);
            int code = esp_http_client_get_status_code(client_);
            if (code != 301 && code != 302 && code != 303 && code != 307 && code != 308) return code;
            esp_http_client_flush_response(client_, nullptr);
            esp_http_client_close(client_);
            if (esp_http_client_set_redirection(client_) != ESP_OK) return code;
        }
        return 0;
    }

    // >0 读到的字节；0 读完了；-1 断开；-2 这一阵没数据（超时）
    int Read(char* buf, int len) {
        int n = esp_http_client_read(client_, buf, len);
        if (n == -ESP_ERR_HTTP_EAGAIN) return -2;
        if (n == 0 && !esp_http_client_is_complete_data_received(client_)) return -1;
        return n < 0 ? -1 : n;
    }

    std::string Url() {
        char u[512] = {};
        if (client_) esp_http_client_get_url(client_, u, sizeof(u));
        return u;
    }

    const std::string& type() const { return type_; }

    // 整个文件多大：200 时是 Content-Length，206 时取 Content-Range 的 "/总长"；不知道为 -1
    int64_t Total(int code) {
        if (code == 200) return client_ ? esp_http_client_get_content_length(client_) : -1;
        size_t slash = range_.rfind('/');
        return slash == std::string::npos ? -1 : atoll(range_.c_str() + slash + 1);
    }

    void Close() {
        if (client_ == nullptr) return;
        esp_http_client_close(client_);
        esp_http_client_cleanup(client_);
        client_ = nullptr;
    }

private:
    static esp_err_t OnEvent(esp_http_client_event_t* e) {
        if (e->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(e->header_key, "Content-Type") == 0) {
            static_cast<EpisodeHttp*>(e->user_data)->type_ = nomi::Lower(e->header_value);
        } else if (e->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(e->header_key, "Content-Range") == 0) {
            static_cast<EpisodeHttp*>(e->user_data)->range_ = e->header_value;
        }
        return ESP_OK;
    }
    esp_http_client_handle_t client_ = nullptr;
    std::string type_, range_;
};

// 读一段（Range）到 out（size 字节）；url 换成跳转后的最终地址，下次直接去那里
bool RangeRead(std::string& url, uint64_t from, size_t size, uint8_t* out) {
    EpisodeHttp http;
    int code = http.Open(url, from, from + size - 1);
    if (code != 206 && !(code == 200 && from == 0)) {
        ESP_LOGW(TAG, "Range %llu+%u -> HTTP %d", static_cast<unsigned long long>(from), static_cast<unsigned>(size), code);
        return false;
    }
    url = http.Url();
    size_t got = 0;
    for (int stalls = 0; got < size && stalls < 3;) {
        int n = http.Read(reinterpret_cast<char*>(out) + got, size - got);
        if (n == -2) {
            stalls++;
            continue;
        }
        if (n <= 0) break;
        stalls = 0;
        got += n;
    }
    return got == size;
}

}  // namespace

NomiRadio& NomiRadio::GetInstance() {
    static NomiRadio instance;
    return instance;
}

/* ---------- 台单 ---------- */

void NomiRadio::LoadList() {
    std::lock_guard<std::mutex> lock(mutex_);
    list_.clear();
    Settings s(kNs);
    std::string json = s.GetString("list");
    cJSON* root = cJSON_Parse(json.c_str());
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        cJSON* name = cJSON_GetObjectItem(item, "n");
        cJSON* url = cJSON_GetObjectItem(item, "u");
        if (cJSON_IsString(name) && cJSON_IsString(url)) list_.push_back({name->valuestring, url->valuestring});
    }
    cJSON_Delete(root);
    if (json.empty()) list_.assign(std::begin(kDefaults), std::end(kDefaults));  // 从没改过就用默认的
    int last = s.GetInt("last", 0);
    current_index_ = last >= 0 && last < static_cast<int>(list_.size()) ? last : 0;
    if (!list_.empty()) current_ = list_[current_index_];
}

void NomiRadio::SaveList() {
    cJSON* root = cJSON_CreateArray();
    for (const auto& st : list_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "n", st.name.c_str());
        cJSON_AddStringToObject(item, "u", st.url.c_str());
        cJSON_AddItemToArray(root, item);
    }
    char* json = cJSON_PrintUnformatted(root);
    Settings s(kNs, true);
    s.SetString("list", json ? json : "[]");
    if (current_index_ >= 0) s.SetInt("last", current_index_);
    cJSON_free(json);
    cJSON_Delete(root);
}

/* ---------- 开始 / 停止 ---------- */

void NomiRadio::Start() {
    LoadList();
    reader_go_ = xSemaphoreCreateBinary();
    decoder_go_ = xSemaphoreCreateBinary();
    static StaticStreamBuffer_t sb;
    auto* storage = static_cast<uint8_t*>(heap_caps_malloc(kBufferBytes + 1, MALLOC_CAP_SPIRAM));
    stream_ = xStreamBufferCreateStatic(kBufferBytes, 1, storage, &sb);
    // 两个常驻任务，栈放 PSRAM（所以它们不能读写 flash / NVS）：平时阻塞在信号量上，不占内部 RAM。
    // 收流任务要做 TLS 握手（播客节目多是 https），栈给足
    xTaskCreatePinnedToCoreWithCaps([](void* arg) { static_cast<NomiRadio*>(arg)->ReaderLoop(); }, "radio_rx",
                                    12288, this, 2, nullptr, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps([](void* arg) { static_cast<NomiRadio*>(arg)->DecoderLoop(); }, "radio_dec",
                                    12288, this, 3, nullptr, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
    const esp_timer_create_args_t args = {
        .callback = [](void* arg) { static_cast<NomiRadio*>(arg)->Monitor(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "radio",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, 200 * 1000));
}

// 每 200ms：该放不放、该停不停就开始 / 收手；一次播放结束（断线）后按退避时间重连
void NomiRadio::Monitor() {
    int64_t now = esp_timer_get_time();
    bool idle = Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
    if (!idle) idle_since_us_ = 0;
    else if (idle_since_us_ == 0) idle_since_us_ = now;
    // 让出 https 最多 30 秒：拉节目单的连接万一卡住，也别把播放一直停着
    if (https_hold_ == 0) https_hold_since_us_ = 0;
    else if (https_hold_since_us_ == 0) https_hold_since_us_ = now;
    bool https_held = https_hold_ > 0 && now - https_hold_since_us_ < 30 * 1000000LL;
    bool held = alarm_hold_ || https_held || !idle || now - idle_since_us_ < kResumeDelayUs;
    bool should = wanted_ && !held && now >= retry_at_us_ && nomi::NetworkUp();

    if (running_) {
        if (busy_ == 0) {
            running_ = false;  // 两个任务都回到等待了：这次播放结束
            if (reached_end_ && wanted_ && IsEpisode()) {  // 节目从头到尾放完了
                reached_end_ = false;
                wanted_ = false;
                state_ = kOff;
                ReportProgress(true);
                position_ = 0;
            }
        } else if (!should && !stopping_) {
            stopping_ = true;
        }
    } else if (should && busy_ == 0) {
        BeginSession();
    }
    if (!wanted_) state_ = kOff;
    else if (held && state_ != kError) state_ = https_held && idle && !alarm_hold_ ? kLoading : kHeld;

    // 待机时小智把 Wi-Fi 设成最省电（MAX_MODEM，约 1 秒醒一次收包），直播流收不过来，
    // 缓冲一会儿空一会儿满，屏幕上就在"连接中"和"在放"之间跳。放的时候关掉省电，停了还原
    if (running_ || (wanted_ && !held)) {  // 断线重连的间隙也别切回省电
        wifi_ps_type_t ps;
        if (esp_wifi_get_ps(&ps) == ESP_OK && ps != WIFI_PS_NONE) {
            esp_wifi_set_ps(WIFI_PS_NONE);
            ESP_LOGI(TAG, "Wi-Fi power save off while playing");
        }
        wifi_awake_ = true;
    } else if (wifi_awake_) {
        wifi_awake_ = false;
        if (idle) esp_wifi_set_ps(WIFI_PS_MAX_MODEM);  // 待机该有的省电；对话中小智自己会设
    }
}

void NomiRadio::BeginSession() {
    xStreamBufferReset(stream_);  // 两个任务都在等信号量，没人碰缓冲
    {
        std::lock_guard<std::mutex> lock(mutex_);
        title_.clear();
        if (current_.episode) current_.start = position_;  // 断线重连、快进快退都从这里接着放
    }
    stopping_ = false;
    reader_done_ = false;
    reached_end_ = false;
    codec_ = 0;
    busy_ = 2;
    running_ = true;
    if (state_ != kError) state_ = kConnecting;
    xSemaphoreGive(reader_go_);
    xSemaphoreGive(decoder_go_);
}

void NomiRadio::EndSession() {
    if (running_) stopping_ = true;
}

void NomiRadio::Select(const Station& station, int index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = station;
        current_index_ = index;
        title_.clear();
    }
    position_ = station.episode ? station.start : 0;
    if (index >= 0) Settings(kNs, true).SetInt("last", index);
    failures_ = 0;
    retry_at_us_ = 0;
    state_ = kConnecting;
    EndSession();  // 正在放别的台：停掉，监视定时器会按新台重开
    wanted_ = true;
    ESP_LOGI(TAG, "Play %s (%s)", station.name.c_str(), station.url.c_str());
}

void NomiRadio::Play(int index) {
    Station st;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index < 0 || index >= static_cast<int>(list_.size())) return;
        st = list_[index];
    }
    Select(st, index);
}

void NomiRadio::Toggle() {
    if (wanted_) {
        Stop();
        return;
    }
    Station st;
    int index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        st = current_;
        index = current_index_;
    }
    if (st.url.empty()) Play(0);
    else {
        if (st.episode) st.start = position_;  // 节目从暂停的地方接着放
        Select(st, index);
    }
}

void NomiRadio::Step(int delta) {
    int n, index;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        n = static_cast<int>(list_.size());
        index = current_index_;
    }
    if (n == 0) return;
    Play(((index < 0 ? 0 : index) + delta + n) % n);
}

void NomiRadio::Stop() {
    if (wanted_ && IsEpisode()) ReportProgress(false);  // 记住放到哪了
    wanted_ = false;
    EndSession();
    state_ = kOff;
}

bool NomiRadio::HoldForHttps() {
    if (!wanted_ || Current().url.rfind("https://", 0) != 0) return false;  // 没在放，或 http 的流不占 TLS
    https_hold_++;
    // 等这次播放收完手（监视定时器 200ms 一轮），最多 5 秒
    for (int i = 0; i < 50 && running_; i++) vTaskDelay(pdMS_TO_TICKS(100));
    return true;
}

void NomiRadio::ReleaseHttpsHold() {
    if (https_hold_ > 0) https_hold_--;
}

/* ---------- 播客节目 ---------- */

void NomiRadio::PlayEpisode(const Station& episode) {
    if (wanted_ && IsEpisode()) ReportProgress(false);  // 换节目前把上一期的位置记下
    Select(episode, -1);
}

bool NomiRadio::IsEpisode() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_.episode;
}

NomiRadio::Station NomiRadio::Current() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

void NomiRadio::Seek(int delta_seconds) { SeekTo(position_ + delta_seconds); }

void NomiRadio::SeekTo(int second) {
    int duration;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!current_.episode) return;
        duration = current_.duration;
    }
    if (duration > 0) second = std::min(second, duration - 3);
    position_ = std::max(0, second);
    reached_end_ = false;
    if (wanted_) {
        state_ = kConnecting;
        retry_at_us_ = 0;
        EndSession();  // 监视定时器按新位置重开
    }
}

// 节目进度交给 NomiPodcast 存（写 NVS 要在主循环里：这里可能在 PSRAM 栈的任务里）
void NomiRadio::ReportProgress(bool finished) {
    if (!on_progress) return;
    Station st = Current();
    if (!st.episode || st.song) return;
    int pos = position_;
    Application::GetInstance().Schedule([this, st, pos, finished]() { on_progress(st, pos, finished); });
}

void NomiRadio::SetDuration(const std::string& url, int seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (seconds > 0 && current_.url == url && current_.duration <= 0) current_.duration = seconds;
}

/* ---------- 收流 ---------- */

void NomiRadio::ReaderLoop() {
    for (;;) {
        xSemaphoreTake(reader_go_, portMAX_DELAY);
        Station st = Current();
        bool mp4 = st.format == Station::kMp4 || (st.format == Station::kAuto && IsMp4(st.url, ""));
        bool ok = !st.episode ? ReadStream(PreferHttp(st.url), 0, 0, true)
                  : mp4       ? ReadMp4(st.url, st.start)
                              : ReadEpisode(st.url, st.start);
        if (!ok && !stopping_) {
            // 断线 / 连不上：1、3、6、12、24、30 秒后再试（服务器偶尔停一下，第一次马上重连）；
            // 连续三次失败显示"连不上"
            failures_++;
            int backoff = failures_ == 1 ? 1 : std::min(30, 3 << std::min(failures_ - 2, 4));
            retry_at_us_ = esp_timer_get_time() + backoff * 1000000LL;
            if (failures_ >= 3) state_ = kError;
            ESP_LOGW(TAG, "Stream ended (failure %d), retry in %ds", failures_, backoff);
        }
        reader_done_ = true;
        busy_--;
    }
}

bool NomiRadio::WriteAudio(const void* data, size_t size) {
    auto* p = static_cast<const uint8_t*>(data);
    while (size > 0 && !stopping_) {
        size_t sent = xStreamBufferSend(stream_, p, size, pdMS_TO_TICKS(200));
        p += sent;
        size -= sent;
    }
    return !stopping_;
}

// MP3 节目：按"文件大小 / 时长"算出 start 秒的字节位置，用 Range 跳过去
bool NomiRadio::ReadEpisode(const std::string& url, int start) {
    Station st = Current();
    int64_t offset = start > 0 ? int64_t(start) * (st.bytes_per_sec > 0 ? st.bytes_per_sec : 16000) : -1;
    EpisodeHttp http;
    int code = http.Open(url, offset);
    if (code != 200 && code != 206) {
        ESP_LOGW(TAG, "Episode HTTP %d: %s", code, url.c_str());
        return false;
    }
    if (st.format == Station::kAuto && IsMp4(url, http.type())) {  // 地址看不出来、响应头说是 m4a
        http.Close();
        return ReadMp4(url, start);
    }
    bool aac = st.format == Station::kAuto ? http.type().find("aac") != std::string::npos : st.format == Station::kAdts;
    codec_ = aac ? kCodecAac : kCodecMp3;
    if (st.song && st.duration <= 0) {
        // 歌没给时长：按文件大小估（和上面按时间跳转一样的码率）。播客不估，有的节目只有 64kbps
        int64_t total = http.Total(code);
        if (total > 0) SetDuration(url, total / (st.bytes_per_sec > 0 ? st.bytes_per_sec : 16000));
    }
    uint64_t skip = code == 200 && offset > 0 ? offset : 0;  // 服务器不认 Range：从头读，前面的扔掉
    ESP_LOGI(TAG, "Episode: %s from %lld", http.type().c_str(), static_cast<long long>(offset > 0 ? offset : 0));
    auto* buf = static_cast<char*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
    bool ok = true;
    for (int stalls = 0; !stopping_;) {
        int n = http.Read(buf, 4096);
        if (n == -2 && ++stalls < 4) continue;  // 一阵没数据：最多等 4 轮（16 秒）
        if (n == 0) {
            reached_end_ = true;  // 下完了
            break;
        }
        if (n < 0) {
            ESP_LOGW(TAG, "Episode stream stopped (%d)", n);
            ok = false;
            break;
        }
        stalls = 0;
        const char* p = buf;
        size_t len = n;
        if (skip > 0) {
            size_t drop = std::min<uint64_t>(skip, len);
            skip -= drop;
            p += drop;
            len -= drop;
        }
        if (len > 0) WriteAudio(p, len);
    }
    heap_caps_free(buf);
    return ok || stopping_;
}

// 电台直播流（流里夹着 ICY 歌名）
bool NomiRadio::ReadStream(const std::string& source, int redirects, int start, bool live) {
    (void)redirects;
    (void)start;
    std::string url = source;
    int code = 0;
    auto http = OpenUrl(url, -1, -1, live, code);
    if (!http) return false;
    if (code != 200) {
        ESP_LOGW(TAG, "HTTP %d: %s", code, url.c_str());
        http->Close();
        return false;
    }
    std::string type = nomi::Lower(http->GetResponseHeader("content-type"));
    uint64_t skip = 0;
    bool aac = type.find("aac") != std::string::npos || type.find("mp4a") != std::string::npos ||
               nomi::Lower(url).find(".aac") != std::string::npos;
    codec_ = aac ? kCodecAac : kCodecMp3;
    int metaint = atoi(http->GetResponseHeader("icy-metaint").c_str());
    ESP_LOGI(TAG, "Connected: %s, %s, metaint %d", type.c_str(), aac ? "AAC" : "MP3", metaint);
    // 连接、等响应头可以慢（上面 8 秒）；读数据每次最多等 3 秒，好让换台、叫它说话时很快收手
    // （千听的流是一阵一阵发的，常有一两秒空档，等太短了串口里全是超时）。连续 10 秒没数据才重连
    http->SetTimeout(3000);
    int stalled_ms = 0;
    size_t received = 0;

    // 按 ICY 协议拆：每 metaint 字节音频后面跟 1 字节长度（×16）和那么长的元数据
    auto* buf = static_cast<char*>(heap_caps_malloc(kReadChunk, MALLOC_CAP_SPIRAM));
    int until_meta = metaint, meta_left = 0;
    bool in_meta = false, want_len = false;
    std::string meta;
    auto write_audio = [this, &skip](const char* p, size_t n) {
        if (skip > 0) {
            size_t drop = std::min<uint64_t>(skip, n);
            skip -= drop;
            p += drop;
            n -= drop;
        }
        WriteAudio(p, n);
    };
    bool ok = true;
    while (!stopping_) {
        auto n = http->Read(buf, kReadChunk);
        if (!n && n.error().code == NetworkErrc::Timeout && stalled_ms < 9000) {
            stalled_ms += 3000;
            continue;
        }
        if (n && *n == 0 && !live) {  // 节目下载完了
            reached_end_ = true;
            break;
        }
        if (!n || *n <= 0) {
            // 断开或 10 秒没数据：记下原因，回头好查
            ESP_LOGW(TAG, "Stream stopped after %u bytes: %s", static_cast<unsigned>(received),
                     !n ? (n.error().code == NetworkErrc::Timeout ? "no data for 10s" : "disconnected") : "server closed");
            ok = false;
            break;
        }
        stalled_ms = 0;
        received += *n;
        const char* p = buf;
        int len = *n;
        while (len > 0) {
            if (metaint <= 0) {
                write_audio(p, len);
                break;
            }
            if (want_len) {
                meta_left = static_cast<unsigned char>(*p) * 16;
                p++;
                len--;
                want_len = false;
                in_meta = meta_left > 0;
                meta.clear();
                if (!in_meta) until_meta = metaint;
            } else if (in_meta) {
                int take = std::min(len, meta_left);
                meta.append(p, take);
                p += take;
                len -= take;
                meta_left -= take;
                if (meta_left == 0) {
                    in_meta = false;
                    until_meta = metaint;
                    size_t s = meta.find("StreamTitle='");
                    size_t e = s == std::string::npos ? s : meta.find("';", s + 13);
                    if (e != std::string::npos) {
                        std::string title = meta.substr(s + 13, e - s - 13);
                        if (!nomi::ValidUtf8(title)) title.clear();  // 有的台是 GBK，显示不了就不显示
                        std::lock_guard<std::mutex> lock(mutex_);
                        title_ = title;
                    }
                }
            } else {
                int take = std::min(len, until_meta);
                write_audio(p, take);
                p += take;
                len -= take;
                until_meta -= take;
                if (until_meta == 0) want_len = true;
            }
        }
    }
    heap_caps_free(buf);
    http->Close();
    return ok || stopping_;  // 被叫停不算失败
}

// m4a 节目：先拿到 moov（目录），按 start 秒找到那一帧的字节位置，从那里开始下载，
// 一帧帧补上 ADTS 头送去解码。同一期快进快退时 moov 用缓存
bool NomiRadio::ReadMp4(const std::string& source, int start) {
    std::string url = source;
    if (moov_url_ != source || moov_ == nullptr) {
        heap_caps_free(moov_);
        moov_ = nullptr;
        moov_url_.clear();
        auto* head = static_cast<uint8_t*>(heap_caps_malloc(65536, MALLOC_CAP_SPIRAM));
        uint64_t moov_offset = 0, moov_size = 0, next = 0, base = 0;
        bool found = false;
        size_t got = 65536;
        for (int hop = 0; hop < 5 && !found; hop++) {
            // 第一次读开头 64KB；moov 不在里面（在文件末尾）就跳到下一个顶层盒子再看
            if (!RangeRead(url, base, hop == 0 ? 65536 : 16, head)) {
                heap_caps_free(head);
                return false;
            }
            got = hop == 0 ? 65536 : 16;
            found = Mp4FindMoov(head, got, base, moov_offset, moov_size, next);
            base = next;
        }
        if (!found || moov_size > 4 * 1024 * 1024) {
            ESP_LOGW(TAG, "No usable moov (%llu bytes)", static_cast<unsigned long long>(moov_size));
            heap_caps_free(head);
            return false;
        }
        moov_ = static_cast<uint8_t*>(heap_caps_malloc(moov_size, MALLOC_CAP_SPIRAM));
        bool ok = moov_ != nullptr;
        if (ok && moov_offset == 0 && moov_size <= got) {
            memcpy(moov_, head, moov_size);
        } else if (ok) {
            ok = RangeRead(url, moov_offset, moov_size, moov_);
        }
        heap_caps_free(head);
        if (!ok) {
            heap_caps_free(moov_);
            moov_ = nullptr;
            return false;
        }
        moov_url_ = source;
        moov_size_ = moov_size;
        ESP_LOGI(TAG, "m4a moov: %u bytes", static_cast<unsigned>(moov_size));
    }
    Mp4Track track;
    if (!Mp4ParseMoov(moov_, moov_size_, track)) {
        ESP_LOGW(TAG, "m4a has no AAC track");
        return false;
    }
    SetDuration(source, Mp4DurationSeconds(track));
    Mp4Cursor cursor;
    if (!Mp4Seek(track, Mp4SampleAtSecond(track, start), cursor)) {
        reached_end_ = true;  // 位置已经在结尾
        return true;
    }
    EpisodeHttp http;
    int code = http.Open(url, int64_t(cursor.offset));
    if (code != 206) {
        ESP_LOGW(TAG, "m4a range -> HTTP %d", code);
        return false;
    }
    codec_ = kCodecAac;
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(8192, MALLOC_CAP_SPIRAM));
    size_t have = 0, used = 0;  // buf 里收到了多少、用掉了多少
    uint64_t pos = cursor.offset;
    int stalled_ms = 0;
    bool ok = true;
    // 从网络取 n 字节（skip 为 true 时扔掉）
    auto take = [&](uint8_t* out, size_t n) -> bool {
        while (n > 0) {
            if (used == have) {
                if (stopping_) return false;
                int r = http.Read(reinterpret_cast<char*>(buf), 8192);
                if (r == -2 && stalled_ms < 12000) {
                    stalled_ms += 4000;
                    continue;
                }
                if (r <= 0) return false;
                stalled_ms = 0;
                have = r;
                used = 0;
            }
            size_t k = std::min(n, have - used);
            if (out) {
                memcpy(out, buf + used, k);
                out += k;
            }
            used += k;
            n -= k;
            pos += k;
        }
        return true;
    };
    uint8_t frame[2048 + 7];
    while (!stopping_) {
        uint32_t size;
        if (!Mp4SampleSize(track, cursor, size)) {
            reached_end_ = true;  // 最后一帧也送出去了
            break;
        }
        if (cursor.offset < pos || size > 2048) {
            ESP_LOGW(TAG, "m4a layout not supported (frame %u)", static_cast<unsigned>(cursor.sample));
            ok = false;
            break;
        }
        if (!take(nullptr, cursor.offset - pos) || !take(frame + 7, size)) {
            ok = stopping_;
            break;
        }
        Mp4AdtsHeader(track, size, frame);
        if (!WriteAudio(frame, size + 7)) break;
        Mp4Advance(track, cursor, size);
    }
    heap_caps_free(buf);
    return ok || stopping_;
}

/* ---------- 解码、送播放 ---------- */

void NomiRadio::DecoderLoop() {
    static std::once_flag once;
    std::call_once(once, []() {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
    });
    auto* in = static_cast<uint8_t*>(heap_caps_malloc(kReadChunk, MALLOC_CAP_SPIRAM));
    std::vector<uint8_t> out(8192);  // AAC 一帧最多 2048 个采样 × 双声道 × 16 位
    for (;;) {
        xSemaphoreTake(decoder_go_, portMAX_DELAY);
        auto& audio = Application::GetInstance().GetAudioService();
        esp_audio_simple_dec_handle_t dec = nullptr;
        std::vector<int16_t> pending;
        int rate = 0, channels = 1;
        bool buffering = true;
        bool episode = IsEpisode();
        int base = position_, reported = position_;
        uint64_t played = 0;  // 这次播放送出去的采样数
        while (!stopping_) {
            // 先攒一点再放；放着放着缓冲空了（网络卡了）也重新攒
            if (buffering) {
                if (xStreamBufferBytesAvailable(stream_) < kPrebufferBytes && !reader_done_) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
                buffering = false;
            }
            if (dec == nullptr) {
                if (codec_ == 0) {
                    if (reader_done_) break;
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
                esp_audio_simple_dec_cfg_t cfg = {};
                cfg.dec_type = codec_ == kCodecAac ? ESP_AUDIO_SIMPLE_DEC_TYPE_AAC : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
                if (esp_audio_simple_dec_open(&cfg, &dec) != ESP_AUDIO_ERR_OK || dec == nullptr) {
                    ESP_LOGE(TAG, "Failed to open decoder");
                    dec = nullptr;
                    break;
                }
            }
            size_t n = xStreamBufferReceive(stream_, in, kReadChunk, pdMS_TO_TICKS(200));
            if (n == 0) {
                if (reader_done_) break;  // 收流结束且放完了
                if (state_ == kPlaying) {
                    state_ = kConnecting;
                    ESP_LOGW(TAG, "Buffer ran dry, rebuffering");
                }
                buffering = true;
                continue;
            }
            esp_audio_simple_dec_raw_t raw = {};
            raw.buffer = in;
            raw.len = n;
            while (raw.len > 0 && !stopping_) {
                esp_audio_simple_dec_out_t frame = {};
                frame.buffer = out.data();
                frame.len = out.size();
                auto ret = esp_audio_simple_dec_process(dec, &raw, &frame);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    out.resize(frame.needed_size);
                    continue;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    // 流中间坏一帧常见（刚连上从半帧开始），跳过这块接着解
                    ESP_LOGD(TAG, "Decode error %d", ret);
                    break;
                }
                if (frame.decoded_size > 0) {
                    if (rate == 0) {
                        esp_audio_simple_dec_info_t info = {};
                        esp_audio_simple_dec_get_info(dec, &info);
                        rate = info.sample_rate;
                        channels = info.channel ? info.channel : 1;
                        ESP_LOGI(TAG, "Playing: %d Hz, %d ch", rate, channels);
                    }
                    const auto* s = reinterpret_cast<const int16_t*>(frame.buffer);
                    size_t samples = frame.decoded_size / sizeof(int16_t) / channels;
                    for (size_t i = 0; i < samples; i++) {  // 混成单声道（喇叭只有一个）
                        int sum = 0;
                        for (int c = 0; c < channels; c++) sum += s[i * channels + c];
                        pending.push_back(static_cast<int16_t>(sum / channels));
                    }
                    // 攒够 60ms 送一块；播放队列满了就在这里等，正好按播放速度往下解
                    size_t chunk = static_cast<size_t>(rate) * 60 / 1000;
                    while (!stopping_ && rate > 0 && pending.size() >= chunk) {
                        std::vector<int16_t> part(pending.begin(), pending.begin() + chunk);
                        pending.erase(pending.begin(), pending.begin() + chunk);
                        if (audio.PushPcmToPlaybackQueue(std::move(part), rate, true)) {
                            if (state_ != kPlaying && !stopping_) {
                                state_ = kPlaying;
                                failures_ = 0;
                            }
                            if (episode && !stopping_) {
                                played += chunk;
                                position_ = base + static_cast<int>(played / rate);
                                if (position_ - reported >= 30) {  // 每放 30 秒记一次进度
                                    reported = position_;
                                    ReportProgress(false);
                                }
                            }
                        }
                    }
                }
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
                if (raw.consumed == 0 && frame.decoded_size == 0) break;  // 不够一帧，等下一块
            }
        }
        if (dec != nullptr) esp_audio_simple_dec_close(dec);
        busy_--;
    }
}

/* ---------- 给界面、网页、大模型 ---------- */

NomiRadio::State NomiRadio::GetState() { return state_; }

std::string NomiRadio::StationName() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_.name;
}

std::string NomiRadio::Title() {
    std::lock_guard<std::mutex> lock(mutex_);
    return title_;
}

int NomiRadio::CurrentIndex() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_index_;
}

std::string NomiRadio::ListText() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text;
    for (const auto& st : list_) text += (text.empty() ? "" : "\n") + st.name;
    return text;
}

std::string NomiRadio::PromptContext() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string ctx;
    if (wanted_ && current_.episode) {
        char pos[48];
        int p = position_;
        snprintf(pos, sizeof(pos), "（放到 %d:%02d", p / 60, p % 60);
        ctx = (current_.song ? "正在放歌《" + current_.name + "》- " + current_.podcast
                             : "正在放播客《" + current_.podcast + "》：" + current_.name) +
              pos +
              (current_.duration > 0 ? " / 共 " + std::to_string(current_.duration / 60) + " 分钟）" : std::string("）")) +
              "。电台";
    } else {
        ctx = wanted_ ? "电台：正在放「" + current_.name + "」" : std::string("电台：没在放");
        if (wanted_ && !title_.empty()) ctx += "（这首是 " + title_ + "）";
    }
    ctx += "。收藏的台：";
    for (size_t i = 0; i < list_.size(); i++) ctx += (i ? "、" : "") + list_[i].name;
    return ctx + "。";
}

std::string NomiRadio::ListJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(root, "stations");
    for (const auto& st : list_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "name", st.name.c_str());
        cJSON_AddStringToObject(item, "url", st.url.c_str());
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddNumberToObject(root, "max", kMaxStations);
    cJSON_AddNumberToObject(root, "playing", wanted_ && !current_.episode ? current_index_ : -2);  // -1 是搜来没收藏的台
    char* json = cJSON_PrintUnformatted(root);
    std::string out = json ? json : "{}";
    cJSON_free(json);
    cJSON_Delete(root);
    return out;
}

std::string NomiRadio::AddFromJson(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    cJSON* name = cJSON_GetObjectItem(root, "name");
    cJSON* url = cJSON_GetObjectItem(root, "url");
    std::string n = cJSON_IsString(name) ? name->valuestring : "";
    std::string u = cJSON_IsString(url) ? url->valuestring : "";
    cJSON_Delete(root);
    if (n.empty() || n.size() > 60) return "!台名要填，最多 20 个字";
    if ((u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0) || u.size() > 300) {
        return "!地址要以 http:// 或 https:// 开头";
    }
    if (u.find(".m3u8") != std::string::npos) return "!不支持 m3u8 分段流，要 MP3 / AAC 直播地址";
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& st : list_) {
        if (st.name == n) {  // 同名就是改地址
            st.url = u;
            SaveList();
            return "{\"ok\":true}";
        }
    }
    if (list_.size() >= kMaxStations) return "!最多存 20 个台，先删掉几个";
    list_.push_back({n, u});
    SaveList();
    return "{\"ok\":true}";
}

std::string NomiRadio::DeleteFromJson(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    cJSON* index = cJSON_GetObjectItem(root, "index");
    int i = cJSON_IsNumber(index) ? index->valueint : -1;
    cJSON_Delete(root);
    std::lock_guard<std::mutex> lock(mutex_);
    if (i < 0 || i >= static_cast<int>(list_.size())) return "!没有这个台";
    list_.erase(list_.begin() + i);
    if (current_index_ == i) current_index_ = -1;  // 正在放的还接着放，只是不在台单里了
    else if (current_index_ > i) current_index_--;
    SaveList();
    return "{\"ok\":true}";
}

std::string NomiRadio::MoveFromJson(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    cJSON* index = cJSON_GetObjectItem(root, "index");
    cJSON* to = cJSON_GetObjectItem(root, "to");
    int from = cJSON_IsNumber(index) ? index->valueint : -1;
    int dest = cJSON_IsNumber(to) ? to->valueint : -1;
    cJSON_Delete(root);
    std::lock_guard<std::mutex> lock(mutex_);
    int n = static_cast<int>(list_.size());
    if (from < 0 || from >= n) return "!没有这个台";
    if (dest < 0 || dest >= n) return "!挪不到那里";
    if (from == dest) return "{\"ok\":true}";
    Station st = std::move(list_[from]);
    list_.erase(list_.begin() + from);
    list_.insert(list_.begin() + dest, std::move(st));
    // 正在放（或上次放）的台跟着挪：下标改过来，屏幕上的换台、重启后接着放的都还是它
    if (current_index_ == from) current_index_ = dest;
    else if (from < current_index_ && current_index_ <= dest) current_index_--;
    else if (dest <= current_index_ && current_index_ < from) current_index_++;
    SaveList();
    return "{\"ok\":true}";
}

std::string NomiRadio::PlayFromJson(const std::string& body) {
    cJSON* root = cJSON_Parse(body.c_str());
    cJSON* index = cJSON_GetObjectItem(root, "index");
    bool stop = cJSON_IsTrue(cJSON_GetObjectItem(root, "stop"));
    int i = cJSON_IsNumber(index) ? index->valueint : -1;
    cJSON_Delete(root);
    if (stop) {
        Stop();
        return "{\"ok\":true}";
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (i < 0 || i >= static_cast<int>(list_.size())) return "!没有这个台";
    }
    Play(i);
    return "{\"ok\":true}";
}

// 电台目录（radio-browser）按名字搜，取票数最高的一个能直接放的（MP3 / AAC，不要 m3u8）
bool NomiRadio::Search(const std::string& query, Station& found) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    http->SetTimeout(8000);
    http->SetHeader("User-Agent", "PocketCompanion/1.0");
    std::string url = "http://all.api.radio-browser.info/json/stations/search?hidebroken=true&order=votes"
                      "&reverse=true&limit=8&name=" + nomi::UrlEncode(query);
    if (!http->Open("GET", url)) return false;
    auto status = http->GetStatusCode();
    std::string body = http->ReadAll();
    http->Close();
    if (!status || *status != 200) return false;
    size_t pos = 0;
    while ((pos = body.find('{', pos)) != std::string::npos) {
        size_t end = body.find("},{", pos);
        std::string obj = body.substr(pos, end == std::string::npos ? std::string::npos : end - pos + 1);
        pos = end == std::string::npos ? body.size() : end + 2;
        std::string codec = nomi::JsonField(obj, "codec"), u = nomi::JsonField(obj, "url_resolved");
        if (nomi::JsonField(obj, "hls") == "1" || u.empty() || u.find(".m3u8") != std::string::npos) continue;
        if (codec != "MP3" && codec != "AAC" && codec != "AAC+") continue;
        found.name = nomi::JsonField(obj, "name");
        found.url = PreferHttp(u);
        if (found.name.size() > 60) found.name.resize(60);
        while (!found.name.empty() && !nomi::ValidUtf8(found.name)) found.name.pop_back();
        return true;
    }
    return false;
}

void NomiRadio::RegisterTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool(
        "self.radio.play",
        "放网络电台 / 音乐。主人说\"放点音乐\"\"来点轻音乐\"\"听新闻\"\"放中国之声\"\"换个台\"时调用；"
        "点名要听哪首歌、哪个歌手的歌用 self.music.play。\n"
        "station：台名（收藏的台见系统提示），也可以是想听的类型或别的台名（会去网上的电台目录搜）；"
        "\"下一个\"\"上一个\"换台；空字符串接着放上次的台。调用后把这句回答说完，设备就安静下来开始放。",
        PropertyList({Property("station", kPropertyTypeString, std::string("")).SetMaxLength(60)}),
        [this](const PropertyList& p) -> ToolResult {
            std::string q = p["station"].value<std::string>();
            std::string result;
            if (q.empty()) {
                if (!wanted_) Toggle();  // 本来就在放（对话时暂停着）：说完自动接着放
                result = "好的，接着放「" + StationName() + "」";
            } else if (q == "下一个" || q == "next" || q == "上一个" || q == "prev") {
                Step(q == "下一个" || q == "next" ? 1 : -1);
                result = "换到「" + StationName() + "」";
            } else {
                int match = -1;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    std::string lq = nomi::Lower(q);
                    for (size_t i = 0; i < list_.size() && match < 0; i++) {
                        if (nomi::Lower(list_[i].name) == lq) match = i;
                    }
                    for (size_t i = 0; i < list_.size() && match < 0; i++) {
                        std::string ln = nomi::Lower(list_[i].name);
                        if (ln.find(lq) != std::string::npos || lq.find(ln) != std::string::npos) match = i;
                    }
                }
                if (match >= 0) {
                    Play(match);
                    result = "好的，放「" + StationName() + "」";
                } else {
                    Station found;
                    if (!Search(q, found)) {
                        return std::unexpected(std::string("收藏里没有，网上的电台目录也没搜到「") + q +
                                               "」；可以换个说法，或在网页设置页添加电台地址");
                    }
                    Select(found, -1);
                    result = "搜到「" + found.name + "」，马上放；主人想以后还听可以说\"收藏这个台\"";
                }
            }
#if CONFIG_DIRECT_API
            DirectApiProtocol::ExitAfterReply();  // 这句说完就回待机，电台才放得出来
#endif
            return result;
        });

    mcp.AddTool("self.radio.stop", "停掉电台、音乐或播客（播客会记住放到哪）。主人说\"关掉音乐\"\"别放了\"\"暂停\"\"停\"时调用。",
                PropertyList(),
                [this](const PropertyList& p) -> ToolResult {
                    (void)p;
                    Stop();
                    return std::string("已停止");
                });

    mcp.AddTool("self.radio.save", "把正在放的台收藏进台单（主人说\"收藏这个台\"）。", PropertyList(),
                [this](const PropertyList& p) -> ToolResult {
                    (void)p;
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (current_.url.empty()) return std::unexpected(std::string("现在没在放电台"));
                    for (size_t i = 0; i < list_.size(); i++) {
                        if (list_[i].url == current_.url) return std::string("这个台已经在收藏里了");
                    }
                    if (list_.size() >= kMaxStations) return std::unexpected(std::string("收藏满了（20 个）"));
                    list_.push_back(current_);
                    current_index_ = static_cast<int>(list_.size()) - 1;
                    SaveList();
                    return "已收藏「" + current_.name + "」";
                });

    mcp.AddTool("self.radio.remove", "从台单里删掉一个台。station 是台名。",
                PropertyList({Property("station", kPropertyTypeString).SetMaxLength(60)}),
                [this](const PropertyList& p) -> ToolResult {
                    std::string q = p["station"].value<std::string>();
                    int index = -1;
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        for (size_t i = 0; i < list_.size() && index < 0; i++) {
                            if (list_[i].name == q || list_[i].name.find(q) != std::string::npos) index = i;
                        }
                    }
                    if (index < 0) return std::unexpected("台单里没有「" + q + "」");
                    DeleteFromJson("{\"index\":" + std::to_string(index) + "}");
                    return "已删除「" + q + "」";
                });
}
