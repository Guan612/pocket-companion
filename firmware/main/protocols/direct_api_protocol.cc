#include "direct_api_protocol.h"

#include "application.h"
#include "board.h"
#include "direct_api_config_server.h"
#include "display.h"
#include "http.h"
#include "ogg_demuxer.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_opus_dec.h>
#include <esp_timer.h>

#include <array>
#include <cstring>

#define TAG "DirectApi"

namespace {

constexpr int kMicSampleRate = 16000;      // 小智上行 Opus 的采样率
constexpr int kMicFrameMs = 60;            // 小智上行 Opus 的帧长
constexpr int kEndSilenceMs = 900;         // 说完后静音多久算一句结束
constexpr int kBargeInMs = 300;            // 它说话时用户开口多久算打断
constexpr int kMaxRecordMs = 15000;
constexpr size_t kHistoryLimit = 12;
constexpr int kHttpTimeoutMs = 30000;
constexpr const char* kSettingsNs = "directapi";
constexpr const char* kDefaultImageQuestion = "你看到了什么？简单说说。";

// 拍照后暂存的 JPEG，取用一次就清掉
std::mutex g_image_mutex;
std::string g_pending_image;

std::string TakePendingImage() {
    std::lock_guard<std::mutex> lock(g_image_mutex);
    return std::exchange(g_pending_image, std::string());
}

bool HasPendingImage() {
    std::lock_guard<std::mutex> lock(g_image_mutex);
    return !g_pending_image.empty();
}

std::string Base64(const std::string& data) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = (uint8_t)data[i] << 16 | (uint8_t)data[i + 1] << 8 | (uint8_t)data[i + 2];
        out += kTable[v >> 18];
        out += kTable[(v >> 12) & 63];
        out += kTable[(v >> 6) & 63];
        out += kTable[v & 63];
    }
    if (i < data.size()) {
        uint32_t v = (uint8_t)data[i] << 16 | (i + 1 < data.size() ? (uint8_t)data[i + 1] << 8 : 0);
        out += kTable[v >> 18];
        out += kTable[(v >> 12) & 63];
        out += i + 1 < data.size() ? kTable[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}
constexpr const char* kDefaultPrompt =
    "你是一个放在桌面上的小伙伴，用口语化的中文回答，每次不超过三句话，不要用 markdown。"
    "回答开头必须带一个情绪标签，只能是 [happy]、[neutral]、[confused] 之一，"
    "例如：[happy]好呀，我来帮你看看。";

std::string TrimSlash(std::string url) {
    while (!url.empty() && url.back() == '/') url.pop_back();
    return url;
}

std::unique_ptr<Http> NewHttp(const std::string& key) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    http->SetTimeout(kHttpTimeoutMs);
    if (!key.empty()) {
        http->SetHeader("Authorization", "Bearer " + key);
    }
    return http;
}

