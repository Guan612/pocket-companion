#include "direct_api_protocol.h"

#include "application.h"
#include "board.h"
#include "direct_api_config_server.h"
#include "display.h"
#include "http.h"
#include "long_term_memory.h"
#include "mcp_server.h"
#include "ogg_demuxer.h"
#include "direct_api_settings.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_audio_dec_default.h>
#include <esp_audio_simple_dec.h>
#include <esp_audio_simple_dec_default.h>
#include <esp_log.h>
#include <esp_opus_dec.h>
#include <esp_timer.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

#define TAG "DirectApi"

namespace {

constexpr int kMicSampleRate = 16000;      // 小智上行 Opus 的采样率
constexpr int kMicFrameMs = 60;            // 小智上行 Opus 的帧长
constexpr int kEndSilenceMs = 900;         // 说完后静音多久算一句结束
constexpr int kBargeInMs = 300;            // 它说话时用户开口多久算打断
constexpr int kMaxRecordMs = 15000;
constexpr size_t kHistoryLimit = 12;
constexpr int kHttpTimeoutMs = 30000;
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

// 从回复里取出 <face>…</face> 表情脚本并删掉；没写结束标签时按括号配对找 JSON 的结尾
std::string ExtractFace(std::string& reply) {
    const size_t open = reply.find("<face>");
    if (open == std::string::npos) return "";
    const size_t start = open + 6;
    size_t end = reply.find("</face>", start), stop = end == std::string::npos ? end : end + 7;
    if (end == std::string::npos) {
        int depth = 0;
        bool in_string = false;
        for (size_t i = start; i < reply.size() && end == std::string::npos; i++) {
            char c = reply[i];
            if (in_string) {
                if (c == '\\') i++;
                else if (c == '"') in_string = false;
            } else if (c == '"') {
                in_string = true;
            } else if (c == '[' || c == '{') {
                depth++;
            } else if ((c == ']' || c == '}') && --depth == 0) {
                end = stop = i + 1;
            }
        }
        if (end == std::string::npos) {  // 写了一半：整段丢掉，免得把 JSON 念出来
            reply.erase(open);
            return "";
        }
    }
    std::string json = reply.substr(start, end - start);
    reply.erase(open, stop - open);
    return json;
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

// 各用途从网页设置页选的"供应商 + 模型"解析出地址 / Key / 模型（存储格式见 direct_api_settings.h）
DirectApiConfig DirectApiConfig::Load() {
    using namespace DirectApiSettings;
    Settings s(kNs, false);
    auto endpoint = [&s](const Role& role) {
        Endpoint e = ResolveRole(s, role);
        return DirectApiEndpoint{e.url, e.key, e.model};
    };
    DirectApiConfig c;
    c.llm = endpoint(kChat);
    c.asr = endpoint(kAsr);
    c.tts = endpoint(kTts);
    c.vision = endpoint(kVision);
    if (c.vision.url.empty() || c.vision.model.empty()) c.vision = c.llm;  // 没单独选就用对话模型
    // 留空的项按默认值处理（网页每次都提交所有字段，留空存的是空字符串）
    c.tts_voice = s.GetString(kTtsVoice);
    c.asr_language = s.GetString(kAsrLang);
    c.system_prompt = s.GetString(kPrompt);
    if (c.tts_voice.empty()) c.tts_voice = "alloy";
    if (c.asr_language.empty()) c.asr_language = "zh";
    if (c.system_prompt.empty()) c.system_prompt = kDefaultPrompt;
    return c;
}

// 对话、语音识别、语音合成都要选好模型才能说话
bool DirectApiConfig::IsComplete() const {
    return !llm.url.empty() && !llm.model.empty() && !asr.url.empty() && !asr.model.empty() &&
           !tts.url.empty() && !tts.model.empty();
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
    // 直连模式自己的本地工具：长期记忆（设备端 MCP 工具的桥接见 Chat()）
    McpServer::GetInstance().AddTool(
        "self.memory.remember",
        "把主人希望长期记住的事存进设备的长期记忆。适合：主人告诉你名字/称呼、喜好、固定安排，"
        "或明确说\"记住\"。一次记一条简短的事实，闲聊内容不要记。",
        PropertyList({Property("fact", kPropertyTypeString).SetMaxLength(200)}),
        [](const PropertyList& properties) -> ToolResult {
            auto fact = properties["fact"].value<std::string>();
            if (!LongTermMemory::Append(fact)) {
                return std::unexpected("长期记忆已满，请提示主人到网页设置页整理");
            }
            return std::string("已记住");
        });

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
    // 视觉模式：说完话的这一刻拍一张，和问题一起发
    if (!text.empty() && !HasPendingImage() && image_provider) {
        std::string jpeg = image_provider();
        if (!jpeg.empty()) AttachImage(std::move(jpeg));
    }
    if (text.empty() && HasPendingImage()) {
        text = kDefaultImageQuestion;  // 拍了照但没说话
    }
    if (text.empty()) {
        EmitJson("llm", "emotion", "neutral");  // 没听清：结束"思考中"（它会一直保持到有结果）
        resume();
        return;
    }
    EmitJson("stt", "text", text.c_str());

    std::string reply = Chat(cfg, text);
    if (cancel_) return;
    if (reply.empty() && pending_face_.empty()) {  // 只做了表情、没说话也算正常
        SetError("大模型没有返回内容");
        return;
    }
    auto [emotion, body] = SplitEmotion(reply);
    EmitJson("llm", "emotion", emotion.c_str());
    if (!pending_face_.empty() && face_handler) {
        std::string error;
        if (!face_handler(pending_face_, error)) ESP_LOGW(TAG, "Face script rejected: %s", error.c_str());
        pending_face_.clear();
    }

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
        SetError("语音识别失败（HTTP " + std::to_string(status ? *status : 0) + "）");
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

    // 这一轮带图就用视觉模型（可在网页设置页单独配置，默认和对话模型一样）
    std::string image = TakePendingImage();
    const DirectApiEndpoint& ep = image.empty() ? cfg.llm : cfg.vision;

    // 长期记忆拼进系统提示词；带工具时补一句使用指引
    auto build_system = [&cfg](bool with_tools) {
        std::string sys = cfg.system_prompt;
        std::string memory = LongTermMemory::Get();
        if (!memory.empty()) {
            sys += "\n\n[主人的长期记忆]（回答时自然运用，不要整段复述）\n" + memory;
        }
        if (!face_prompt.empty()) sys += "\n\n" + face_prompt;
        if (with_tools) {
            sys +=
                "\n\n你可以调用设备工具：调音量 / 亮度、查设备状态用对应工具；"
                "主人说了值得长期记住的信息（称呼、喜好、安排，或明确说\"记住\"）时，"
                "调用 self.memory.remember，一次一条简短事实。";
        }
        return sys;
    };

    static bool tools_broken = false;  // 服务不支持 function calling 时，这次开机不再带 tools
    auto build_request = [&](bool with_tools) {
        cJSON* req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "model", ep.model.c_str());
        cJSON* messages = cJSON_AddArrayToObject(req, "messages");
        auto add = [messages](const std::string& role, const std::string& content) {
            cJSON* m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", role.c_str());
            cJSON_AddStringToObject(m, "content", content.c_str());
            cJSON_AddItemToArray(messages, m);
        };
        add("system", build_system(with_tools));
        // 历史只存文字；这一轮如果拍了照，用户消息改成"文字 + 图片"的多模态格式
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
        // 设备端 MCP 工具 → OpenAI function calling；带图的请求走视觉模型，不带 tools
        if (with_tools) {
            cJSON* tools = cJSON_Parse(McpServer::GetInstance()
                                           .GetOpenAiToolsJson({"self.camera.take_photo"})
                                           .c_str());
            if (cJSON_IsArray(tools) && cJSON_GetArraySize(tools) > 0) {
                cJSON_AddItemToObject(req, "tools", tools);
            } else {
                cJSON_Delete(tools);
            }
        }
        return req;
    };

    bool with_tools = image.empty() && !tools_broken;
    cJSON* req = build_request(with_tools);
    std::string reply;
    for (int round = 0; round < 4 && reply.empty(); round++) {
        char* json = cJSON_PrintUnformatted(req);
        auto http = NewHttp(ep.key);
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::string(json));
        cJSON_free(json);
        if (!http->Open("POST", ep.url + "/chat/completions")) {
            cJSON_Delete(req);
            history_.pop_back();
            SetError("大模型连接失败");
            return "";
        }
        auto status = http->GetStatusCode();
        std::string resp = http->ReadAll();
        if (!status || *status >= 400) {
            if (with_tools && status && *status < 500) {
                // 多半是不支持 function calling：去掉 tools 重试一次，这个开机内不再带
                ESP_LOGW(TAG, "LLM rejected tools (HTTP %d), retrying without them", *status);
                tools_broken = true;
                cJSON_Delete(req);
                req = build_request(false);
                with_tools = false;
                round--;  // 重试不占轮数
                continue;
            }
            ESP_LOGE(TAG, "LLM failed: %d %s", status ? *status : -1, resp.substr(0, 200).c_str());
            cJSON_Delete(req);
            history_.pop_back();
            SetError("大模型请求失败（HTTP " + std::to_string(status ? *status : 0) + "）");
            return "";
        }

        cJSON* root = cJSON_Parse(resp.c_str());
        cJSON* choices = root ? cJSON_GetObjectItem(root, "choices") : nullptr;
        cJSON* first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
        cJSON* message = first ? cJSON_GetObjectItem(first, "message") : nullptr;
        cJSON* content = message ? cJSON_GetObjectItem(message, "content") : nullptr;
        cJSON* tool_calls = message ? cJSON_GetObjectItem(message, "tool_calls") : nullptr;
        if (!cJSON_IsArray(tool_calls) || cJSON_GetArraySize(tool_calls) == 0) {
            if (cJSON_IsString(content)) reply = content->valuestring;
            cJSON_Delete(root);
            break;
        }
        if (message == nullptr) {
            cJSON_Delete(root);
            break;
        }
        // 模型要用工具：assistant 消息原样回填（tool_calls 的 id 不能变），再补 tool 结果
        ESP_LOGI(TAG, "LLM wants %d tool call(s)", cJSON_GetArraySize(tool_calls));
        cJSON* msgs = cJSON_GetObjectItem(req, "messages");
        cJSON_AddItemToArray(msgs, cJSON_Duplicate(message, true));
        cJSON* tc = nullptr;
        cJSON_ArrayForEach(tc, tool_calls) {
            auto id = cJSON_GetObjectItem(tc, "id");
            auto fn = cJSON_GetObjectItem(tc, "function");
            auto fname = fn ? cJSON_GetObjectItem(fn, "name") : nullptr;
            auto fargs = fn ? cJSON_GetObjectItem(fn, "arguments") : nullptr;
            if (!cJSON_IsString(id) || !cJSON_IsString(fname)) {
                ESP_LOGW(TAG, "Malformed tool_call, skipping");
                break;
            }
            std::string result = McpServer::GetInstance().CallToolLocal(
                fname->valuestring, cJSON_IsString(fargs) ? fargs->valuestring : "{}");
            ESP_LOGI(TAG, "Tool %s -> %s", fname->valuestring, result.substr(0, 120).c_str());
            cJSON* tr = cJSON_CreateObject();
            cJSON_AddStringToObject(tr, "role", "tool");
            cJSON_AddStringToObject(tr, "tool_call_id", id->valuestring);
            cJSON_AddStringToObject(tr, "content", result.c_str());
            cJSON_AddItemToArray(msgs, tr);
        }
        cJSON_Delete(root);
        // 下一轮带着 tool 结果重新请求
    }
    cJSON_Delete(req);
    ESP_LOGI(TAG, "LLM: %s", reply.c_str());
    pending_face_ = ExtractFace(reply);  // 表情脚本不进历史，也不念出来

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

namespace {

// 语音合成先要 opus（设备原生，最省事）；服务回 4xx 说不支持，就改要 mp3，这次开机内记住
std::atomic<bool> g_tts_mp3{false};
std::once_flag g_audio_dec_once;

enum class SpeechFormat { kOgg, kWav, kMp3, kUnknown };

// 按开头几个字节认格式：不少服务不管 response_format，总是回 mp3
SpeechFormat SniffSpeech(const uint8_t* p, size_t n) {
    if (n >= 4 && memcmp(p, "OggS", 4) == 0) return SpeechFormat::kOgg;
    if (n >= 4 && memcmp(p, "RIFF", 4) == 0) return SpeechFormat::kWav;
    if (n >= 3 && memcmp(p, "ID3", 3) == 0) return SpeechFormat::kMp3;
    if (n >= 2 && p[0] == 0xFF && (p[1] & 0xE0) == 0xE0) return SpeechFormat::kMp3;  // MPEG 帧同步字
    return SpeechFormat::kUnknown;
}

// 发合成请求；连不上返回 nullptr，否则 status 是 HTTP 状态码
std::unique_ptr<Http> RequestSpeech(const DirectApiConfig& cfg, const std::string& text, bool mp3, int& status) {
    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", cfg.tts.model.c_str());
    cJSON_AddStringToObject(req, "input", text.c_str());
    cJSON_AddStringToObject(req, "voice", cfg.tts_voice.c_str());
    cJSON_AddStringToObject(req, "response_format", mp3 ? "mp3" : "opus");
    char* json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    auto http = NewHttp(cfg.tts.key);
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("Accept-Encoding", "identity");
    http->SetContent(std::string(json));
    cJSON_free(json);
    status = 0;
    if (!http->Open("POST", cfg.tts.url + "/audio/speech")) return nullptr;
    auto s = http->GetStatusCode();
    status = s ? *s : 0;
    return http;
}

// 先要 opus，被 4xx 拒了就换 mp3 再要一次（并记住）
std::unique_ptr<Http> RequestSpeechWithFallback(const DirectApiConfig& cfg, const std::string& text, int& status) {
    auto http = RequestSpeech(cfg, text, g_tts_mp3, status);
    if (http && !g_tts_mp3 && status >= 400 && status < 500) {
        ESP_LOGW(TAG, "TTS rejected opus (HTTP %d: %s), switching to mp3", status,
                 http->ReadAll().substr(0, 120).c_str());
        g_tts_mp3 = true;
        http = RequestSpeech(cfg, text, true, status);
    }
    return http;
}

}  // namespace

// 语音合成，边下载边播：Ogg Opus 拆包进解码队列（和小智服务器下发的一样）；
// mp3 / wav 在这里用乐鑫音频库解成 PCM，直接进播放队列
bool DirectApiProtocol::Speak(const DirectApiConfig& cfg, const std::string& sentence) {
    int status = 0;
    auto http = RequestSpeechWithFallback(cfg, sentence, status);
    if (!http) {
        SetError("语音合成连接失败");
        return false;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "TTS failed: %d %s", status, http->ReadAll().substr(0, 200).c_str());
        SetError("语音合成失败（HTTP " + std::to_string(status) + "）");
        return false;
    }

