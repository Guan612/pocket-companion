#include "nomi_clock.h"

#include "application.h"
#include "board.h"
#include "mcp_server.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define TAG "NomiClock"

extern "C" {
extern const uint8_t nomi_alarm_ogg[];  // nomi_alarm_sound.c
extern const size_t nomi_alarm_ogg_len;
}

namespace {

constexpr const char* kAlarmNs = "nomi_alarm";
constexpr const char* kCityNs = "directapi";  // 网页设置页写这里
constexpr const char* kCityKey = "wx_city";
constexpr const char* kPlaceNs = "nomi_wx";  // 定位缓存：按哪个城市名查的、经纬度、地名
constexpr int kMaxAlarms = 10;
constexpr int64_t kRingIntervalUs = 2500 * 1000LL;
constexpr int64_t kRingSoundUs = 90 * 1000000LL;
constexpr int64_t kWeatherEveryUs = 30 * 60 * 1000000LL;
constexpr int64_t kWeatherRetryUs = 5 * 60 * 1000000LL;
constexpr int kLateDropSeconds = 600;  // 关着机错过的一次性闹钟：晚了 10 分钟以上就不补响了
constexpr int kDaily = 0x7F, kWeekdays = 0x3E, kWeekends = 0x41;
const char* const kWeekdayNames[] = {"日", "一", "二", "三", "四", "五", "六"};

std::string_view AlarmSound() {
    return std::string_view(reinterpret_cast<const char*>(nomi_alarm_ogg), nomi_alarm_ogg_len);
}

struct tm Local(time_t t) {
    struct tm tm = {};
    localtime_r(&t, &tm);
    return tm;
}

// 当天 0 点；第 offset 天后的 0 点
time_t Midnight(time_t t, int offset = 0) {
    struct tm tm = Local(t);
    tm.tm_mday += offset;
    tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

int DaysBetween(time_t from, time_t to) {
    return static_cast<int>(std::lround((Midnight(to) - Midnight(from)) / 86400.0));
}

const char* WeatherName(int code) {
    switch (code) {
        case 0: return "晴";
        case 1: return "晴间多云";
        case 2: return "多云";
        case 3: return "阴";
        case 45: case 48: return "雾";
        case 51: case 53: case 55: return "毛毛雨";
        case 56: case 57: return "冻毛毛雨";
        case 61: return "小雨";
        case 63: return "中雨";
        case 65: return "大雨";
        case 66: case 67: return "冻雨";
        case 71: return "小雪";
        case 73: return "中雪";
        case 75: return "大雪";
        case 77: return "雪粒";
        case 80: case 81: return "阵雨";
        case 82: return "强阵雨";
        case 85: case 86: return "阵雪";
        case 95: return "雷阵雨";
        case 96: case 99: return "雷阵雨伴冰雹";
        default: return "未知天气";
    }
}

std::string RepeatText(int days) {
    if (days == kDaily) return "每天";
    if (days == kWeekdays) return "工作日";
    if (days == kWeekends) return "周末";
    std::string text = "每周";
    for (int d = 1; d <= 7; d++) {
        if (days & (1 << (d % 7))) text += kWeekdayNames[d % 7];  // 从周一排到周日
    }
    return text;
}

bool NetworkUp() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    return netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

// 天气、定位都是几百字节的 http 接口：不走 TLS，不占内部 RAM
bool HttpGet(const std::string& url, std::string& body) {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(2);
    http->SetTimeout(8000);
    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "GET failed: %s", url.c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    body = http->ReadAll();
    http->Close();
    if (!status || *status != 200) {
        ESP_LOGW(TAG, "GET %s -> %d", url.c_str(), status ? *status : -1);
        return false;
    }
    return true;
}

std::string UrlEncode(const std::string& s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

double JsonNumber(cJSON* obj, const char* key, int index = -1) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (index >= 0) item = cJSON_GetArrayItem(item, index);
    return cJSON_IsNumber(item) ? item->valuedouble : NAN;
}

}  // namespace

NomiClock& NomiClock::GetInstance() {
    static NomiClock instance;
    return instance;
}

void NomiClock::Start() {
    // 设备在国内用：固定北京时间（没有夏令时）
    setenv("TZ", "CST-8", 1);
    tzset();
    LoadAlarms();
    const esp_timer_create_args_t args = {
        .callback = [](void* arg) { static_cast<NomiClock*>(arg)->Tick(); },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "nomi_clock",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer_));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer_, 1000 * 1000));
}