// 16k 单声道 16bit PCM 加 44 字节 WAV 头
std::string MakeWav(const std::vector<int16_t>& pcm) {
    const uint32_t data_size = pcm.size() * sizeof(int16_t);
    std::string wav;
    wav.reserve(44 + data_size);
    auto u32 = [&wav](uint32_t v) { wav.append(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&wav](uint16_t v) { wav.append(reinterpret_cast<const char*>(&v), 2); };
    wav += "RIFF";
    u32(36 + data_size);
    wav += "WAVEfmt ";
    u32(16);
    u16(1);  // PCM
    u16(1);  // 单声道
    u32(kMicSampleRate);
    u32(kMicSampleRate * 2);
    u16(2);
    u16(16);
    wav += "data";
    u32(data_size);
    wav.append(reinterpret_cast<const char*>(pcm.data()), data_size);
    return wav;
}

std::vector<int16_t> DecodeOpus(const std::vector<std::vector<uint8_t>>& packets) {
    std::vector<int16_t> pcm;
    esp_opus_dec_cfg_t cfg = {
        .sample_rate = kMicSampleRate,
        .channel = ESP_AUDIO_MONO,
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
    };
    void* dec = nullptr;
    if (esp_opus_dec_open(&cfg, sizeof(cfg), &dec) != ESP_AUDIO_ERR_OK || dec == nullptr) {
        ESP_LOGE(TAG, "Failed to open opus decoder");
        return pcm;
    }
    const size_t frame_samples = kMicSampleRate * kMicFrameMs / 1000;
    pcm.reserve(packets.size() * frame_samples);
    std::vector<int16_t> frame(frame_samples);
    for (const auto& p : packets) {
        esp_audio_dec_in_raw_t raw = {
            .buffer = const_cast<uint8_t*>(p.data()),
            .len = static_cast<uint32_t>(p.size()),
            .consumed = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t out = {
            .buffer = reinterpret_cast<uint8_t*>(frame.data()),
            .len = static_cast<uint32_t>(frame.size() * sizeof(int16_t)),
            .decoded_size = 0,
        };
        esp_audio_dec_info_t info = {};
        if (esp_opus_dec_decode(dec, &raw, &out, &info) == ESP_AUDIO_ERR_OK) {
            pcm.insert(pcm.end(), frame.begin(), frame.begin() + out.decoded_size / sizeof(int16_t));
        }
    }
    esp_opus_dec_close(dec);
    return pcm;
}

// 去掉开头的 [happy] 这类情绪标签
std::pair<std::string, std::string> SplitEmotion(const std::string& reply) {
    size_t start = reply.find_first_not_of(" \n");
    if (start != std::string::npos && reply[start] == '[') {
        size_t end = reply.find(']', start);
        if (end != std::string::npos && end - start < 16) {
            std::string emotion = reply.substr(start + 1, end - start - 1);
            size_t body = reply.find_first_not_of(" \n", end + 1);
            return {emotion, body == std::string::npos ? "" : reply.substr(body)};
        }
    }
    return {"neutral", reply};
}

// 按句切开逐句合成，第一句能尽早出声；太短的句子并到下一句
std::vector<std::string> SplitSentences(const std::string& text) {
    static const char* const kEnds[] = {"。", "！", "？", "；", "!", "?", ";", "\n"};
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < text.size();) {
        size_t len = 1;
        bool is_end = false;
        for (auto e : kEnds) {
            size_t n = strlen(e);
            if (text.compare(i, n, e) == 0) {
                len = n;
                is_end = true;
                break;
            }
        }
        cur.append(text, i, len);
        i += len;
        if (is_end && cur.size() >= 12) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (cur.find_first_not_of(" \n") != std::string::npos) {
        out.push_back(cur);
    }
    return out;
}

}  // namespace

DirectApiConfig DirectApiConfig::Load() {
    Settings s(kSettingsNs, false);
    DirectApiConfig c;
    c.llm = {TrimSlash(s.GetString("llm_url")), s.GetString("llm_key"), s.GetString("llm_model")};
    // ASR / TTS 没单独填地址或 key 时沿用 LLM 的
    c.asr = {TrimSlash(s.GetString("asr_url", c.llm.url)), s.GetString("asr_key", c.llm.key),
             s.GetString("asr_model", "whisper-1")};
    c.tts = {TrimSlash(s.GetString("tts_url", c.llm.url)), s.GetString("tts_key", c.llm.key),
             s.GetString("tts_model", "tts-1")};
    if (c.asr.url.empty()) c.asr.url = c.llm.url;
    if (c.asr.key.empty()) c.asr.key = c.llm.key;
    if (c.tts.url.empty()) c.tts.url = c.llm.url;
    if (c.tts.key.empty()) c.tts.key = c.llm.key;
    c.tts_voice = s.GetString("tts_voice", "alloy");
    c.asr_language = s.GetString("asr_lang", "zh");
    c.system_prompt = s.GetString("prompt", kDefaultPrompt);
    return c;
}

bool DirectApiConfig::IsComplete() const {
    return !llm.url.empty() && !llm.model.empty() && !asr.url.empty() && !tts.url.empty();
}

DirectApiProtocol::DirectApiProtocol() {
    server_sample_rate_ = 24000;
    server_frame_duration_ = 20;
    turn_sem_ = xSemaphoreCreateBinary();
}

DirectApiProtocol::~DirectApiProtocol() {
    if (worker_ != nullptr) {
        vTaskDelete(worker_);
    }
    vSemaphoreDelete(turn_sem_);
}

bool DirectApiProtocol::Start() {
    // TLS 握手和 JSON 解析比较吃栈
    xTaskCreate(WorkerEntry, "direct_api", 12 * 1024, this, 4, &worker_);
    config_url_ = DirectApiConfigServer::Start();
    if (!DirectApiConfig::Load().IsComplete() && !config_url_.empty()) {
        Board::GetInstance().GetDisplay()->ShowNotification("设置页：" + config_url_, 30000);
    }
    return true;
}

bool DirectApiProtocol::OpenAudioChannel() {
    if (!DirectApiConfig::Load().IsComplete()) {
        SetError("API 未配置，请用浏览器打开 " +
                 (config_url_.empty() ? std::string("设备的设置页") : config_url_) + " 填写");
        return false;
    }
    error_occurred_ = false;
    opened_ = true;
    last_incoming_time_ = std::chrono::steady_clock::now();
    if (on_audio_channel_opened_) on_audio_channel_opened_();
    return true;
}

void DirectApiProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    cancel_ = true;
    recording_ = false;
    bool was_opened = opened_.exchange(false);
    if (was_opened && on_audio_channel_closed_) on_audio_channel_closed_();
}

