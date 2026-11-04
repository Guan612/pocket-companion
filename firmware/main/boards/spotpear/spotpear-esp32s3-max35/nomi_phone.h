// 手机连接（系统能力）：蓝牙 BLE，兼容 Gadgetbridge（开源的安卓手表伴侣 App）跟 Bangle.js 说的那套协议。
// 手机上装 Gadgetbridge（再加 OsmAnd / CoMaps 导航）就能把导航、手机 GPS、通知这些发过来，不用自己写手机 App。
//
// 分层：系统只做最薄的一层——蓝牙、广播、按行切消息、认出每条的类型（JSON 的 "t"），原样转交给要这一类的应用
// （应用 phone_listen）；系统自己只处理对时。消息里各字段什么意思（导航的转向、GPS 的速度……）归应用。
//
// 协议（看的 Gadgetbridge 源码 BangleJSCoordinator / BangleJSDeviceSupport / BangleJSConstants）：
// - Nordic UART 服务 6e400001-b5a3-f393-e0a9-e50e24dcca9e：手机写 6e400002，板子用 6e400003 通知回去。
// - Gadgetbridge 扫描时只认这个服务，设备名要是 Bangle.js / Espruino / Puck.js 这类开头：叫「Bangle.js Nomi」。
// - 手机发来的一行一条："\x10GB({...JSON...})\n"（JSON 里 "t" 是类型：nav 导航、gps 手机定位、notify 通知……）；
//   连上时还发一行 JavaScript 对时："\x10setTime(秒);E.setTimeZone(时区);"。
// - 板子回的也是一行一条 JSON，比如 {"t":"gps_power","status":true} 让手机开始推定位（Gadgetbridge 里这台设备的
//   「使用手机的GPS数据」要打开）。行尾要 \r\n（只发 \n，它会把最后一个字符当 \r 吃掉）。
#pragma once

#include <functional>
#include <string>

namespace NomiPhone {

void Start();  // 板子初始化时调一次：开蓝牙、开始广播

bool Connected();
// 一行 JSON 发给手机（自动加换行、按 MTU 切开）。没连上返回 false
bool Send(const std::string& json);

// 收到一条 GB(...) 消息：type 是 "t"，json 是整条（UTF-8，不带 GB( )）。在蓝牙任务里调，别做耗时的事
using MessageListener = std::function<void(const std::string& type, const std::string& json)>;
using StateListener = std::function<void(bool connected)>;
void SetListeners(MessageListener on_message, StateListener on_state);  // 应用运行时设一次

}  // namespace NomiPhone
