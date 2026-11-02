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
#include <functional>
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
    DirectApiEndpoint vision;  // 带图的请求用它；没单独填就和 llm 一样
    std::string tts_voice;
    std::string asr_language;
    std::string system_prompt;
    bool tts_speak = true;  // 语音播报：关了只出字幕，不调合成接口
    bool one_shot = false;  // 一问一答：自动聆听时回答完一句就回待机，不接着听

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

    // 收一张拍好的 JPEG（self.camera.look 的拍照任务放进来的），这一轮的请求会带上它发给视觉模型
    static void AttachImage(std::string jpeg);

    // 表情脚本（板子支持才设置）：face_prompt 加进系统提示词；回复里的 <face>…</face> 交给 face_handler 播放
    static std::string face_prompt;
    static std::function<bool(const std::string& json, std::string& error)> face_handler;

    // 正在"思考"：一句话录完（已停止收音）到开口说话之间——识别、大模型、工具调用、拍照都在这段。
    // 设备状态这时仍是聆听（自动模式）或待机（松键后），屏幕靠它显示"思考中"
    static bool IsThinking() { return thinking_; }

protected:
    bool SendText(const std::string& text) override;

private:
    static std::atomic<bool> thinking_;
    std::atomic<bool> opened_{false};
    std::atomic<bool> recording_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<bool> busy_{false};
    // self.chat.end 置位：这一轮说完就退出聆听回待机，而不是接着听
    std::atomic<bool> exit_after_reply_{false};
    ListeningMode mode_ = kListeningModeAutoStop;
    std::string config_url_;  // 网页设置页地址

    // 这一轮已作废：被用户打断，或音频通道已关闭（退出聆听 / 断网）。
    // 作废的轮次不再发请求，晚到的失败也不弹错误提示
    bool Aborted() const { return cancel_ || !opened_; }

    std::mutex mutex_;
    // 本轮录音的 Opus 包（16k，60ms），每包前面 2 字节长度，连成一整块放 PSRAM。
    // 原来一包一个 vector：每个一两百字节，全落在内部 RAM，15 秒就是两百多块，
    // 把连续对话本来就紧的内部 RAM 吃掉、切碎，上传录音的 TLS 就发不出去（语音识别连接失败）
    std::string packets_;
    bool heard_voice_ = false;
    int silence_ms_ = 0;
    int voice_ms_ = 0;    // 本轮里 VAD 连续听到声音多久了（要够 kMinVoiceMs 才算有人说话）
    int recorded_ms_ = 0;
    int barge_in_ms_ = 0;  // 它说话时用户连续开口多久了（随时打断模式）

    std::vector<std::pair<std::string, std::string>> history_;  // role, content
    std::string pending_face_;  // 这一轮回复里取出的表情脚本，开口说话时播
    SemaphoreHandle_t turn_sem_ = nullptr;
    TaskHandle_t worker_ = nullptr;

    void FinishRecording();
    static void WorkerEntry(void* arg);
    void WorkerLoop();
    void RunTurn(std::string packets);

    void EmitJson(const char* type, const char* key, const char* value,
                  const char* key2 = nullptr, const char* value2 = nullptr);
    std::string Transcribe(const DirectApiConfig& cfg, std::string packets);
    std::string Chat(const DirectApiConfig& cfg, const std::string& user_text);
    bool Speak(const DirectApiConfig& cfg, const std::string& sentence);
};

#endif  // DIRECT_API_PROTOCOL_H
