// 系统播放器：应用包交给它的一路音频（电台直播流、一期播客、SD 卡上的文件），收流、解码、送播放。
// http(s) 上的 MP3 / AAC 直播流，或者一期节目（MP3 / m4a）、本地文件（FLAC / WAV / MP3），边收边解边放。
//
// 收流和解码各一个常驻任务，栈都在 PSRAM；网络数据先进 PSRAM 环形缓冲，攒够一点再开始放，
// 网络抖一下不至于断音。设备不在待机（有人叫它说话）或闹钟在响时停掉、连接也断开，
// 把内部 RAM 让给语音识别；回到待机后自动接着放（电台是直播，节目从停下的地方接着放）。
// 放什么、上一首下一首是应用的事（Station.owner 记着是哪个应用放的，放完 / 换首通知它）；
// 固件自己不放任何东西，只留「停 / 暂停」一个大模型工具。
#pragma once

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

class Mp4Track;

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
        bool song = false;      // 一首歌（在线或本地）：不记进度，放完交给 on_finished 接下一首
        bool local = false;     // SD 卡上的文件：url 是设备上的路径（/sd/…），不用联网；对话时只暂停不断开
        std::string owner;      // 哪个应用包放的（应用 id）
        // 地址和响应头看不出真实格式时（m4a 的扩展名写成 .aac、类型是 octet-stream），调用方先探好填这里
        enum Format { kAuto, kMp3, kAdts, kMp4, kFlac, kWav } format = kAuto;
    };

    static NomiRadio& GetInstance();
    // 网上的音频按文件头（至少 12 字节）认格式：扩展名、响应头常对不上（m4a 写成 .aac、类型是 octet-stream）。
    // 认不出（不是 MP3 / AAC / m4a）返回 kAuto
    static Station::Format SniffFormat(const std::string& head);

    void Start();  // 建任务和监视定时器；板子构造时调用一次
    void RegisterTools();  // 只有大模型的「停」（放什么、换哪个都归应用）

    // 屏幕上的按钮
    void Toggle();  // 放 / 停（节目是暂停，停着的还是它）
    void Stop();
    // 闹钟响时先停，关了闹钟接着放
    void SetAlarmHold(bool hold) { alarm_hold_ = hold; }
    // 要另开一条 https（拉节目单等）时先让出来：正在放的若是 https，暂停到 ReleaseHttpsHold，回来从原位置接着放。
    // 内部 RAM 只够同时一条 TLS（两条时只剩 10KB 左右）。返回 true 表示暂停了（已经断开），用完要 Release。
    // 会等最多 5 秒，别在界面任务里调
    bool HoldForHttps();
    void ReleaseHttpsHold();

    // 应用放一路音频（电台直播 / 节目 / 歌曲）
    void PlayEpisode(const Station& episode);
    void Seek(int delta_seconds);  // 快进 / 快退（只对节目有用）
    void SeekTo(int second);
    bool IsEpisode();
    Station Current();
    int Position() { return position_; }  // 节目放到第几秒
    // 一首歌 / 一期节目从头放到尾放完了：在主循环里调（应用包放的，通知那个应用接下一首）
    std::function<void(const Station& item)> on_finished;

    State GetState();
    bool Wanted() { return wanted_; }
    // 还在放 url 这个的话，换显示的名字 / 歌手（本地歌读到标签后用；空的不换）
    void UpdateMeta(const std::string& url, const std::string& name, const std::string& artist);
    std::string StationName();
    std::string Title();  // 流里带的歌名（ICY StreamTitle），没有就空

private:
    std::mutex mutex_;
    Station current_;
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
    // m4a 的目录（moov 里各个表在哪、内存里的窗口）：同一期节目快进快退、对话后接着放时不用再读一遍。
    // 只有收流任务碰它
    std::string mp4_url_;
    std::unique_ptr<Mp4Track> mp4_;

    NomiRadio() = default;
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
    void Select(const Station& station);
    void SetDuration(const std::string& url, int seconds);  // 节目没给时长时补上（还是放着的这个才写）
};