bool DirectApiProtocol::IsAudioChannelOpened() const { return opened_ && !error_occurred_; }

bool DirectApiProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!recording_) {
        // 随时打断：回声消除打开时，它说话期间麦克风也一直在送音频；
        // 设备端 VAD 听到用户开口超过一小会儿，就停掉这一轮，Application 会切回聆听
        if (busy_ && mode_ == kListeningModeRealtime) {
            bool voice = Application::GetInstance().GetAudioService().IsVoiceDetected();
            barge_in_ms_ = voice ? barge_in_ms_ + kMicFrameMs : 0;
            if (barge_in_ms_ >= kBargeInMs && !cancel_) {
                ESP_LOGI(TAG, "Barge-in detected");
                cancel_ = true;
            }
        }
        return true;
    }
    bool done = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        packets_.push_back(std::move(packet->payload));
        recorded_ms_ += kMicFrameMs;
        // 手动模式靠松键结束；自动模式用设备端 VAD 判断说完了没有
        if (mode_ != kListeningModeManualStop) {
            if (Application::GetInstance().GetAudioService().IsVoiceDetected()) {
                heard_voice_ = true;
                silence_ms_ = 0;
            } else if (heard_voice_) {
                silence_ms_ += kMicFrameMs;
            }
            done = heard_voice_ && silence_ms_ >= kEndSilenceMs;
        }
        done = done || recorded_ms_ >= kMaxRecordMs;
    }
    if (done) {
        FinishRecording();
    }
    return true;
}

void DirectApiProtocol::SendWakeWordDetected(const std::string& wake_word) { (void)wake_word; }

void DirectApiProtocol::SendStartListening(ListeningMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    mode_ = mode;
    packets_.clear();
    heard_voice_ = false;
    silence_ms_ = 0;
    recorded_ms_ = 0;
    if (!busy_) {
        cancel_ = false;
    }
    recording_ = true;
}

void DirectApiProtocol::SendStopListening() {
    if (recording_) {
        FinishRecording();
    }
}

void DirectApiProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    cancel_ = true;
}

void DirectApiProtocol::SendMcpMessage(const std::string& message) {
    // 工具调用以后再接（拍照识图等），现在先忽略设备端 MCP 的上报
    (void)message;
}

bool DirectApiProtocol::SendText(const std::string& text) {
    (void)text;
    return true;
}

void DirectApiProtocol::FinishRecording() {
    if (!recording_.exchange(false) || busy_) {
        return;
    }
    xSemaphoreGive(turn_sem_);
}

void DirectApiProtocol::WorkerEntry(void* arg) {
    static_cast<DirectApiProtocol*>(arg)->WorkerLoop();
}

