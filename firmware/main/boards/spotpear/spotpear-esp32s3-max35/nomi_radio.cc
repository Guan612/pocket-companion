#include "nomi_radio.h"

#include "application.h"
#include "board.h"
#include "mcp_server.h"
#include "settings.h"
#if CONFIG_DIRECT_API
#include "direct_api_protocol.h"
#endif

#include <cJSON.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

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

bool NetworkUp() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    return netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

// 千听的 https 地址换成 http：同一个 CDN，http 不用 TLS，省内部 RAM
std::string PreferHttp(std::string url) {
    for (const char* host : {"https://lhttp.qtfm.cn/", "https://lhttp.qingting.fm/"}) {
        if (url.rfind(host, 0) == 0) return "http://lhttp.qingting.fm/" + url.substr(strlen(host));
    }
    return url;
}

bool ValidUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0) return false;
        for (int k = 1; k <= n; k++) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += n + 1;
    }
    return true;
}

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string UrlEncode(const std::string& s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// 电台目录的返回有几十 KB、几百个字段：用 cJSON 解析会在内部 RAM 里建几百个小节点，
// 这里只扫需要的几个字段。value 是字符串时处理常见转义（含 \uXXXX）
std::string JsonField(const std::string& obj, const char* key) {
    std::string pat = std::string("\"") + key + "\":";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    while (p < obj.size() && obj[p] == ' ') p++;
    if (p >= obj.size()) return "";
    if (obj[p] != '"') {
        size_t e = obj.find_first_of(",}", p);
        return obj.substr(p, e == std::string::npos ? std::string::npos : e - p);
    }
    std::string out;
    for (size_t i = p + 1; i < obj.size(); i++) {
        char c = obj[i];
        if (c == '"') break;
        if (c != '\\' || i + 1 >= obj.size()) {
            out += c;
            continue;
        }
        char e = obj[++i];
        if (e == 'u' && i + 4 < obj.size()) {
            unsigned cp = strtoul(obj.substr(i + 1, 4).c_str(), nullptr, 16);
            i += 4;
            if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        } else {
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e;  // \" \\ \/ 原样
        }
    }
    return out;
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
    Settings(kNs, true).SetString("list", json ? json : "[]");
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
    // 两个常驻任务，栈放 PSRAM（它们不写 flash）：平时阻塞在信号量上，不占内部 RAM
    xTaskCreatePinnedToCoreWithCaps([](void* arg) { static_cast<NomiRadio*>(arg)->ReaderLoop(); }, "radio_rx",
                                    6144, this, 2, nullptr, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
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
    bool held = alarm_hold_ || !idle || now - idle_since_us_ < kResumeDelayUs;
    bool should = wanted_ && !held && now >= retry_at_us_ && NetworkUp();

    if (running_) {
        if (busy_ == 0) {
            running_ = false;  // 两个任务都回到等待了：这次播放结束
        } else if (!should && !stopping_) {
            stopping_ = true;
        }
    } else if (should && busy_ == 0) {
        BeginSession();
    }
    if (!wanted_) state_ = kOff;
    else if (held && state_ != kError) state_ = kHeld;

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
    }
    stopping_ = false;
    reader_done_ = false;
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
    else Select(st, index);
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
    wanted_ = false;
    EndSession();
    state_ = kOff;
}

/* ---------- 收流 ---------- */

void NomiRadio::ReaderLoop() {
    for (;;) {
        xSemaphoreTake(reader_go_, portMAX_DELAY);
        std::string url;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            url = current_.url;
        }
        bool ok = ReadStream(PreferHttp(url), 0);
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

bool NomiRadio::ReadStream(const std::string& url, int redirects) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    http->SetTimeout(8000);
    http->SetHeader("Icy-MetaData", "1");  // 要流里夹带的歌名
    http->SetHeader("User-Agent", "PocketCompanion/1.0");
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "Open failed: %s", url.c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    int code = status ? *status : 0;
    if ((code == 301 || code == 302 || code == 307) && redirects < 3) {
        std::string location = http->GetResponseHeader("location");
        http->Close();
        return !location.empty() && ReadStream(location, redirects + 1);
    }
    if (code != 200) {
        ESP_LOGW(TAG, "HTTP %d: %s", code, url.c_str());
        http->Close();
        return false;
    }
    std::string type = Lower(http->GetResponseHeader("content-type"));
    bool aac = type.find("aac") != std::string::npos || type.find("mp4a") != std::string::npos ||
               Lower(url).find(".aac") != std::string::npos;
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
    auto write_audio = [this](const char* p, size_t n) {
        while (n > 0 && !stopping_) {
            size_t sent = xStreamBufferSend(stream_, p, n, pdMS_TO_TICKS(200));
            p += sent;
            n -= sent;
        }
    };
    bool ok = true;
    while (!stopping_) {
        auto n = http->Read(buf, kReadChunk);
        if (!n && n.error().code == NetworkErrc::Timeout && stalled_ms < 9000) {
            stalled_ms += 3000;
            continue;
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
                        if (!ValidUtf8(title)) title.clear();  // 有的台是 GBK，显示不了就不显示
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
    std::string ctx = wanted_ ? "电台：正在放「" + current_.name + "」" : std::string("电台：没在放");
    if (wanted_ && !title_.empty()) ctx += "（这首是 " + title_ + "）";
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
    cJSON_AddNumberToObject(root, "playing", wanted_ ? current_index_ : -2);  // -1 是搜来没收藏的台
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
                      "&reverse=true&limit=8&name=" + UrlEncode(query);
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
        std::string codec = JsonField(obj, "codec"), u = JsonField(obj, "url_resolved");
        if (JsonField(obj, "hls") == "1" || u.empty() || u.find(".m3u8") != std::string::npos) continue;
        if (codec != "MP3" && codec != "AAC" && codec != "AAC+") continue;
        found.name = JsonField(obj, "name");
        found.url = PreferHttp(u);
        if (found.name.size() > 60) found.name.resize(60);
        while (!found.name.empty() && !ValidUtf8(found.name)) found.name.pop_back();
        return true;
    }
    return false;
}

void NomiRadio::RegisterTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool(
        "self.radio.play",
        "放网络电台 / 音乐。主人说\"放点音乐\"\"来点轻音乐\"\"听新闻\"\"放中国之声\"\"换个台\"时调用。\n"
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
                    std::string lq = Lower(q);
                    for (size_t i = 0; i < list_.size() && match < 0; i++) {
                        if (Lower(list_[i].name) == lq) match = i;
                    }
                    for (size_t i = 0; i < list_.size() && match < 0; i++) {
                        std::string ln = Lower(list_[i].name);
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

    mcp.AddTool("self.radio.stop", "停掉电台 / 音乐。主人说\"关掉音乐\"\"别放了\"\"停\"时调用。", PropertyList(),
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