bool NomiClock::TimeValid() const { return Local(time(nullptr)).tm_year >= 2025 - 1900; }

// 每秒一次，跑在 esp_timer 任务里：只做判断，要花时间的事交给主循环或单开任务
void NomiClock::Tick() {
    auto& app = Application::GetInstance();
    if (!sntp_started_ && NetworkUp()) {
        sntp_started_ = true;
        app.Schedule([this]() { StartSntp(); });
    }
    if (!TimeValid()) return;

    CheckAlarms(time(nullptr));

    int64_t us = esp_timer_get_time();
    auto state = app.GetDeviceState();
    if (ringing_) {
        // 闹钟是待机时响的，接着有人叫它说话了：当是听到了，停掉
        if (ring_fired_idle_ && (state == kDeviceStateListening || state == kDeviceStateSpeaking)) {
            StopRinging();
        } else if (ring_sounding_) {
            if (us - ring_start_us_ > kRingSoundUs) {
                ring_sounding_ = false;  // 响够了，只留屏幕上的提醒
            } else if (us - ring_sound_us_ >= kRingIntervalUs && state != kDeviceStateListening &&
                       state != kDeviceStateSpeaking) {  // 对话中不插铃声，屏幕上有提醒
                ring_sound_us_ = us;
                app.Schedule([]() { Application::GetInstance().PlaySound(AlarmSound()); });
            }
        }
    }

    // 天气：待机时才去拉（不和对话抢网络），30 分钟一次，失败 5 分钟后再试
    if (!weather_busy_ && us >= weather_next_us_ && state == kDeviceStateIdle && NetworkUp()) {
        weather_busy_ = true;
        auto task = [](void* arg) {
            auto* self = static_cast<NomiClock*>(arg);
            self->FetchWeather();
            self->weather_busy_ = false;
            vTaskDelete(nullptr);
        };
        if (xTaskCreate(task, "weather", 6144, this, 1, nullptr) != pdPASS) {
            weather_busy_ = false;
            weather_next_us_ = us + kWeatherRetryUs;
        }
    }
}

void NomiClock::StartSntp() {
    esp_sntp_config_t config = {};
    config.start = true;
    config.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    const char* servers[] = {"ntp.aliyun.com", "ntp.tencent.com", "cn.pool.ntp.org"};
    int n = std::min<int>(std::size(servers), CONFIG_LWIP_SNTP_MAX_SERVERS);
    for (int i = 0; i < n; i++) config.servers[i] = servers[i];
    config.num_of_servers = n;
    config.sync_cb = [](struct timeval* tv) {
        struct tm tm = Local(tv->tv_sec);
        ESP_LOGI(TAG, "Time synced: %04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    };
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        sntp_started_ = false;  // 下一秒再试
    }
}

/* ---------- 闹钟 ---------- */

void NomiClock::LoadAlarms() {
    std::lock_guard<std::mutex> lock(mutex_);
    alarms_.clear();
    std::string json = Settings(kAlarmNs).GetString("list");
    cJSON* root = cJSON_Parse(json.c_str());
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        Alarm a;
        a.id = static_cast<int>(JsonNumber(item, "i"));
        double at = JsonNumber(item, "a"), hm = JsonNumber(item, "m"), days = JsonNumber(item, "d");
        if (!std::isnan(at)) a.at = static_cast<int64_t>(at);
        if (!std::isnan(hm)) a.hm = static_cast<int>(hm);
        if (!std::isnan(days)) a.days = static_cast<int>(days);
        cJSON* label = cJSON_GetObjectItem(item, "l");
        if (cJSON_IsString(label)) a.label = label->valuestring;
        if (a.id <= 0 || (a.hm < 0 && a.at <= 0) || (a.hm >= 0 && (a.days & kDaily) == 0)) continue;
        next_id_ = std::max(next_id_, a.id + 1);
        alarms_.push_back(std::move(a));
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %u alarm(s)", static_cast<unsigned>(alarms_.size()));
}

void NomiClock::SaveAlarms() {
    cJSON* root = cJSON_CreateArray();
    for (const auto& a : alarms_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "i", a.id);
        if (a.hm >= 0) {
            cJSON_AddNumberToObject(item, "m", a.hm);
            cJSON_AddNumberToObject(item, "d", a.days);
        } else {
            cJSON_AddNumberToObject(item, "a", static_cast<double>(a.at));
        }
        if (!a.label.empty()) cJSON_AddStringToObject(item, "l", a.label.c_str());
        cJSON_AddItemToArray(root, item);
    }
    char* json = cJSON_PrintUnformatted(root);
    Settings(kAlarmNs, true).SetString("list", json ? json : "[]");
    cJSON_free(json);
    cJSON_Delete(root);
}