void DirectApiProtocol::WorkerLoop() {
    while (true) {
        xSemaphoreTake(turn_sem_, portMAX_DELAY);
        std::vector<std::vector<uint8_t>> packets;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            packets.swap(packets_);
        }
        busy_ = true;
        cancel_ = false;
        barge_in_ms_ = 0;
        RunTurn(std::move(packets));
        busy_ = false;
    }
}

void DirectApiProtocol::EmitJson(const char* type, const char* key, const char* value,
                                 const char* key2, const char* value2) {
    if (!on_incoming_json_) return;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", type);
    if (key) cJSON_AddStringToObject(root, key, value);
    if (key2) cJSON_AddStringToObject(root, key2, value2);
    last_incoming_time_ = std::chrono::steady_clock::now();
    on_incoming_json_(root);
    cJSON_Delete(root);
}

void DirectApiProtocol::RunTurn(std::vector<std::vector<uint8_t>> packets) {
    auto cfg = DirectApiConfig::Load();
    auto& app = Application::GetInstance();
    // 没识别出内容时回到聆听；手动模式下 Application 会自己回到待机
    auto resume = [this]() {
        if (mode_ != kListeningModeManualStop && opened_) {
            SendStartListening(mode_);
        }
    };

    EmitJson("llm", "emotion", "thinking");
    std::string text = Transcribe(cfg, packets);
    packets.clear();
    if (cancel_) return;
    if (text.empty() && HasPendingImage()) {
        text = kDefaultImageQuestion;  // 拍了照但没说话
    }
    if (text.empty()) {
        resume();
        return;
    }
    EmitJson("stt", "text", text.c_str());

    std::string reply = Chat(cfg, text);
    if (cancel_) return;
    if (reply.empty()) {
        SetError("大模型没有返回内容");
        return;
    }
    auto [emotion, body] = SplitEmotion(reply);
    EmitJson("llm", "emotion", emotion.c_str());

    EmitJson("tts", "state", "start");
    // 等 Application 切到 Speaking，否则送进去的音频会被丢掉
    for (int i = 0; i < 50 && app.GetDeviceState() != kDeviceStateSpeaking && !cancel_; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    for (const auto& sentence : SplitSentences(body)) {
        if (cancel_) break;
        EmitJson("tts", "state", "sentence_start", "text", sentence.c_str());
        if (!Speak(cfg, sentence)) break;
    }
    // 等本轮语音播完再结束，否则自动模式会边放边开始录音
    while (!cancel_ && !app.GetAudioService().IsPlaybackIdle()) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    EmitJson("tts", "state", "stop");
}

std::string DirectApiProtocol::Transcribe(const DirectApiConfig& cfg,
                                          const std::vector<std::vector<uint8_t>>& packets) {
    auto pcm = DecodeOpus(packets);
    if (pcm.size() < kMicSampleRate / 3) {  // 不到 0.3 秒，多半是误触
        return "";
    }

    const std::string boundary = "----PocketCompanionBoundary";
    std::string body;
    auto field = [&](const char* name, const std::string& value) {
        body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + name +
                "\"\r\n\r\n" + value + "\r\n";
    };
    field("model", cfg.asr.model);
    if (!cfg.asr_language.empty()) field("language", cfg.asr_language);
    body += "--" + boundary +
            "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
            "Content-Type: audio/wav\r\n\r\n";
    body += MakeWav(pcm);
    body += "\r\n--" + boundary + "--\r\n";
    pcm.clear();
    pcm.shrink_to_fit();

    auto http = NewHttp(cfg.asr.key);
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetContent(std::move(body));
    if (!http->Open("POST", cfg.asr.url + "/audio/transcriptions")) {
        SetError("语音识别连接失败");
        return "";
    }
    auto status = http->GetStatusCode();
    std::string resp = http->ReadAll();
    if (!status || *status >= 400) {
        ESP_LOGE(TAG, "ASR failed: %d %s", status ? *status : -1, resp.substr(0, 200).c_str());
        SetError("语音识别失败");
        return "";
    }

    std::string text;
    if (cJSON* root = cJSON_Parse(resp.c_str())) {
        cJSON* t = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(t)) text = t->valuestring;
        cJSON_Delete(root);
    }
    ESP_LOGI(TAG, "ASR: %s", text.c_str());
    return text;
}

