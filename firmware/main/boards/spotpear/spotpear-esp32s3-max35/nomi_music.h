// 在线音乐：在主人填的音乐网站上按歌名搜，从歌曲页拿音频直链、封面和带时间的歌词，交给 NomiRadio 放。
//
// 只认一种站点格式（网址在网页设置页填，存 NVS nomi_music 命名空间的 site），详见 docs/online-music.md：
// 搜索页 {网站}/so/{关键词}/ 每条结果带 data-rid；歌曲页 {网站}/song/{rid}/ 里有 base64 编码的音频地址、
// id="album-cover" 的封面和 data-time 的歌词。网站失效了换一个同款的填上就行，不用改固件。
// 音频、封面先试 http（不占 TLS 的内部 RAM）；格式看文件头认（有的站把 m4a 也写成 .aac）。歌放完就停，不记进度。
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
    struct Song {
        std::string id, title, artist;
    };
    static constexpr int kCoverSize = 88;  // 和播客封面一样大：收听页左边同一个位置

    static NomiMusic& GetInstance();
    void Start();  // 读网站地址、建后台任务
    void RegisterTools();

    // 收听页的"音乐"标签
    bool Playing();  // 正在放（或暂停着）的是一首歌
    // 歌词窗口：7 行（'\n' 分隔），第 4 行是唱到的这句（前奏时是空的）；这首没有歌词时返回空串
    std::string LyricsWindow();
    std::string Hint();                   // 没在放歌时右边的提示
    const uint16_t* Cover(int& version);  // RGB565，kCoverSize 见方；没有时返回 nullptr。version 变了说明换了图

    // 网页设置页：GET /music.json，POST /music/site、/music/search、/music/play；出错时返回 "!原因"
    std::string InfoJson();
    std::string SetSiteJson(const std::string& body);
    std::string SearchJson(const std::string& body);
    std::string PlayJson(const std::string& body);  // {"index":n}：放上次搜到的第 n 首

private:
    struct Lyric {
        int time;              // 第几秒开始唱
        uint32_t start, len;   // 在 lyrics_ 里的位置
    };

    std::mutex mutex_;
    std::string site_;           // "http://example.com"，不带结尾的 /；空 = 还没设
    std::vector<Song> results_;  // 上次搜到的："下一首"往下放
    int result_index_ = -1;
    std::string song_url_;       // 正在放的那首的音频地址：和 NomiRadio 现在放的对得上，歌词才算数
    std::string lyrics_;         // 歌词正文，一句接一句
    std::vector<Lyric> lines_;

    // 后台任务：下封面（栈在 PSRAM，不碰 NVS）
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
    void Post(std::function<void()> job);
    void WorkerLoop();
    std::string Site();
    bool Get(const std::string& url, std::string& body, size_t max_bytes);
    // 搜到的放进 out；网站连不上、或者不是这种格式的网站返回 false
    bool Search(const std::string& query, std::vector<Song>& out);
    // 取歌曲页、探格式、放起来。返回给大模型 / 网页的说明，放不了时以 "!" 开头
    std::string Play(const Song& song);
    void LoadCover(const std::string& url);
};