    // 先读到能认出格式的几个字节
    std::array<char, 2048> buffer;
    std::string head;
    while (head.size() < 4 && !cancel_) {
        auto n = http->Read(buffer.data(), buffer.size());
        if (!n || *n <= 0) break;
        head.append(buffer.data(), *n);
    }
    if (cancel_) return false;
    if (head.empty()) return true;  // 空响应：这句没声音，接着下一句
    const auto* h = reinterpret_cast<const uint8_t*>(head.data());
    const SpeechFormat format = SniffSpeech(h, head.size());
    if (format == SpeechFormat::kUnknown) {
        ESP_LOGE(TAG, "TTS response format unknown: %02x %02x %02x %02x", h[0], head.size() > 1 ? h[1] : 0,
                 head.size() > 2 ? h[2] : 0, head.size() > 3 ? h[3] : 0);
        SetError("语音合成返回的格式认不出（要 opus / mp3 / wav）");
        return false;
    }

    // 把已经读到的开头和后面的数据依次交给 feed；feed 返回 false 就停
    auto pump = [&](auto&& feed) {
        if (!feed(h, head.size())) return false;
        while (!cancel_) {
            auto n = http->Read(buffer.data(), buffer.size());
            if (!n || *n <= 0) break;
            last_incoming_time_ = std::chrono::steady_clock::now();
            if (!feed(reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<size_t>(*n))) return false;
        }
        return true;
    };
    auto& audio = Application::GetInstance().GetAudioService();