int64_t NomiClock::NextFire(const Alarm& a, time_t now) const {
    if (a.hm < 0) return a.at;
    for (int d = 0; d <= 7; d++) {
        time_t day = Midnight(now, d);
        if (!(a.days & (1 << Local(day).tm_wday))) continue;
        struct tm tm = Local(day);
        tm.tm_hour = a.hm / 60;
        tm.tm_min = a.hm % 60;
        tm.tm_isdst = -1;
        time_t t = mktime(&tm);
        if (t > now) return t;
    }
    return INT64_MAX;
}

// "今天 07:00" / "明天 07:00" / "周三 07:00" / "11月12日 07:00"
std::string NomiClock::Describe(const Alarm& a, time_t now) const {
    int64_t t = NextFire(a, now);
    if (t == INT64_MAX) return "";
    struct tm tm = Local(t);
    int days = DaysBetween(now, t);
    char day[24];
    if (days == 0) snprintf(day, sizeof(day), "今天");
    else if (days == 1) snprintf(day, sizeof(day), "明天");
    else if (days == 2) snprintf(day, sizeof(day), "后天");
    else if (days < 7) snprintf(day, sizeof(day), "周%s", kWeekdayNames[tm.tm_wday]);
    else snprintf(day, sizeof(day), "%d月%d日", tm.tm_mon + 1, tm.tm_mday);
    char buf[48];
    snprintf(buf, sizeof(buf), "%s %02d:%02d", day, tm.tm_hour, tm.tm_min);
    return buf;
}

std::string NomiClock::AlarmsText() {
    if (!TimeValid()) return "";
    time_t now = time(nullptr);
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<const Alarm*> sorted;
    for (const auto& a : alarms_) sorted.push_back(&a);
    std::sort(sorted.begin(), sorted.end(), [this, now](const Alarm* x, const Alarm* y) {
        return NextFire(*x, now) < NextFire(*y, now);
    });
    std::string text;
    for (const Alarm* a : sorted) {
        std::string when = Describe(*a, now);
        if (a->hm >= 0) when += " · " + RepeatText(a->days);
        text += std::to_string(a->id) + "\t" + when + "\t" + a->label + "\n";
    }
    if (!text.empty()) text.pop_back();
    return text;
}

int NomiClock::AddAlarm(Alarm alarm) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (static_cast<int>(alarms_.size()) >= kMaxAlarms) return -1;
    alarm.id = next_id_++;
    alarms_.push_back(alarm);
    SaveAlarms();
    return alarm.id;
}

bool NomiClock::DeleteAlarm(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t before = alarms_.size();
    if (id == 0) {
        alarms_.clear();
    } else {
        alarms_.erase(std::remove_if(alarms_.begin(), alarms_.end(),
                                     [id](const Alarm& a) { return a.id == id; }),
                      alarms_.end());
    }
    if (alarms_.size() == before) return false;
    SaveAlarms();
    return true;
}

void NomiClock::CheckAlarms(time_t now) {
    std::vector<std::pair<std::string, time_t>> fire;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t minute = now / 60;
        bool new_minute = minute != last_minute_;
        last_minute_ = minute;
        struct tm tm = Local(now);
        for (auto it = alarms_.begin(); it != alarms_.end();) {
            if (it->hm < 0) {
                if (it->at <= now) {
                    if (now - it->at < kLateDropSeconds) fire.emplace_back(it->label, it->at);
                    else ESP_LOGW(TAG, "Dropped missed alarm #%d", it->id);
                    it = alarms_.erase(it);
                    changed = true;
                    continue;
                }
            } else if (new_minute && it->hm == tm.tm_hour * 60 + tm.tm_min &&
                       (it->days & (1 << tm.tm_wday))) {
                fire.emplace_back(it->label, now);
            }
            ++it;
        }
    }
    if (changed) {
        // 写 NVS 要几毫秒到几十毫秒，别在 esp_timer 任务里做
        Application::GetInstance().Schedule([this]() {
            std::lock_guard<std::mutex> lock(mutex_);
            SaveAlarms();
        });
    }
    for (const auto& [label, when] : fire) Fire(label, when);
}

