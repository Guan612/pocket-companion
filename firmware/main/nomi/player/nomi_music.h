// 「正在放」的那首（或那期）的显示：SD 卡上的歌读文件里的标签（FLAC 的 Vorbis 注释、MP3 的 ID3）、
// 内嵌 JPEG 封面、同名 .lrc 歌词；网上的歌用应用给的封面地址和 LRC 歌词（media_meta）。
// 放哪首、下一首放什么、音乐网站都是音乐应用的事，这里只管显示。收听页、ui_player 控件都用这一份。
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class NomiMusic {
public:
    static constexpr int kCoverSize = 88;  // 收听页、应用卡片左边的封面都是这个大小

    static NomiMusic& GetInstance();
    void Start();  // 建下封面的后台任务

    // 应用包放的歌
    struct Track {
        std::string path;           // 设备上的路径（/sd/…）
        std::string title, artist;  // 读到标签前是文件名、空
    };
    // 应用放了一首 SD 卡上的歌：后台读它的标签（换掉显示的歌名歌手）、内嵌封面、同名 .lrc 歌词
    void ShowLocal(const std::string& path);
    // 应用放了一首网上的歌：封面地址（JPEG，可以空）、LRC 格式的歌词（可以空）
    void ShowRemote(const std::string& url, const std::string& cover_url, const std::string& lrc);
    // 文件的标题、歌手、时长（秒，不知道为 0）；cover 不为空时顺便取内嵌的 JPEG 封面
    static void ReadInfo(const std::string& path, Track& track, int& duration, std::string* cover = nullptr);
    static bool IsAudioFile(const std::string& name);

    // 正在放（或暂停着）的是不是一首歌（收听页按歌还是节目显示）
    bool Playing();
    // 歌词窗口：7 行（'\n' 分隔），第 4 行是唱到的这句（前奏时是空的）；这首没有歌词时返回空串
    std::string LyricsWindow();
    std::string Hint();                   // 没在放歌时收听页右边的提示
    const uint16_t* Cover(int& version);  // RGB565，kCoverSize 见方；没有时返回 nullptr。version 变了说明换了图

private:
    struct Lyric {
        int time;              // 第几秒开始唱
        uint32_t start, len;   // 在 lyrics_ 里的位置
    };

    std::mutex mutex_;
    std::string song_url_;       // 正在放的那首：和 NomiRadio 现在放的对得上，歌词才算数
    std::string lyrics_;         // 歌词正文，一句接一句
    std::vector<Lyric> lines_;

    // 后台任务：下封面（栈在 PSRAM）
    std::mutex jobs_mutex_;
    std::mutex net_mutex_;  // 音乐的联网一次只做一件
    std::deque<std::function<void()>> jobs_;
    SemaphoreHandle_t jobs_sem_ = nullptr;

    // 封面：两块缓冲轮着用，换图时界面还在画旧的那块也不会花
    uint16_t* covers_[2] = {nullptr, nullptr};
    std::atomic<int> cover_active_{-1};
    std::atomic<int> cover_version_{0};
    std::string cover_url_;  // 只在后台任务里用

    NomiMusic() = default;
    void LoadLocalExtras(const std::string& path);  // 后台任务里：标签、封面、歌词
    void SetLyrics(const std::string& url, const std::string& lrc);  // 解析 LRC，还在放 url 这首才换上
    void SetCover(const std::string& jpg);          // 后台任务里：换封面（空 = 没有封面）
    void Post(std::function<void()> job);
    void WorkerLoop();
    bool Get(const std::string& url, std::string& body, size_t max_bytes);  // 顺带让出正在放的 https
    void LoadCover(const std::string& url);
};
