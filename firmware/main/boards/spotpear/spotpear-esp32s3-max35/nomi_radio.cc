#include "nomi_radio.h"

#include "application.h"
#include "board.h"
#include "mcp_server.h"
#include "nomi_mp4.h"
#include "nomi_sdcard.h"
#include "nomi_util.h"

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

constexpr size_t kBufferBytes = 64 * 1024;    // 网络数据缓冲：64kbps 约 8 秒
constexpr size_t kPrebufferBytes = 12 * 1024;  // 攒够这么多再开始放：64kbps 约 1.5 秒
constexpr size_t kReadChunk = 2048;
constexpr int kMaxBadChunks = 64;  // 连着这么多块（128KB）一帧都解不出来就放弃
constexpr int64_t kResumeDelayUs = 800 * 1000;  // 回到待机多久后接着放（连续对话一轮轮之间别忽停忽放）
enum { kCodecMp3 = 1, kCodecAac = 2, kCodecFlac = 3, kCodecWav = 4 };

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
    int last = 0;
    for (int stalls = 0; got < size && stalls < 3;) {
        int n = last = http.Read(reinterpret_cast<char*>(out) + got, size - got);
        if (n == -2) {
            stalls++;
            continue;
        }
        if (n <= 0) break;
        stalls = 0;
        got += n;
    }
    if (got != size) {
        ESP_LOGW(TAG, "Range %llu+%u: got %u (HTTP %d, last read %d): %s", static_cast<unsigned long long>(from),
                 static_cast<unsigned>(size), static_cast<unsigned>(got), code, last, url.c_str());
    }
    return got == size;
}

}  // namespace

NomiRadio& NomiRadio::GetInstance() {
    static NomiRadio instance;
    return instance;
}

/* ---------- 开始 / 停止 ---------- */