void NomiClock::Fire(const std::string& label, time_t when) {
    struct tm tm = Local(when);
    char time_str[8];
    snprintf(time_str, sizeof(time_str), "%02d:%02d", tm.tm_hour, tm.tm_min);
    ESP_LOGI(TAG, "Alarm %s %s", time_str, label.c_str());
    int64_t us = esp_timer_get_time();
    ring_fired_idle_ = Application::GetInstance().GetDeviceState() == kDeviceStateIdle;
    ring_label_ = label;
    ring_start_us_ = us;
    ring_sound_us_ = us - kRingIntervalUs;  // 这一秒就响第一遍
    ring_sounding_ = true;
    ringing_ = true;
    std::string text = time_str;
    Application::GetInstance().Schedule([this, text, label]() {
        if (on_ring_start) on_ring_start(text, label);
    });
}

void NomiClock::StopRinging() {
    if (!ringing_.exchange(false)) return;
    ring_sounding_ = false;
    Application::GetInstance().Schedule([this]() {
        if (on_ring_stop) on_ring_stop();
    });
}

void NomiClock::Snooze() {
    if (!ringing_) return;
    Alarm a;
    a.at = time(nullptr) + 5 * 60;
    a.label = ring_label_;
    AddAlarm(a);
    StopRinging();
}

/* ---------- 天气 ---------- */

// 城市没设就按网络出口 IP 定位。查到的经纬度缓存在 NVS，城市改了才重新查
bool NomiClock::ResolvePlace(double& lat, double& lon, std::string& place) {
    std::string city = Settings(kCityNs).GetString(kCityKey);
    {
        Settings cache(kPlaceNs);
        if (cache.GetString("for", "\x01") == city && !cache.GetString("lat").empty()) {
            lat = atof(cache.GetString("lat").c_str());
            lon = atof(cache.GetString("lon").c_str());
            place = cache.GetString("name");
            return true;
        }
    }
    std::string body;
    cJSON* root = nullptr;
    bool ok = false;
    if (city.empty()) {
        // 走代理或公司专线时可能定到别的城市；时钟页上写着城市名，不对就在设置页里填
        if (HttpGet("http://ip-api.com/json/?lang=zh-CN&fields=status,city,lat,lon", body)) {
            root = cJSON_Parse(body.c_str());
            cJSON* status = cJSON_GetObjectItem(root, "status");
            cJSON* name = cJSON_GetObjectItem(root, "city");
            lat = JsonNumber(root, "lat");
            lon = JsonNumber(root, "lon");
            ok = cJSON_IsString(status) && strcmp(status->valuestring, "success") == 0 &&
                 !std::isnan(lat) && !std::isnan(lon);
            place = cJSON_IsString(name) ? name->valuestring : "";
        }
    } else if (HttpGet("http://geocoding-api.open-meteo.com/v1/search?count=1&language=zh&format=json&name=" +
                           UrlEncode(city),
                       body)) {
        root = cJSON_Parse(body.c_str());
        cJSON* first = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "results"), 0);
        cJSON* name = cJSON_GetObjectItem(first, "name");
        lat = JsonNumber(first, "latitude");
        lon = JsonNumber(first, "longitude");
        ok = first != nullptr && !std::isnan(lat) && !std::isnan(lon);
        place = cJSON_IsString(name) ? name->valuestring : city;
        if (!ok && body.find("\"generationtime_ms\"") != std::string::npos) {
            // 接口通了但没这个地名：别每 5 分钟再查一遍
            std::lock_guard<std::mutex> lock(mutex_);
            weather_ = Weather();
            weather_.place = "没找到「" + city + "」";
            weather_next_us_ = esp_timer_get_time() + kWeatherEveryUs;
            cJSON_Delete(root);
            return false;
        }
    }
    cJSON_Delete(root);
    if (!ok) return false;
    Settings cache(kPlaceNs, true);
    char buf[24];
    cache.SetString("for", city);
    snprintf(buf, sizeof(buf), "%.4f", lat);
    cache.SetString("lat", buf);
    snprintf(buf, sizeof(buf), "%.4f", lon);
    cache.SetString("lon", buf);
    cache.SetString("name", place);
    ESP_LOGI(TAG, "Weather place: %s (%.2f, %.2f)", place.c_str(), lat, lon);
    return true;
}

