// 离线热点：断网时设备自己广播一个热点（APSTA，和正在重试的站点共存），手机连上来就能
// 用网页设置页控制——80 端口的网页服务本来就在听所有网卡，热点一开它自动可达；
// DnsServer 把所有域名都指到设备，手机连上会自动弹出设置页（captive portal）。
// 没开成功过连接就开热点，拿到 IP（联网成功）自动关，站点照常退避重试，不受影响。
#pragma once

#include <string>

namespace NomiHotspot {

// 开机调一次：注册事件——断网满一分钟开热点，拿到 IP 关。只在事件回调里干活，不碰 Wi-Fi
void Init();

bool Active();  // 热点开着（设置页用它决定显示连接热点还是局域网的二维码）
std::string Ssid();
std::string Url();  // http://192.168.4.1/

// 手动进配网热点前调一下：配网热点用同一个 AP 接口，别让两边抢（设置页「开配网热点」那条路）
void StopForConfigMode();

}  // namespace NomiHotspot
