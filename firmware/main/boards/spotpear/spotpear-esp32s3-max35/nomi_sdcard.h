// TF 卡：SPI 模式，用的时候挂到 /sd，没人用了过一会儿自动卸载。照片、录音、本地音乐等功能都从这里取卡。
//
// 板子是通用的，卡也是：
// - 没有插卡检测脚：挂载时卡不应答就当没插（也包括 R65/R66 没焊、4G 版占着总线的板子）。
// - 只认 FAT（FAT12/16/32）：ESP-IDF 的 FatFs 没开 exFAT，64GB 以上的卡出厂多是 exFAT，要先在电脑上格式化。
//   卡应答了但挂不上就报"格式不支持"，绝不自动格式化（会清掉卡里的东西）。
// - 读写偶尔出错（实测约每读 1000～2000 次 8KB 一次，和时钟快慢、放不放声音无关）：出错先重新初始化卡再试，
//   还不行才算卡坏了 / 被拔了，下次用时重新挂。这层在 FatFs 底下做，上面的 fopen / fread 感觉不到。
// - CLK 用的 GPIO43 在另一种焊法（R30 焊上、R64 不焊）下也是功放开关：挂着卡时它被 SPI 时钟占着，
//   那种板子在读写卡期间喇叭没声。所以不用就卸载，卸载后用 on_bus_released 把引脚还给功放。
//   这块板（R30 不焊、功放常开）实测边读卡边放声音没问题。
#pragma once

#include <esp_timer.h>
#include <sd_protocol_types.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

class NomiSdCard {
public:
    enum State { kUnknown, kNoCard, kUnsupported, kReady };
    static constexpr const char* kMountPoint = "/sd";

    static NomiSdCard& GetInstance();

    // 卸载、释放 SPI 总线后调：板子把 GPIO43 还给功放
    std::function<void()> on_bus_released;

    // 用卡前 Acquire（挂上了返回 true），用完 Release。最后一个 Release 后闲置一会儿自动卸载。
    // 会做 SPI 读写，别在中断里调
    bool Acquire();
    void Release();
    // 自动成对：if (NomiSdCard::Use sd; sd) { …读写 /sd/… }
    class Use {
    public:
        Use() : ok_(GetInstance().Acquire()) {}
        ~Use() {
            if (ok_) GetInstance().Release();
        }
        explicit operator bool() const { return ok_; }
        Use(const Use&) = delete;
        Use& operator=(const Use&) = delete;

    private:
        bool ok_;
    };

    State GetState();
    // 网页设置页 GET /sd.json：当场挂一下看卡在不在、多大、还剩多少
    std::string StatusJson();

    // FatFs 底层读写出错重试都失败时调（在 FatFs 的锁里，不能当场卸载）：下次 Release 时卸载
    void MarkBroken() { broken_ = true; }

private:
    std::mutex mutex_;
    sdmmc_card_t* card_ = nullptr;
    bool bus_ready_ = false;
    int users_ = 0;
    std::atomic<bool> broken_{false};
    State state_ = kUnknown;
    esp_timer_handle_t idle_timer_ = nullptr;

    NomiSdCard() = default;
    bool MountLocked();
    void UnmountLocked();
    void ReleaseBusLocked();
    void UnmountIfIdle();
};