void NomiClock::FetchWeather() {
    int64_t retry_at = esp_timer_get_time() + kWeatherRetryUs;
    double lat = 0, lon = 0;
    std::string place;
    if (!ResolvePlace(lat, lon, place)) {
        if (weather_next_us_ < retry_at) weather_next_us_ = retry_at;
        return;
    }
    char url[320];
    snprintf(url, sizeof(url),
             "http://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code,is_day"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=2",
             lat, lon);
    std::string body;
    if (!HttpGet(url, body)) {
        weather_next_us_ = retry_at;
        return;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    cJSON* current = cJSON_GetObjectItem(root, "current");
    cJSON* daily = cJSON_GetObjectItem(root, "daily");
    Weather w;
    double temp = JsonNumber(current, "temperature_2m"), code = JsonNumber(current, "weather_code");
    double is_day = JsonNumber(current, "is_day");
    double values[6] = {JsonNumber(daily, "temperature_2m_max", 0), JsonNumber(daily, "temperature_2m_min", 0),
                        JsonNumber(daily, "weather_code", 1),       JsonNumber(daily, "temperature_2m_max", 1),
                        JsonNumber(daily, "temperature_2m_min", 1), 0};
    w.valid = !std::isnan(temp) && !std::isnan(code);
    if (w.valid) {
        w.temp = static_cast<int>(std::lround(temp));
        w.code = static_cast<int>(code);
        w.is_day = std::isnan(is_day) || is_day > 0.5;
        w.max = std::isnan(values[0]) ? w.temp : static_cast<int>(std::lround(values[0]));
        w.min = std::isnan(values[1]) ? w.temp : static_cast<int>(std::lround(values[1]));
        w.tomorrow_code = std::isnan(values[2]) ? -1 : static_cast<int>(values[2]);
        w.tomorrow_max = std::isnan(values[3]) ? 0 : static_cast<int>(std::lround(values[3]));
        w.tomorrow_min = std::isnan(values[4]) ? 0 : static_cast<int>(std::lround(values[4]));
        w.place = place;
    }
    cJSON_Delete(root);
    if (!w.valid) {
        ESP_LOGW(TAG, "Bad weather response: %.120s", body.c_str());
        weather_next_us_ = retry_at;
        return;
    }
    ESP_LOGI(TAG, "Weather %s: %s %d°C (%d~%d)", place.c_str(), WeatherName(w.code), w.temp, w.min, w.max);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        weather_ = w;
    }
    weather_next_us_ = esp_timer_get_time() + kWeatherEveryUs;
}

/* ---------- 给界面和大模型的文字 ---------- */

std::string NomiClock::TimeText() {
    if (!TimeValid()) return "";
    struct tm tm = Local(time(nullptr));
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

std::string NomiClock::DateText() {
    if (!TimeValid()) return "";
    struct tm tm = Local(time(nullptr));
    char buf[40];
    snprintf(buf, sizeof(buf), "%d月%d日 星期%s", tm.tm_mon + 1, tm.tm_mday, kWeekdayNames[tm.tm_wday]);
    return buf;
}

int NomiClock::WeatherCode() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!weather_.valid) return -1;
    return weather_.code + (weather_.is_day ? 0 : 1000);
}

std::string NomiClock::WeatherTemp() {
    std::lock_guard<std::mutex> lock(mutex_);
    return weather_.valid ? std::to_string(weather_.temp) + "°" : "";
}

std::string NomiClock::WeatherText() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!weather_.valid) return weather_.place;  // 没找到城市时是提示，其他时候空
    return std::string(WeatherName(weather_.code)) + " · " + weather_.place + "  " +
           std::to_string(weather_.min) + "~" + std::to_string(weather_.max) + "°";
}

std::string NomiClock::WeatherCity() { return Settings(kCityNs).GetString(kCityKey); }