    if (format == SpeechFormat::kOgg) {
        OggDemuxer demuxer;
        bool stop = false;
        demuxer.OnPacket([this, &audio, &stop](const uint8_t* data, int sample_rate, int frame_duration_ms,
                                               size_t size) {
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
        bool bad = false;
        pump([&](const uint8_t* p, size_t n) {
            demuxer.Process(p, n);
            bad = demuxer.HasError();
            return !bad && !stop;
        });
        if (bad) {
            ESP_LOGE(TAG, "TTS Ogg stream is not Opus");
            SetError("语音合成返回的 Ogg 不是 opus");
            return false;
        }
        return !cancel_;
    }

    // mp3 / wav：simple decoder 支持任意大小的输入块，边收边解
    std::call_once(g_audio_dec_once, []() {
        esp_audio_dec_register_default();
        esp_audio_simple_dec_register_default();
    });
    esp_audio_simple_dec_cfg_t dec_cfg = {};
    dec_cfg.dec_type = format == SpeechFormat::kWav ? ESP_AUDIO_SIMPLE_DEC_TYPE_WAV : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    esp_audio_simple_dec_handle_t dec = nullptr;
    if (esp_audio_simple_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK || dec == nullptr) {
        ESP_LOGE(TAG, "Failed to open %s decoder", format == SpeechFormat::kWav ? "wav" : "mp3");
        SetError("语音合成解码器打不开");
        return false;
    }
    std::vector<uint8_t> out(4608);  // 一帧 mp3 最多 1152 个采样 × 双声道 × 16 位
    std::vector<int16_t> pending;    // 攒够约 60ms 再送进播放队列，和 Opus 一帧差不多
    int rate = 0, channels = 1;
    bool failed = false, aborted = false;

    auto flush = [&](bool all) {
        const size_t chunk = static_cast<size_t>(rate) * 60 / 1000;
        while (!aborted && rate > 0 && !pending.empty() && (all || pending.size() >= chunk)) {
            const size_t n = pending.size() < chunk || all ? pending.size() : chunk;
            std::vector<int16_t> part(pending.begin(), pending.begin() + n);
            pending.erase(pending.begin(), pending.begin() + n);
            if (!audio.PushPcmToPlaybackQueue(std::move(part), rate, true)) aborted = true;  // 被打断了
        }
    };
    auto decode = [&](const uint8_t* p, size_t n, bool eos) {
        esp_audio_simple_dec_raw_t raw = {};
        raw.buffer = const_cast<uint8_t*>(p);
        raw.len = n;
        raw.eos = eos;
        while ((raw.len > 0 || eos) && !cancel_ && !aborted) {
            esp_audio_simple_dec_out_t frame = {};
            frame.buffer = out.data();
            frame.len = out.size();
            auto ret = esp_audio_simple_dec_process(dec, &raw, &frame);
            if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                out.resize(frame.needed_size);
                continue;
            }
            if (ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "TTS decode failed: %d", ret);
                failed = true;
                return false;
            }
            if (frame.decoded_size > 0) {
                if (rate == 0) {
                    esp_audio_simple_dec_info_t info = {};
                    esp_audio_simple_dec_get_info(dec, &info);
                    if (info.bits_per_sample != 16) {
                        ESP_LOGE(TAG, "TTS audio is %d-bit, only 16-bit supported", info.bits_per_sample);
                        failed = true;
                        return false;
                    }
                    rate = info.sample_rate;
                    channels = info.channel ? info.channel : 1;
                    ESP_LOGI(TAG, "TTS %s: %d Hz, %d ch", format == SpeechFormat::kWav ? "wav" : "mp3", rate, channels);
                }
                const auto* s = reinterpret_cast<const int16_t*>(frame.buffer);
                const size_t samples = frame.decoded_size / sizeof(int16_t) / channels;
                for (size_t i = 0; i < samples; i++) {  // 多声道混成单声道
                    int sum = 0;
                    for (int c = 0; c < channels; c++) sum += s[i * channels + c];
                    pending.push_back(static_cast<int16_t>(sum / channels));
                }
                flush(false);
            }
            raw.buffer += raw.consumed;
            raw.len -= raw.consumed;
            if (raw.consumed == 0 && frame.decoded_size == 0) break;  // 不够一帧，等下一块
            if (eos && raw.len == 0 && frame.decoded_size == 0) break;
        }
        return !failed && !aborted;
    };

