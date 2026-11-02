// 播客：订阅（存 NVS）、Apple 播客目录搜索、读 RSS 最新几期、记每个播客听到哪、封面图。
// 真正的播放交给 NomiRadio（同一套收流 / 解码）。
//
// 联网的活（搜索、拉节目单、下封面）在一个常驻的后台任务里做（栈在 PSRAM），屏幕上点了不卡；
// 大模型的工具在对话任务里同步做，好把节目标题告诉它。写 NVS 一律排到主循环里。
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

class NomiPodcast {
public:
    struct Podcast {
        std::string name, feed, art;  // art：Apple 目录给的封面小图地址
    };
    struct Episode {
        std::string title, url, date;
        int duration = 0, bytes_per_sec = 0;
    };
    static constexpr int kCoverSize = 88;

    static NomiPodcast& GetInstance();
    void Start();  // 读订阅、建后台任务、接上播放器的进度回调
    void RegisterTools();

    // 屏幕上：打开一个订阅看节目单、回到订阅列表、放节目单里的第几期
    void Open(int index);
    void Back();
    void PlayOpened(int episode);
    std::string ListText();      // 订阅名，一行一个
    int Opened();                // 打开的是第几个订阅；-1 是在订阅列表
    int LoadState();             // 节目单：0 好了，1 加载中，2 加载失败
    std::string EpisodesText();  // 打开的节目单，一行一期："标题\t日期 · 时长"
    // 正在放的那个播客的封面：RGB565，kCoverSize 见方；还没有时返回 nullptr。version 变了说明换了图
    const uint16_t* Cover(int& version);
    std::string PromptContext();

    // 网页设置页：GET /podcast.json，POST /podcast/search、/podcast/subscribe、/podcast/delete
    std::string ListJson();
    std::string SearchJson(const std::string& body);
    std::string SubscribeJson(const std::string& body);
    std::string DeleteJson(const std::string& body);
    std::string PlayJson(const std::string& body);  // {"index":n}：放第 n 个订阅的最新一期（没听完的接着放）

private:
    std::mutex mutex_;
    std::vector<Podcast> subs_;
    int opened_ = -1;
    std::vector<Episode> episodes_;  // 打开的那个订阅的节目单
    std::atomic<int> load_state_{0};
    Podcast playing_;  // 正在放 / 刚放过的节目属于哪个播客（可能没订阅）

    // 后台任务
    std::mutex jobs_mutex_;
    std::recursive_mutex net_mutex_;  // 播客的联网（节目单、搜索、封面）一次只做一件
    std::deque<std::function<void()>> jobs_;
    SemaphoreHandle_t jobs_sem_ = nullptr;

    // 封面：两块缓冲轮着用，换图时界面还在画旧的那块也不会花
    uint16_t* covers_[2] = {nullptr, nullptr};
    std::atomic<int> cover_active_{-1};
    std::atomic<int> cover_version_{0};
    std::string cover_url_;

    NomiPodcast() = default;
    void Post(std::function<void()> job);
    void WorkerLoop();
    void SaveSubs();  // 在主循环里写
    bool FetchEpisodes(const std::string& feed, std::vector<Episode>& out, std::string* channel = nullptr);
    bool SearchApple(const std::string& query, std::vector<Podcast>& out, int limit);
    void LoadCover(const std::string& art);
    // 返回给大模型的说明。in_worker 为 true（在后台任务里调，栈在 PSRAM、不能读写 NVS）时
    // 上次听到哪儿要到主循环里再查，说明里就不带"从哪儿接着放"
    std::string PlayEpisode(const Podcast& podcast, const Episode& episode, bool in_worker = false);
    int SavedPosition(const std::string& feed, const std::string& url);
    int FindSub(const std::string& query);
};
