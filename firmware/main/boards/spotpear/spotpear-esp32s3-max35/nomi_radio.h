// 网络电台：http(s) 上的 MP3 / AAC 直播流，边收边解边放。
//
// 收流和解码各一个常驻任务，栈都在 PSRAM；网络数据先进 PSRAM 环形缓冲，攒够一点再开始放，
// 网络抖一下不至于断音。设备不在待机（有人叫它说话）或闹钟在响时停掉、连接也断开，
// 把内部 RAM 让给语音识别；回到待机后自动接着放（直播，没有进度可接）。
// 台单存 NVS（nomi_radio 命名空间的 list，JSON），网页设置页和大模型工具都能增删。
#pragma once

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class NomiRadio {
public:
    enum State { kOff, kConnecting, kPlaying, kHeld, kError };
    struct Station {
        std::string name, url;
    };

    static NomiRadio& GetInstance();

    void Start();  // 读台单、建任务和监视定时器；板子构造时调用一次
    void RegisterTools();

    // 屏幕上的按钮
    void Play(int index);  // 放台单里的第几个
    void Toggle();         // 放 / 停
    void Step(int delta);  // 下一个 / 上一个
    void Stop();
    // 闹钟响时先停，关了闹钟接着放
    void SetAlarmHold(bool hold) { alarm_hold_ = hold; }

    State GetState();
    std::string StationName();
    std::string Title();     // 流里带的歌名（ICY StreamTitle），没有就空
    int CurrentIndex();      // 台单里的下标；搜来还没收藏的台是 -1
    std::string ListText();  // 台名，一行一个
    std::string PromptContext();

    // 网页设置页：GET /radio.json、POST /radio/add、POST /radio/delete
    std::string ListJson();
    std::string AddFromJson(const std::string& body);     // 出错时返回 "!原因"
    std::string DeleteFromJson(const std::string& body);  // 同上
    std::string PlayFromJson(const std::string& body);    // {"index":n} 放第几个，{"stop":true} 停

private:
    std::mutex mutex_;
    std::vector<Station> list_;
    Station current_;
    int current_index_ = 0;
    std::string title_;
    std::atomic<bool> wanted_{false};      // 主人要它放着（对话、闹钟时暂停，之后接着放）
    std::atomic<bool> alarm_hold_{false};
    std::atomic<State> state_{kOff};

    // 一次播放（session）：监视定时器开始 / 停止，两个任务各跑各的循环
    std::atomic<bool> running_{false};   // 这次播放在进行
    std::atomic<bool> stopping_{false};  // 让两个任务收手
    std::atomic<int> busy_{0};           // 还在干活的任务数（都回到等待才能开下一次）
    std::atomic<bool> reader_done_{false};
    int64_t idle_since_us_ = 0;
    bool wifi_awake_ = false;  // 放的时候把 Wi-Fi 省电关了，停了要还原
    int64_t retry_at_us_ = 0;
    int failures_ = 0;
    SemaphoreHandle_t reader_go_ = nullptr, decoder_go_ = nullptr;
    StreamBufferHandle_t stream_ = nullptr;
    std::atomic<int> codec_{0};  // 1 MP3，2 AAC
    esp_timer_handle_t timer_ = nullptr;

    NomiRadio() = default;
    void LoadList();
    void SaveList();  // 调用方持有 mutex_
    void Monitor();
    void BeginSession();
    void EndSession();
    void ReaderLoop();
    void DecoderLoop();
    bool ReadStream(const std::string& url, int redirects);
    void Select(const Station& station, int index);
    bool Search(const std::string& query, Station& found);
};