std::string NomiClock::PromptContext() {
    std::string ctx;
    time_t now = time(nullptr);
    if (TimeValid()) {
        struct tm tm = Local(now);
        char buf[96];
        snprintf(buf, sizeof(buf), "现在是 %d年%d月%d日 星期%s %02d:%02d（北京时间）。", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, kWeekdayNames[tm.tm_wday], tm.tm_hour, tm.tm_min);
        ctx = buf;
    } else {
        ctx = "设备还没对上时间（刚开机或没联网），设闹钟、提醒要等对上时间。";
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (weather_.valid) {
        char buf[200];
        snprintf(buf, sizeof(buf), "\n天气（%s）：%s，现在 %d°C，今天 %d~%d°C", weather_.place.c_str(),
                 WeatherName(weather_.code), weather_.temp, weather_.min, weather_.max);
        ctx += buf;
        if (weather_.tomorrow_code >= 0) {
            snprintf(buf, sizeof(buf), "；明天%s，%d~%d°C", WeatherName(weather_.tomorrow_code),
                     weather_.tomorrow_min, weather_.tomorrow_max);
            ctx += buf;
        }
        ctx += "。";
    }
    if (!alarms_.empty() && TimeValid()) {
        ctx += "\n已设的闹钟和提醒：";
        for (const auto& a : alarms_) {
            ctx += "#" + std::to_string(a.id) + " " + Describe(a, now);
            if (a.hm >= 0) ctx += "（" + RepeatText(a.days) + "）";
            if (!a.label.empty()) ctx += " " + a.label;
            ctx += "；";
        }
    }
    return ctx;
}

/* ---------- 大模型的工具 ---------- */

void NomiClock::RegisterTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool(
        "self.alarm.add",
        "定一个闹钟（到点响铃，屏幕上显示事由）。主人说\"明早 7 点叫我\"\"每天 7 点半叫我起床\""
        "\"工作日 9 点提醒我打卡\"\"周五下午 3 点提醒我开会\"时调用。\n"
        "time：24 小时制 HH:MM。repeat：once 只响一次（默认）、daily 每天、weekdays 周一到周五、"
        "weekends 周六周日。date：只响一次时可选，YYYY-MM-DD，不填就是 time 之后最近的那次。"
        "label：事由，简短，比如\"起床\"\"开会\"，没有就空着。\n"
        "\"X 分钟后\"\"半小时后\"这种倒计时用 self.reminder.add。",
        PropertyList({Property("time", kPropertyTypeString),
                      Property("repeat", kPropertyTypeString, std::string("once")),
                      Property("date", kPropertyTypeString, std::string("")),
                      Property("label", kPropertyTypeString, std::string("")).SetMaxLength(90)}),
        [this](const PropertyList& p) -> ToolResult {
            if (!TimeValid()) return std::unexpected(std::string("设备还没对上时间，联网对时后再设"));
            int h = -1, m = -1;
            std::string time_arg = p["time"].value<std::string>();
            if (sscanf(time_arg.c_str(), "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 || m > 59) {
                return std::unexpected(std::string("time 要写成 24 小时制 HH:MM，比如 07:30"));
            }
            std::string repeat = p["repeat"].value<std::string>();
            Alarm a;
            a.label = p["label"].value<std::string>();
            time_t now = time(nullptr);
            if (repeat.empty() || repeat == "once") {
                std::string date = p["date"].value<std::string>();
                struct tm tm = Local(now);
                if (!date.empty()) {
                    int y, mo, d;
                    if (sscanf(date.c_str(), "%d-%d-%d", &y, &mo, &d) != 3) {
                        return std::unexpected(std::string("date 要写成 YYYY-MM-DD"));
                    }
                    tm.tm_year = y - 1900;
                    tm.tm_mon = mo - 1;
                    tm.tm_mday = d;
                }
                tm.tm_hour = h;
                tm.tm_min = m;
                tm.tm_sec = 0;
                tm.tm_isdst = -1;
                a.at = mktime(&tm);
                if (date.empty() && a.at <= now) {
                    tm.tm_mday += 1;  // 今天这个点已经过了，就是明天
                    a.at = mktime(&tm);
                }
                if (a.at <= now) return std::unexpected(std::string("这个时间已经过了"));
            } else {
                a.hm = h * 60 + m;
                if (repeat == "daily") a.days = kDaily;
                else if (repeat == "weekdays") a.days = kWeekdays;
                else if (repeat == "weekends") a.days = kWeekends;
                else return std::unexpected(std::string("repeat 只能是 once、daily、weekdays、weekends"));
            }
            if (AddAlarm(a) < 0) {
                return std::unexpected(std::string("闹钟已经有 10 个了，先删掉几个（self.alarm.remove）"));
            }
            std::string text = "已设好：" + Describe(a, now);
            if (a.hm >= 0) text += "（" + RepeatText(a.days) + "）";
            if (!a.label.empty()) text += " " + a.label;
            return text;
        });

    mcp.AddTool(
        "self.reminder.add",
        "倒计时提醒：从现在起过多少秒响铃。主人说\"25 分钟后叫我\"\"半小时后提醒我关火\"\"番茄钟\"时调用。"
        "seconds：多少秒后（10 到 86400），label：事由，比如\"关火\"\"休息一下\"，没有就空着。",
        PropertyList({Property("seconds", kPropertyTypeInteger, 10, 86400),
                      Property("label", kPropertyTypeString, std::string("")).SetMaxLength(90)}),
        [this](const PropertyList& p) -> ToolResult {
            if (!TimeValid()) return std::unexpected(std::string("设备还没对上时间，联网对时后再设"));
            Alarm a;
            int seconds = p["seconds"].value<int>();
            time_t now = time(nullptr);
            a.at = now + seconds;
            a.label = p["label"].value<std::string>();
            if (AddAlarm(a) < 0) {
                return std::unexpected(std::string("闹钟已经有 10 个了，先删掉几个（self.alarm.remove）"));
            }
            struct tm tm = Local(a.at);
            char buf[64];
            snprintf(buf, sizeof(buf), "已设好：%02d:%02d:%02d 提醒（%d 分 %d 秒后）", tm.tm_hour, tm.tm_min,
                     tm.tm_sec, seconds / 60, seconds % 60);
            return std::string(buf) + (a.label.empty() ? std::string() : " " + a.label);
        });

    mcp.AddTool("self.alarm.list", "列出已设的闹钟和倒计时提醒，带编号（删除时用）。", PropertyList(),
                [this](const PropertyList& p) -> ToolResult {
                    (void)p;
                    std::string text = AlarmsText();
                    if (text.empty()) return std::string("还没有闹钟和提醒");
                    std::string out;
                    size_t pos = 0;
                    while (pos <= text.size()) {
                        size_t nl = text.find('\n', pos);
                        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
                        size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
                        out += "#" + line.substr(0, t1) + " " + line.substr(t1 + 1, t2 - t1 - 1);
                        if (t2 + 1 < line.size()) out += " " + line.substr(t2 + 1);
                        out += "\n";
                        if (nl == std::string::npos) break;
                        pos = nl + 1;
                    }
                    return out;
                });

    mcp.AddTool("self.alarm.remove",
                "删除闹钟或倒计时提醒。id 是编号（系统提示里的 #编号，或 self.alarm.list 列出的）；"
                "id 为 0 删除全部，删全部前先和主人确认。",
                PropertyList({Property("id", kPropertyTypeInteger, 0, 1000000)}),
                [this](const PropertyList& p) -> ToolResult {
                    int id = p["id"].value<int>();
                    if (!DeleteAlarm(id)) return std::unexpected(std::string("没有这个编号的闹钟"));
                    return std::string(id == 0 ? "已全部删除" : "已删除");
                });

    mcp.AddTool("self.weather.set_city",
                "设置天气预报的城市。主人说\"我在深圳\"\"天气改成北京的\"时调用；city 写中文城市名，"
                "写空字符串表示按网络位置自动判断。",
                PropertyList({Property("city", kPropertyTypeString).SetMaxLength(60)}),
                [this](const PropertyList& p) -> ToolResult {
                    std::string city = p["city"].value<std::string>();
                    Settings(kCityNs, true).SetString(kCityKey, city);
                    weather_next_us_ = 0;  // 下一秒（待机时）就重新查
                    return std::string("好的，天气改成") + (city.empty() ? "按网络位置自动判断" : city) +
                           "，过一会儿时钟页上就更新了";
                });
}
