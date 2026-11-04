#include "nomi_hotspot.h"

#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <esp_netif.h>

#include <atomic>

#include "dns_server.h"
#include "application.h"
#include "board.h"
#if CONFIG_DIRECT_API
#include "direct_api_config_server.h"
#endif
#include "wifi_manager.h"

#define TAG "NomiHotspot"

namespace {

// 断网满一分钟才开热点：Wi-Fi 闪断、路由器重启个几十秒都不打扰
constexpr int64_t kStartDelayUs = 60 * 1000 * 1000;

esp_timer_handle_t s_timer = nullptr;
esp_netif_t *s_ap_netif = nullptr;   // 建了就不销毁（无线驱动跑着时销毁 netif 有风险），下次直接复用
DnsServer *s_dns = nullptr;
std::atomic<bool> s_active{false};   // 热点开着
std::atomic<bool> s_pending{false};  // 60 秒的定时器在数着
std::string s_ssid;

void BuildSsid() {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char buf[16];
    snprintf(buf, sizeof(buf), "Nomi-%02X%02X", mac[4], mac[5]);
    s_ssid = buf;
}

// 要在主循环里调（esp_timer / 事件任务只排任务）。开 AP 不影响站点：esp_wifi_set_mode(APSTA)
// 只是给跑着的站点加开一个 AP 面，重试照旧（WifiManager 只管自己的那套互斥，不拦这些调用）
void StartNow() {
    if (s_active || WifiManager::GetInstance().IsConfigMode()) return;
    if (s_ssid.empty()) BuildSsid();

    esp_netif_ip_info_t ip = {};
    IP4_ADDR(&ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_set_ip_info(s_ap_netif, &ip);
        esp_netif_dhcps_start(s_ap_netif);
    }

    wifi_config_t ap = {};
    strcpy((char *)ap.ap.ssid, s_ssid.c_str());
    ap.ap.ssid_len = s_ssid.size();
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // 当 AP 期间不省电，不然 beacon 不稳

    if (!s_dns) {
        s_dns = new DnsServer();
        s_dns->Start(ip.gw);  // 所有域名都指到设备，手机连上自动弹设置页
    }
    s_active = true;

#if CONFIG_DIRECT_API
    // 没起就起（开机还没联网时协议没跑，服务不在），起了就把网址刷成热点地址
    DirectApiConfigServer::Start();
#endif
    ESP_LOGI(TAG, "Hotspot %s up, internal free %u", s_ssid.c_str(),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
}

void StopNow() {
    if (!s_active) return;
    s_active = false;
    if (s_dns) {
        s_dns->Stop();
        delete s_dns;
        s_dns = nullptr;
    }
    // 不能学配网热点那样 esp_wifi_stop()——会把正在重试的站点一起停掉，只收 AP 面
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // 还回当 AP 前的省电档
    ESP_LOGI(TAG, "Hotspot down");
#if CONFIG_DIRECT_API
    DirectApiConfigServer::Start();  // 网址刷回局域网地址（服务没起的话只算个网址，不起服务）
#endif
}

void OnStartTimer(void *) {
    s_pending = false;
    Application::GetInstance().Schedule([]() { StartNow(); });
}

void OnWifiEvent(void *, esp_event_base_t, int32_t id, void *) {
    // 事件任务里只动定时器和标记，Wi-Fi 的活儿排进主循环干
    if (id == WIFI_EVENT_STA_DISCONNECTED && !s_active && !s_pending) {
        s_pending = true;
        esp_timer_start_once(s_timer, kStartDelayUs);
    }
}

void OnGotIp(void *, esp_event_base_t, int32_t id, void *) {
    if (s_pending) {
        s_pending = false;
        esp_timer_stop(s_timer);
    }
    Application::GetInstance().Schedule([]() { StopNow(); });
}

}  // namespace

namespace NomiHotspot {

void Init() {
    esp_timer_create_args_t timer_args = {
        .callback = OnStartTimer,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "nomi_hotspot",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_timer));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &OnWifiEvent, nullptr, nullptr));
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &OnGotIp, nullptr, nullptr));

    // 开机就数着：存在压根连不上的地方（存的 Wi-Fi 不在空中）就没有断连事件，得靠这一趟
    s_pending = true;
    esp_timer_start_once(s_timer, kStartDelayUs);
}

bool Active() { return s_active; }

std::string Ssid() {
    if (s_ssid.empty()) BuildSsid();
    return s_ssid;
}

std::string Url() { return "http://192.168.4.1/"; }

void StopForConfigMode() {
    if (s_pending) {
        s_pending = false;
        esp_timer_stop(s_timer);
    }
    StopNow();
}

}  // namespace NomiHotspot