std::string DirectApiProtocol::Chat(const DirectApiConfig& cfg, const std::string& user_text) {
    history_.emplace_back("user", user_text);

    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", cfg.llm.model.c_str());
    cJSON* messages = cJSON_AddArrayToObject(req, "messages");
    auto add = [messages](const std::string& role, const std::string& content) {
        cJSON* m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", role.c_str());
        cJSON_AddStringToObject(m, "content", content.c_str());
        cJSON_AddItemToArray(messages, m);
    };
    add("system", cfg.system_prompt);
    // 历史只存文字；这一轮如果拍了照，用户消息改成"文字 + 图片"的多模态格式
    std::string image = TakePendingImage();
    for (size_t i = 0; i < history_.size(); i++) {
        if (i + 1 < history_.size() || image.empty()) {
            add(history_[i].first, history_[i].second);
            continue;
        }
        cJSON* m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "user");
        cJSON* parts = cJSON_AddArrayToObject(m, "content");
        cJSON* text_part = cJSON_CreateObject();
        cJSON_AddStringToObject(text_part, "type", "text");
        cJSON_AddStringToObject(text_part, "text", history_[i].second.c_str());
        cJSON_AddItemToArray(parts, text_part);
        cJSON* image_part = cJSON_CreateObject();
        cJSON_AddStringToObject(image_part, "type", "image_url");
        cJSON* url = cJSON_AddObjectToObject(image_part, "image_url");
        cJSON_AddStringToObject(url, "url", ("data:image/jpeg;base64," + Base64(image)).c_str());
        cJSON_AddItemToArray(parts, image_part);
        cJSON_AddItemToArray(messages, m);
        ESP_LOGI(TAG, "Sending image (%u bytes JPEG) with question", (unsigned)image.size());
    }
    char* json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    auto http = NewHttp(cfg.llm.key);
    http->SetHeader("Content-Type", "application/json");
    http->SetContent(std::string(json));
    cJSON_free(json);
    if (!http->Open("POST", cfg.llm.url + "/chat/completions")) {
        history_.pop_back();
        SetError("大模型连接失败");
        return "";
    }
    auto status = http->GetStatusCode();
    std::string resp = http->ReadAll();
    if (!status || *status >= 400) {
        ESP_LOGE(TAG, "LLM failed: %d %s", status ? *status : -1, resp.substr(0, 200).c_str());
        history_.pop_back();
        SetError("大模型请求失败");
        return "";
    }

    std::string reply;
    if (cJSON* root = cJSON_Parse(resp.c_str())) {
        cJSON* choices = cJSON_GetObjectItem(root, "choices");
        cJSON* first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
        cJSON* message = first ? cJSON_GetObjectItem(first, "message") : nullptr;
        cJSON* content = message ? cJSON_GetObjectItem(message, "content") : nullptr;
        if (cJSON_IsString(content)) reply = content->valuestring;
        cJSON_Delete(root);
    }
    ESP_LOGI(TAG, "LLM: %s", reply.c_str());

    if (reply.empty()) {
        history_.pop_back();
    } else {
        history_.emplace_back("assistant", SplitEmotion(reply).second);
        if (history_.size() > kHistoryLimit) {
            history_.erase(history_.begin(), history_.end() - kHistoryLimit);
        }
    }
    return reply;
}