    bool ok = pump([&](const uint8_t* p, size_t n) { return decode(p, n, false); });
    if (ok && !cancel_) decode(nullptr, 0, true);  // 把解码器里剩下的冲出来
    if (!failed && !cancel_) flush(true);
    esp_audio_simple_dec_close(dec);
    if (failed) {
        SetError("语音合成解码失败");
        return false;
    }
    return !cancel_ && !aborted;
}

// 发请求并返回 HTTP 状态码；连不上返回 0
static int PostStatus(Http& http, const std::string& url) {
    if (!http.Open("POST", url)) return 0;
    auto status = http.GetStatusCode();
    return status ? *status : 0;
}

std::function<std::string()> DirectApiProtocol::image_provider;
std::string DirectApiProtocol::face_prompt;
std::function<bool(const std::string&, std::string&)> DirectApiProtocol::face_handler;

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

    // 语音合成：和 Speak 一样先要 opus、不行再要 mp3，看实际回来的是什么格式
    {
        int status = 0;
        auto http = RequestSpeechWithFallback(cfg, "测试", status);
        if (!http || status < 200 || status >= 300) {
            report += "\n语音合成：" + describe(http ? status : 0) + "，检查地址 / Key / 模型 / 音色";
        } else {
            uint8_t head[4] = {};
            auto n = http->Read(reinterpret_cast<char*>(head), sizeof(head));
            switch (SniffSpeech(head, n ? static_cast<size_t>(*n) : 0)) {
                case SpeechFormat::kOgg: report += "\n语音合成：正常（opus）"; break;
                case SpeechFormat::kMp3: report += "\n语音合成：正常（mp3，设备自己解码）"; break;
                case SpeechFormat::kWav: report += "\n语音合成：正常（wav）"; break;
                default: report += "\n语音合成：返回的格式认不出，要 opus / mp3 / wav"; break;
            }
        }
    }
    return report;
}
