// 网络电台和播客节目的播放器：http(s) 上的 MP3 / AAC 直播流，或者一期播客（MP3 / m4a），边收边解边放。
//
// 收流和解码各一个常驻任务，栈都在 PSRAM；网络数据先进 PSRAM 环形缓冲，攒够一点再开始放，
// 网络抖一下不至于断音。设备不在待机（有人叫它说话）或闹钟在响时停掉、连接也断开，
// 把内部 RAM 让给语音识别；回到待机后自动接着放（电台是直播，播客从停下的地方接着放）。
// 电台台单存 NVS（nomi_radio 命名空间的 list，JSON），网页设置页和大模型工具都能增删；
// 播客的订阅、节目单、进度归 NomiPodcast 管，它只把要放的那期交给这里；在线音乐的歌（NomiMusic）也一样。
#pragma once

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class NomiRadio {
public:
    // kHeld：对话 / 闹钟时暂停着；kLoading：为拉节目单等让出了 https，拉完接着放
    enum State { kOff, kConnecting, kPlaying, kHeld, kError, kLoading };
    struct Station {
        std::string name, url;
        // 播客节目：从 start 秒开始放，能暂停接着放、快进快退
        bool episode = false;
        int start = 0;
        int duration = 0;       // 总长（秒），不知道为 0
        int bytes_per_sec = 0;  // MP3 按时间跳转用：文件大小 / 时长；不知道按 128kbps 算
        std::string podcast;    // 节目所属播客名（显示用）；歌是歌手
        std::string key;        // 进度记在哪（NomiPodcast 用）
        bool song = false;      // 一首歌（NomiMusic，在线或本地）：不记进度，放完交给 on_finished 接下一首
        bool local = false;     // SD 卡上的文件：url 是设备上的路径（/sd/…），不用联网；对话时只暂停不断开
        std::string owner;      // 哪个应用包放的（应用 id）；空 = 系统自己的电台、播客、在线点歌
        // 地址和响应头看不出真实格式时（m4a 的扩展名写成 .aac、类型是 octet-stream），调用方先探好填这里
        enum Format { kAuto, kMp3, kAdts, kMp4, kFlac, kWav } format = kAuto;
    };

    static NomiRadio& GetInstance();
    // 网上的音频按文件头（至少 12 字节）认格式：扩展名、响应头常对不上（m4a 写成 .aac、类型是 octet-stream）。
    // 认不出（不是 MP3 / AAC / m4a）返回 kAuto
    static Station::Format SniffFormat(const std::string& head);

    void Start();  // 读台单、建任务和监视定时器；板子构造时调用一次
    void RegisterTools();

    // 屏幕上的按钮
    void Play(int index);  // 放台单里的第几个
    void Toggle();         // 放 / 停（节目是暂停，记住位置）
    void Step(int delta);  // 下一个 / 上一个台
    void Stop();
    // 闹钟响时先停，关了闹钟接着放
    void SetAlarmHold(bool hold) { alarm_hold_ = hold; }
    // 要另开一条 https（拉节目单等）时先让出来：正在放的若是 https，暂停到 ReleaseHttpsHold，回来从原位置接着放。
    // 内部 RAM 只够同时一条 TLS（两条时只剩 10KB 左右）。返回 true 表示暂停了（已经断开），用完要 Release。
    // 会等最多 5 秒，别在界面任务里调
    bool HoldForHttps();
    void ReleaseHttpsHold();

    // 播客
    void PlayEpisode(const Station& episode);
    void Seek(int delta_seconds);  // 快进 / 快退（只对节目有用）
    void SeekTo(int second);
    bool IsEpisode();
    Station Current();
    int Position() { return position_; }  // 节目放到第几秒
    // 节目的进度：每 30 秒、暂停、放完时在主循环里调（finished 为放完）
    std::function<void(const Station& episode, int position, bool finished)> on_progress;
    // 一首歌 / 一期节目从头放到尾放完了：在主循环里调（应用包放的，通知那个应用接下一首）
    std::function<void(const Station& item)> on_finished;

    State GetState();
    bool Wanted() { return wanted_; }
    // 还在放 url 这个的话，换显示的名字 / 歌手（本地歌读到标签后用；空的不换）
    void UpdateMeta(const std::string& url, const std::string& name, const std::string& artist);
    std::string StationName();
    std::string Title();     // 流里带的歌名（ICY StreamTitle），没有就空
    int CurrentIndex();      // 台单里的下标；搜来还没收藏的台、播客节目是 -1
    std::string ListText();  // 台名，一行一个
    std::string PromptContext();

    // 网页设置页：GET /radio.json、POST /radio/add、POST /radio/delete、POST /radio/move、POST /radio/play
    std::string ListJson();
    std::string AddFromJson(const std::string& body);     // 出错时返回 "!原因"
    std::string DeleteFromJson(const std::string& body);  // 同上
    std::string MoveFromJson(const std::string& body);    // {"index":n,"to":m} 把第 n 个挪到第 m 个（排序）
    std::string PlayFromJson(const std::string& body);    // {"index":n} 放第几个，{"stop":true} 停

private:
    std::mutex mutex_;
    std::vector<Station> list_;
    Station current_;
    int current_index_ = 0;
    std::string title_;
    std::atomic<bool> wanted_{false};      // 主人要它放着（对话、闹钟时暂停，之后接着放）
    std::atomic<bool> alarm_hold_{false};
    std::atomic<int> https_hold_{0};
    int64_t https_hold_since_us_ = 0;
    std::atomic<State> state_{kOff};
    std::atomic<int> position_{0};

    // 一次播放（session）：监视定时器开始 / 停止，两个任务各跑各的循环
    std::atomic<bool> running_{false};   // 这次播放在进行
    std::atomic<bool> stopping_{false};  // 让两个任务收手
    std::atomic<int> busy_{0};           // 还在干活的任务数（都回到等待才能开下一次）
    std::atomic<bool> reader_done_{false};
    std::atomic<bool> reached_end_{false};  // 节目下载到了结尾（放完这次就是放完了）
    int64_t idle_since_us_ = 0;
    bool wifi_awake_ = false;  // 放的时候把 Wi-Fi 省电关了，停了要还原
    int64_t retry_at_us_ = 0;
    int failures_ = 0;
    SemaphoreHandle_t reader_go_ = nullptr, decoder_go_ = nullptr;
    StreamBufferHandle_t stream_ = nullptr;
    std::atomic<int> codec_{0};  // 1 MP3，2 AAC（ADTS），3 FLAC，4 WAV
    std::atomic<bool> paused_{false};  // 本地文件对话、闹钟时：解码停住不送声音，不断开、不丢位置
    std::atomic<bool> decode_failed_{false};  // 这次播放一直解不出来，解码任务放弃了
    esp_timer_handle_t timer_ = nullptr;
    // m4a 的目录（moov）：同一期节目快进快退时不用再下一遍
    std::string moov_url_;
    uint8_t* moov_ = nullptr;
    size_t moov_size_ = 0;

    NomiRadio() = default;
    void LoadList();
    void SaveList();  // 调用方持有 mutex_；上次放的下标一起存，台单改了顺序重启后还是同一个台
    void Monitor();
    void BeginSession();
    void EndSession();
    void ReaderLoop();
    void DecoderLoop();
    bool ReadStream(const std::string& url, int redirects, int start, bool live);
    bool ReadMp4(const std::string& url, int start);
    bool ReadEpisode(const std::string& url, int start);
    bool ReadLocal(const std::string& path, int start);
    bool WriteAudio(const void* data, size_t size);
    void Select(const Station& station, int index);
    void SetDuration(const std::string& url, int seconds);  // 节目没给时长时补上（还是放着的这个才写）
    void ReportProgress(bool finished);
    bool Search(const std::string& query, Station& found);
};