// 请求 Ogg Opus，边下载边拆包送进解码队列
bool DirectApiProtocol::Speak(const DirectApiConfig& cfg, const std::string& sentence) {
    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", cfg.tts.model.c_str());
    cJSON_AddStringToObject(req, "input", sentence.c_str());
    cJSON_AddStringToObject(req, "voice", cfg.tts_voice.c_str());
    cJSON_AddStringToObject(req, "response_format", "opus");
    char* json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    auto http = NewHttp(cfg.tts.key);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Accept-Encoding", "identity");
    http->SetContent(std::string(json));
    cJSON_free(json);
    if (!http->Open("POST", cfg.tts.url + "/audio/speech")) {
        SetError("语音合成连接失败");
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status || *status >= 400) {
        ESP_LOGE(TAG, "TTS failed: %d %s", status ? *status : -1,
                 http->ReadAll().substr(0, 200).c_str());
        SetError("语音合成失败");
        return false;
    }

    auto& audio = Application::GetInstance().GetAudioService();
    OggDemuxer demuxer;
    bool stop = false;
    demuxer.OnPacket([this, &audio, &stop](const uint8_t* data, int sample_rate,
                                           int frame_duration_ms, size_t size) {
        if (stop || cancel_) {
            stop = true;
            return;
        }
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = frame_duration_ms;
        packet->payload.assign(data, data + size);
        // 下载比实时快，用阻塞方式等解码队列腾出位置，避免丢包
        if (!audio.PushPacketToDecodeQueue(std::move(packet), true)) stop = true;
    });

    std::array<char, 2048> buffer;
    while (!stop && !cancel_) {
        auto n = http->Read(buffer.data(), buffer.size());
        if (!n || *n == 0) break;
        demuxer.Process(reinterpret_cast<const uint8_t*>(buffer.data()), *n);
        if (demuxer.HasError()) {
            ESP_LOGE(TAG, "TTS response is not Ogg Opus");
            SetError("语音合成格式不支持，需要 opus");
            return false;
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    }
    return !cancel_;
}

// 发请求并返回 HTTP 状态码；连不上返回 0
static int PostStatus(Http& http, const std::string& url) {
    if (!http.Open("POST", url)) return 0;
    auto status = http.GetStatusCode();
    return status ? *status : 0;
}

void DirectApiProtocol::AttachImage(std::string jpeg) {
    std::lock_guard<std::mutex> lock(g_image_mutex);
    g_pending_image = std::move(jpeg);
}

std::string DirectApiProtocol::SelfTest() {
    auto cfg = DirectApiConfig::Load();
    if (!cfg.IsComplete()) {
        return "还没配置 API，请先在网页设置页填写";
    }
    auto describe = [](int status) {
        return status == 0 ? std::string("连不上") : "HTTP " + std::to_string(status);
    };

    // 大模型：发一句最短的话
    std::string report;
    {
        int64_t start = esp_timer_get_time();
        auto http = NewHttp(cfg.llm.key);
        http->SetHeader("Content-Type", "application/json");
        http->SetContent("{\"model\":\"" + cfg.llm.model +
                         "\",\"max_tokens\":8,\"messages\":[{\"role\":\"user\",\"content\":\"你好\"}]}");
        int status = PostStatus(*http, cfg.llm.url + "/chat/completions");
        int ms = static_cast<int>((esp_timer_get_time() - start) / 1000);
        report = status >= 200 && status < 300
                     ? "大模型：正常（" + std::to_string(ms) + " ms）"
                     : "大模型：" + describe(status) + "，检查地址 / Key / 模型";
    }

    // 语音合成：看服务能不能返回 Ogg Opus
    {
        auto http = NewHttp(cfg.tts.key);
        http->SetHeader("Content-Type", "application/json");
        http->SetContent("{\"model\":\"" + cfg.tts.model + "\",\"input\":\"测试\",\"voice\":\"" +
                         cfg.tts_voice + "\",\"response_format\":\"opus\"}");
        int status = PostStatus(*http, cfg.tts.url + "/audio/speech");
        if (status < 200 || status >= 300) {
            report += "\n语音合成：" + describe(status) + "，检查地址 / Key / 模型 / 音色";
        } else {
            char head[4] = {};
            auto n = http->Read(head, sizeof(head));
            bool ogg = n && *n == 4 && memcmp(head, "OggS", 4) == 0;
            report += ogg ? "\n语音合成：正常，支持 opus" : "\n语音合成：返回的不是 opus，换支持 opus 的服务";
        }
    }
    return report;
}
