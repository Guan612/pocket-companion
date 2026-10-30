// 直连 OpenAI 兼容 API 的协议实现：不经过小智服务器，设备自己调
// ASR（/audio/transcriptions）→ LLM（/chat/completions）→ TTS（/audio/speech）。
//
// 对 Application 来说它和 WebsocketProtocol 一样：录音通过 SendAudio 进来，
// 识别结果、情绪、TTS 起止用 on_incoming_json_ 模拟服务端消息发回去，
// TTS 音频（Ogg Opus）解出 Opus 包后直接送进解码队列。
// 配置存在 NVS 的 "directapi" 命名空间，由 DirectApiConfigServer 的网页写入。
#ifndef DIRECT_API_PROTOCOL_H
#define DIRECT_API_PROTOCOL_H

#include "protocol.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct DirectApiEndpoint {
    std::string url;  // base URL，例如 https://api.openai.com/v1
    std::string key;
    std::string model;
};

struct DirectApiConfig {
    DirectApiEndpoint asr, llm, tts;
    std::string tts_voice;
    std::string asr_language;
    std::string system_prompt;

    static DirectApiConfig Load();
    bool IsComplete() const;
};

class DirectApiProtocol : public Protocol {
public:
    DirectApiProtocol();
    ~DirectApiProtocol();

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    void SendWakeWordDetected(const std::string& wake_word) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendMcpMessage(const std::string& message) override;

    // 用当前配置试调一次大模型和语音合成，返回给人看的结果（会阻塞几秒）
    static std::string SelfTest();

    // 附上一张 JPEG，下一轮提问时和问题一起发给大模型（多模态）；没说话就默认问看到了什么
    static void AttachImage(std::string jpeg);

protected:
    bool SendText(const std::string& text) override;

private:
    std::atomic<bool> opened_{false};
    std::atomic<bool> recording_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> busy_{false};
    ListeningMode mode_ = kListeningModeAutoStop;
    std::string config_url_;  // 网页设置页地址

    std::mutex mutex_;
    std::vector<std::vector<uint8_t>> packets_;  // 本轮录音的 Opus 包（16k，60ms）
    bool heard_voice_ = false;
    int silence_ms_ = 0;
    int recorded_ms_ = 0;
    int barge_in_ms_ = 0;  // 它说话时用户连续开口多久了（随时打断模式）

    std::vector<std::pair<std::string, std::string>> history_;  // role, content
    SemaphoreHandle_t turn_sem_ = nullptr;
    TaskHandle_t worker_ = nullptr;

    void FinishRecording();
    static void WorkerEntry(void* arg);
    void WorkerLoop();
    void RunTurn(std::vector<std::vector<uint8_t>> packets);

    void EmitJson(const char* type, const char* key, const char* value,
                  const char* key2 = nullptr, const char* value2 = nullptr);
    std::string Transcribe(const DirectApiConfig& cfg,
                           const std::vector<std::vector<uint8_t>>& packets);
    std::string Chat(const DirectApiConfig& cfg, const std::string& user_text);
    bool Speak(const DirectApiConfig& cfg, const std::string& sentence);
};

#endif  // DIRECT_API_PROTOCOL_H