void NomiRadio::Start() {
    reader_go_ = xSemaphoreCreateBinary();
    decoder_go_ = xSemaphoreCreateBinary();
    static StaticStreamBuffer_t sb;
    auto* storage = static_cast<uint8_t*>(heap_caps_malloc(kBufferBytes + 1, MALLOC_CAP_SPIRAM));
    stream_ = xStreamBufferCreateStatic(kBufferBytes, 1, storage, &sb);
    // 两个常驻任务，栈放 PSRAM（所以它们不能读写 flash / NVS）：平时阻塞在信号量上，不占内部 RAM。
    // 收流任务要做 TLS 握手（播客节目多是 https），栈给足
    // 优先级都比界面任务（2）低：放 FLAC 时 CPU 本来就满，翻页时让界面先画；解码跟得上播放就行，
    // 播放队列和收流缓冲够垫几百毫秒
    xTaskCreatePinnedToCoreWithCaps([](void* arg) { static_cast<NomiRadio*>(arg)->ReaderLoop(); }, "radio_rx",
                                    12288, this, 1, nullptr, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps([](void* arg) { static_cast<NomiRadio*>(arg)->DecoderLoop(); }, "radio_dec",
                                    12288, this, 1, nullptr, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
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
    Station cur = Current();
    bool should = wanted_ && !held && now >= retry_at_us_ && (cur.local || nomi::NetworkUp());

    if (running_) {
        if (busy_ == 0) {
            running_ = false;  // 两个任务都回到等待了：这次播放结束
            paused_ = false;
            if (decode_failed_.exchange(false)) {
                retry_at_us_ = now + 5 * 1000000LL;  // 网上的流过一会儿重试
                if (cur.local) {  // 本地文件重试也一样：停下，主人换一首
                    wanted_ = false;
                    state_ = kError;
                }
            }
            if (reached_end_ && wanted_ && IsEpisode()) {  // 节目 / 歌从头到尾放完了
                reached_end_ = false;
                wanted_ = false;
                state_ = kOff;
                position_ = 0;
                if (on_finished) {
                    Application::GetInstance().Schedule([this, cur]() { on_finished(cur); });
                }
            }
        } else if (cur.local && wanted_ && held) {
            paused_ = true;  // 本地文件：对话、闹钟时停住就行，不用断开（不占网络、TLS 的内存），之后原地接着放
        } else if (!should && !stopping_) {
            paused_ = false;
            stopping_ = true;
        } else if (should) {
            paused_ = false;
        }
    } else if (should && busy_ == 0) {
        paused_ = false;
        BeginSession();
    }
    if (!wanted_) state_ = kOff;
    else if (held && state_ != kError) state_ = https_held && idle && !alarm_hold_ ? kLoading : kHeld;

    // 待机时小智把 Wi-Fi 设成最省电（MAX_MODEM，约 1 秒醒一次收包），直播流收不过来，
    // 缓冲一会儿空一会儿满，屏幕上就在"连接中"和"在放"之间跳。放的时候关掉省电，停了还原
    if (!cur.local && (running_ || (wanted_ && !held))) {  // 断线重连的间隙也别切回省电；本地文件不用网
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

void NomiRadio::Select(const Station& station) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = station;
        title_.clear();
    }
    position_ = station.episode ? station.start : 0;
    failures_ = 0;
    retry_at_us_ = 0;
    state_ = kConnecting;
    EndSession();  // 正在放别的：停掉，监视定时器会按新的这个重开
    wanted_ = true;
    ESP_LOGI(TAG, "Play %s (%s)", station.name.c_str(), station.url.c_str());
}

void NomiRadio::Toggle() {
    if (wanted_) {
        Stop();
        return;
    }
    Station st;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        st = current_;
    }
    if (st.url.empty()) return;  // 还没有放过的东西（放什么由应用决定）
    if (st.episode) st.start = position_;  // 节目从暂停的地方接着放
    Select(st);
}

void NomiRadio::Stop() {
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

/* ---------- 应用包放的节目 / 歌 ---------- */

void NomiRadio::PlayEpisode(const Station& episode) {
    Select(episode);
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

// 网上的音频按文件头认格式：给放它的应用先探好用哪个地址
NomiRadio::Station::Format NomiRadio::SniffFormat(const std::string& head) {
    if (head.size() < 12) return Station::kAuto;
    const auto* b = reinterpret_cast<const uint8_t*>(head.data());
    if (memcmp(b + 4, "ftyp", 4) == 0) return Station::kMp4;
    bool sync = b[0] == 0xFF && (b[1] & 0xE0) == 0xE0;
    if (memcmp(b, "ID3", 3) == 0 || (sync && (b[1] & 0x06) != 0)) return Station::kMp3;
    if (sync) return Station::kAdts;  // 帧头里 layer 为 0 的是 ADTS
    return Station::kAuto;
}

void NomiRadio::UpdateMeta(const std::string& url, const std::string& name, const std::string& artist) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_.url != url) return;
    if (!name.empty()) current_.name = name;
    if (!artist.empty()) current_.podcast = artist;
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
        bool ok = st.local    ? ReadLocal(st.url, st.start)
                  : !st.episode ? ReadStream(PreferHttp(st.url), 0, 0, true)
                  : mp4         ? ReadMp4(st.url, st.start)
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
        if (!moov_) {
            ESP_LOGW(TAG, "No PSRAM for moov (%u bytes, largest free %u)", static_cast<unsigned>(moov_size),
                     static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        }
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

/* ---------- SD 卡上的文件 ---------- */

namespace {

// FLAC 帧头的 CRC-8（多项式 0x07）
uint8_t FlacCrc8(const uint8_t* p, size_t n) {
    uint8_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : crc << 1;
    }
    return crc;
}

// buf[i] 起是不是一个 FLAC 帧头：同步码、保留位、长度可变的几段都对上，再核一下 CRC-8
bool IsFlacFrame(const uint8_t* b, size_t avail) {
    if (avail < 16 || b[0] != 0xFF || (b[1] & 0xFE) != 0xF8) return false;
    int bs = b[2] >> 4, sr = b[2] & 0x0F;
    if (bs == 0 || sr == 15 || (b[3] & 0x01) || (b[3] >> 4) > 10 || ((b[3] >> 1) & 7) == 3) return false;
    // 帧号是 UTF-8 式的变长编码：开头有几个 1 就占几个字节（没有 1 是 1 个字节，10 开头不合法）
    int ones = 0;
    for (uint8_t c = b[4]; ones < 8 && (c & 0x80); c <<= 1) ones++;
    if (ones == 1 || ones > 7) return false;
    size_t p = 4 + (ones == 0 ? 1 : ones);
    if (bs == 6) p += 1;
    if (bs == 7) p += 2;
    if (sr == 12) p += 1;
    if (sr == 13 || sr == 14) p += 2;
    return p < avail && FlacCrc8(b, p) == b[p];
}

// MPEG-1 / 2 Layer III 的码率（kbps），帧头第 3 字节高 4 位
int Mp3Bitrate(const uint8_t* h) {
    static const int v1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int v2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;
    int idx = h[2] >> 4;
    return ((h[1] >> 3) & 3) == 3 ? v1[idx] : v2[idx];
}

}  // namespace

// SD 卡上的文件：看文件头认格式，从 start 秒开始读，送进缓冲（缓冲满了会在这等，正好按播放速度往下读）。
// 跳到中间：WAV 按字节率精确算；MP3 / AAC 按平均码率估，解码器自己找帧；FLAC 先把文件头（fLaC 和元数据块）
// 送给解码器，再从估的位置往后找到一个帧头接着送
bool NomiRadio::ReadLocal(const std::string& path, int start) {
    NomiSdCard::Use sd;
    if (!sd) {
        ESP_LOGW(TAG, "No SD card: %s", path.c_str());
        return false;
    }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        ESP_LOGW(TAG, "Cannot open %s", path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    int64_t size = ftell(f);
    uint8_t head[64] = {};
    fseek(f, 0, SEEK_SET);
    size_t hn = fread(head, 1, sizeof(head), f);
    int64_t audio = 0;  // 第一帧音频在文件里的位置
    int duration = 0, block = 1, codec = kCodecMp3;
    // FLAC、WAV 的解码器要先看到文件头才认得出格式。不把原样的头喂给它：FLAC 的头里常有内嵌封面，
    // 大的几百 KB，喂过去又慢又没用。这里拼一个最小的头（FLAC 只留 STREAMINFO，WAV 只留 fmt），
    // 从头放、跳到中间放都先送它，再接音频帧
    std::string min_head;
    if (hn >= 4 && !memcmp(head, "fLaC", 4)) {
        codec = kCodecFlac;
        // 元数据块：4 字节头（最后一块的标志、类型、长度）；STREAMINFO 里有采样率和总采样数
        int64_t pos = 4;
        for (bool last = false; !last && pos + 4 <= size;) {
            uint8_t bh[4], si[34];
            fseek(f, pos, SEEK_SET);
            if (fread(bh, 1, 4, f) != 4) break;
            last = bh[0] & 0x80;
            uint32_t len = (bh[1] << 16) | (bh[2] << 8) | bh[3];
            if ((bh[0] & 0x7F) == 0 && len >= 34 && fread(si, 1, 34, f) == 34) {
                uint32_t rate = (si[10] << 12) | (si[11] << 4) | (si[12] >> 4);
                uint64_t total = (static_cast<uint64_t>(si[13] & 0x0F) << 32) | (static_cast<uint32_t>(si[14]) << 24) |
                                 (si[15] << 16) | (si[16] << 8) | si[17];
                if (rate) duration = total / rate;
                static const uint8_t kStreamInfoLast[4] = {0x80, 0x00, 0x00, 34};  // 标成最后一块
                min_head.assign("fLaC", 4);
                min_head.append(reinterpret_cast<const char*>(kStreamInfoLast), 4);
                min_head.append(reinterpret_cast<const char*>(si), 34);
            }
            pos += 4 + len;
        }
        audio = pos;
    } else if (hn >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4)) {
        codec = kCodecWav;
        uint32_t byte_rate = 0;
        std::string fmt_chunk;
        for (int64_t pos = 12; pos + 8 <= size;) {
            uint8_t ch[8], fmt[40];
            fseek(f, pos, SEEK_SET);
            if (fread(ch, 1, 8, f) != 8) break;
            uint32_t len = ch[4] | (ch[5] << 8) | (ch[6] << 16) | (static_cast<uint32_t>(ch[7]) << 24);
            if (!memcmp(ch, "fmt ", 4) && len >= 16 && len <= sizeof(fmt) && fread(fmt, 1, len, f) == len) {
                byte_rate = fmt[8] | (fmt[9] << 8) | (fmt[10] << 16) | (static_cast<uint32_t>(fmt[11]) << 24);
                block = std::max(1, fmt[12] | (fmt[13] << 8));
                fmt_chunk.assign(reinterpret_cast<const char*>(ch), 8);
                fmt_chunk.append(reinterpret_cast<const char*>(fmt), len);
            } else if (!memcmp(ch, "data", 4)) {
                if (!fmt_chunk.empty()) {
                    uint32_t data_len = static_cast<uint32_t>(std::min<int64_t>(len, size - (pos + 8)));
                    uint32_t riff_len = 4 + fmt_chunk.size() + 8 + data_len;
                    auto le32 = [](uint32_t v) {
                        char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16),
                                     static_cast<char>(v >> 24)};
                        return std::string(b, 4);
                    };
                    min_head = "RIFF" + le32(riff_len) + "WAVE" + fmt_chunk + "data" + le32(data_len);
                }
                audio = pos + 8;
                if (byte_rate) duration = std::min<int64_t>(len, size - audio) / byte_rate;
                break;
            }
            pos += 8 + len + (len & 1);
        }
        if (audio == 0) {
            fclose(f);
            ESP_LOGW(TAG, "WAV without data chunk: %s", path.c_str());
            return false;
        }
    } else {
        if (hn >= 8 && !memcmp(head + 4, "ftyp", 4)) {  // m4a：要按目录（moov）找帧，本地的还没做
            fclose(f);
            ESP_LOGW(TAG, "Local m4a not supported yet: %s", path.c_str());
            return false;
        }
        if (hn >= 10 && !memcmp(head, "ID3", 3)) {  // ID3v2 标签：长度是 4 个 7 位
            audio = 10 + ((head[6] & 0x7F) << 21 | (head[7] & 0x7F) << 14 | (head[8] & 0x7F) << 7 | (head[9] & 0x7F));
            if (head[5] & 0x10) audio += 10;
        }
        uint8_t fh[4] = {};
        fseek(f, audio, SEEK_SET);
        fread(fh, 1, 4, f);
        if (fh[0] == 0xFF && (fh[1] & 0xF6) == 0xF0) codec = kCodecAac;  // ADTS
        int kbps = codec == kCodecMp3 ? Mp3Bitrate(fh) : 0;
        duration = (size - audio) * 8 / ((kbps > 0 ? kbps : 128) * 1000);
    }
    SetDuration(path, duration);
    codec_ = codec;

    if ((codec == kCodecFlac || codec == kCodecWav) && min_head.empty()) {
        fclose(f);
        ESP_LOGW(TAG, "Bad %s header: %s", codec == kCodecFlac ? "FLAC" : "WAV", path.c_str());
        return false;
    }
    int64_t offset = audio;
    if (start > 0 && duration > 0) {
        offset = audio + (size - audio) * std::min(start, duration) / duration;
        if (codec == kCodecWav) offset = audio + (offset - audio) / block * block;
    }
    if (!min_head.empty()) WriteAudio(min_head.data(), min_head.size());
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
    if (!buf) {
        fclose(f);
        return false;
    }
    if (offset > audio && codec == kCodecFlac) {  // 往后找一个真的帧头（最多看 256KB）
        int64_t found = -1;
        for (int64_t pos = offset; found < 0 && pos < offset + 256 * 1024 && pos < size; pos += 4096 - 32) {
            fseek(f, pos, SEEK_SET);
            size_t n = fread(buf, 1, 4096, f);
            for (size_t i = 0; i + 16 <= n; i++) {
                if (IsFlacFrame(buf + i, n - i)) {
                    found = pos + i;
                    break;
                }
            }
            if (n < 4096) break;
        }
        offset = found >= 0 ? found : audio;
        if (found < 0) position_ = 0;  // 没找到就从头放
    }
    ESP_LOGI(TAG, "Local %s: codec %d, %d s, from byte %lld", path.c_str(), codec, duration,
             static_cast<long long>(offset));
    fseek(f, offset, SEEK_SET);
    bool ok = true;
    while (!stopping_) {
        size_t n = fread(buf, 1, 4096, f);
        if (n == 0) {
            if (ferror(f)) ok = false;  // 卡被拔了之类
            else reached_end_ = true;
            break;
        }
        if (!WriteAudio(buf, n)) break;
    }
    heap_caps_free(buf);
    fclose(f);
    return ok;
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
        int rate = 0, channels = 1, bytes_per_sample = 2;
        bool buffering = true;
        bool episode = IsEpisode();
        int base = position_;
        uint64_t played = 0;  // 这次播放送出去的采样数
        int bad_chunks = 0;   // 连着多少块一帧都没解出来
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
                cfg.dec_type = codec_ == kCodecAac    ? ESP_AUDIO_SIMPLE_DEC_TYPE_AAC
                               : codec_ == kCodecFlac ? ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC
                               : codec_ == kCodecWav  ? ESP_AUDIO_SIMPLE_DEC_TYPE_WAV
                                                      : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
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
            bool decoded = false, failed = false;
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
                    failed = true;
                    break;
                }
                if (frame.decoded_size > 0) {
                    decoded = true;
                    if (rate == 0) {
                        esp_audio_simple_dec_info_t info = {};
                        esp_audio_simple_dec_get_info(dec, &info);
                        rate = info.sample_rate;
                        channels = info.channel ? info.channel : 1;
                        bytes_per_sample = info.bits_per_sample >= 8 ? info.bits_per_sample / 8 : 2;
                        ESP_LOGI(TAG, "Playing: %d Hz, %d ch, %d bit (first frame %u bytes)", rate, channels,
                                 bytes_per_sample * 8, (unsigned)frame.decoded_size);
                    }
                    // 混成单声道（喇叭只有一个）；无损的 24 / 32 位、WAV 的 8 位都换成 16 位
                    const uint8_t* b = frame.buffer;
                    size_t samples = frame.decoded_size / bytes_per_sample / channels;
                    for (size_t i = 0; i < samples; i++) {
                        int32_t sum = 0;
                        for (int c = 0; c < channels; c++, b += bytes_per_sample) {
                            switch (bytes_per_sample) {
                            case 1: sum += (static_cast<int32_t>(b[0]) - 128) << 8; break;
                            case 3: sum += static_cast<int32_t>((b[0] << 8) | (b[1] << 16) | (b[2] << 24)) >> 16; break;
                            case 4: sum += static_cast<int32_t>(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)) >> 16; break;
                            default: sum += static_cast<int16_t>(b[0] | (b[1] << 8)); break;
                            }
                        }
                        pending.push_back(static_cast<int16_t>(sum / channels));
                    }
                    // 攒够 60ms 送一块；播放队列满了就在这里等，正好按播放速度往下解
                    size_t chunk = static_cast<size_t>(rate) * 60 / 1000;
                    while (paused_ && !stopping_) vTaskDelay(pdMS_TO_TICKS(50));  // 本地文件对话时停在这
                    while (!stopping_ && rate > 0 && pending.size() >= chunk) {
                        std::vector<int16_t> part(pending.begin(), pending.begin() + chunk);
                        pending.erase(pending.begin(), pending.begin() + chunk);
                        if (audio.PushPcmToPlaybackQueue(std::move(part), rate, true)) {
                            if (state_ != kPlaying && !stopping_) {
                                state_ = kPlaying;
                                failures_ = 0;
                            }
                            if (episode && !stopping_) {  // 节目的进度（应用自己来问，Position()）
                                played += chunk;
                                position_ = base + static_cast<int>(played / rate);
                            }
                        }
                    }
                }
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
                if (raw.consumed == 0 && frame.decoded_size == 0) break;  // 不够一帧，等下一块
            }
            // 一直解不出来（格式认错、文件坏了）：别一块块报错下去（解码库每次都打一行日志，串口刷满会把
            // 界面拖卡），这次就不放了
            if (decoded) bad_chunks = 0;
            else if (failed && ++bad_chunks >= kMaxBadChunks) {
                ESP_LOGW(TAG, "Cannot decode %s, giving up", Current().name.c_str());
                decode_failed_ = true;
                stopping_ = true;
            }
        }
        if (dec != nullptr) esp_audio_simple_dec_close(dec);
        busy_--;
    }
}

/* ---------- 给界面 ---------- */

NomiRadio::State NomiRadio::GetState() { return state_; }

std::string NomiRadio::StationName() {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_.name;
}

std::string NomiRadio::Title() {
    std::lock_guard<std::mutex> lock(mutex_);
    return title_;
}

/* ---------- 大模型的工具 ---------- */

void NomiRadio::RegisterTools() {
    // 放什么、放哪个、换哪个都归应用（self.app.command）；系统只留停 / 暂停这一个
    McpServer::GetInstance().AddTool(
        "self.radio.stop",
        "停掉正在放的音乐、电台或播客（应用会记住进度）。主人说\"关掉音乐\"\"别放了\"\"暂停\"\"停\"时调用。",
        PropertyList(),
        [this](const PropertyList& p) -> ToolResult {
            (void)p;
            Stop();
            return std::string("已停止");
        });
}
