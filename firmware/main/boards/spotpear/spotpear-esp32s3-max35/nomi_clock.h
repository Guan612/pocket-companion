// 时钟页背后的东西：对时（SNTP，北京时间）、天气（open-meteo，免 key，走 http）、闹钟和倒计时提醒。
//
// 界面经 NomiDisplay 的 GetText / Action 读写这里；大模型经 MCP 工具设闹钟、改天气城市，
// 系统提示词里带上现在的时间、天气和已设的闹钟（见 PromptContext）。
// 闹钟存 NVS（nomi_alarm 命名空间），断电重启还在；天气城市存在 directapi 命名空间的 wx_city，
// 网页设置页和 self.weather.set_city 共用。
#pragma once

#include <esp_timer.h>

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class NomiClock {
public:
    static NomiClock& GetInstance();

    // 时区、每秒一次的定时器、读出存着的闹钟；板子构造时调用一次
    void Start();
    // 给大模型的工具：闹钟、倒计时提醒、天气城市
    void RegisterTools();

    bool TimeValid() const;
    std::string TimeText();     // "14:32"，还没对上时间时为空
    std::string DateText();     // "11月2日 星期日"
    int WeatherCode();          // WMO 天气代码，夜里加 1000；还没有天气时 -1
    std::string WeatherTemp();  // "26°"
    std::string WeatherText();  // "多云 · 深圳  23~32°"
    std::string WeatherCity();  // 设的城市，空 = 按网络位置自动
    // 接下来要响的闹钟，按时间排，一行一个："编号\t什么时候\t事由"
    std::string AlarmsText();
    bool DeleteAlarm(int id);  // id 为 0 删除全部

    // 写进系统提示词：现在的时间、天气、已设的闹钟
    std::string PromptContext();

    // 响铃：铃声每隔两秒响一遍，90 秒后不再响，但提醒一直留在屏幕上，等人点掉或按 BOOT 键
    bool IsRinging() const { return ringing_; }
    void StopRinging();
    void Snooze();  // 5 分钟后再响
    // 天气城市改了（网页设置页保存时）：待机时马上重新查
    void RefreshWeather() { weather_next_us_ = 0; }
    std::function<void(const std::string& time, const std::string& label)> on_ring_start;
    std::function<void()> on_ring_stop;

private:
    struct Alarm {
        int id = 0;
        int64_t at = 0;  // 只响一次：到点时刻（epoch 秒）
        int hm = -1;     // 重复闹钟：一天里的第几分钟；-1 表示只响一次
        int days = 0;    // 重复闹钟：星期掩码，bit0 周日 … bit6 周六
        std::string label;
    };

    struct Weather {
        bool valid = false;
        int code = -1, tomorrow_code = -1;
        bool is_day = true;
        int temp = 0, max = 0, min = 0, tomorrow_max = 0, tomorrow_min = 0;
        std::string place;
    };

    mutable std::mutex mutex_;
    std::vector<Alarm> alarms_;
    int next_id_ = 1;
    Weather weather_;

    esp_timer_handle_t timer_ = nullptr;
    bool sntp_started_ = false;
    int64_t last_minute_ = 0;
    std::atomic<bool> ringing_{false};
    std::atomic<bool> ring_sounding_{false};
    bool ring_fired_idle_ = false;  // 响铃时是待机：之后有人叫它说话就当是听到了
    int64_t ring_start_us_ = 0, ring_sound_us_ = 0;
    std::string ring_label_;
    std::atomic<bool> weather_busy_{false};
    std::atomic<int64_t> weather_next_us_{0};

    NomiClock() = default;
    void Tick();
    void StartSntp();
    void CheckAlarms(time_t now);
    void Fire(const std::string& label, time_t when);
    void LoadAlarms();
    void SaveAlarms();  // 调用方持有 mutex_
    int AddAlarm(Alarm alarm);  // 返回编号，满了返回 -1
    int64_t NextFire(const Alarm& alarm, time_t now) const;
    std::string Describe(const Alarm& alarm, time_t now) const;
    void FetchWeather();
    bool ResolvePlace(double& lat, double& lon, std::string& place);
};
