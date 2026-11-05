// Nomi 的系统部分怎么起来：板子把屏幕和硬件准备好之后调 Start，这里按开着的能力（CONFIG_NOMI_*，见 nomi/Kconfig）
// 一样样启动，登记大模型工具、每轮对话的系统提示、网页设置页的接口。
//
// 分工：板子（boards/…）只管引脚和硬件——屏幕、触摸、声音、电池、背光、按键、休眠，以及有的话初始化摄像头
// （NomiCamera::Init）、告诉 SD 卡接在哪（NomiSdCard::Configure）。用不用、怎么用这些硬件是 Nomi 的事。
#pragma once

#include <functional>

class NomiDisplay;

namespace NomiSystem {

struct Hooks {
    // 有事要让人看见（闹钟响了）：重置休眠计时、屏幕关着或调暗了就亮起来
    std::function<void()> wake_screen;
};

// 板子构造函数的最后调。这时板子单例还没建完，这里和它启动的东西都不能调 Board::GetInstance()
void Start(NomiDisplay* display, const Hooks& hooks);

// 网络栈起来之后调（WifiBoard::StartNetwork 之后）：离线热点的事件要挂在它建的默认事件循环上
void OnNetworkStarted();

// 睡着一会儿后板子要调暗屏幕时先问一句：电子相框在放就别调暗（当相框用就是要亮着看）
bool KeepBright();

}  // namespace NomiSystem
