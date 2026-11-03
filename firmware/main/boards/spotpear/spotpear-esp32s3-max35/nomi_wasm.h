// 应用包的运行时：乐鑫的 wasm-micro-runtime 解释执行 app.wasm。系统接口见 docs/app-sdk.md。
//
// 每个跑着的应用一个"进程"：自己的 pthread（栈在 PSRAM；运行时要求在 pthread 里跑）、自己的模块实例和事件队列，
// 所以音乐应用能在后台放歌、画首页卡片，同时前台开着别的应用。
//
// 界面能挂上、拿下：从桌面打开时挂上（NOMI_EV_SHOW），右划退出时拿下（NOMI_EV_HIDE）。拿下后有理由的进程
// 留在后台——有首页卡片、或者它放的东西还在放（像手机的音乐在后台放）；没理由的就结束。
// 应用改界面时拿显示锁，拿到锁先看界面还在不在，不在就什么都不做——所以关界面不用等应用。
#pragma once

#include <string>

#include "nomi_app.h"

class Display;
struct NomiPackage;

namespace NomiWasm {

// 运行时初始化（板子构造时调一次）。display 用来拿显示锁：板子构造时还不能调
// Board::GetInstance()（单例还没建完，再进去会自己等自己），所以由板子直接传进来
void Init(Display* display);
Display* GetDisplay();

// 给应用包登记到桌面的 nomi_app_t 用（都在 LVGL 锁里调）
void Open(const nomi_app_t* app, lv_obj_t* root);
void Close(const nomi_app_t* app);
bool Back(const nomi_app_t* app);

// 有首页卡片的应用：开机后在后台跑起来画卡片
void StartBackground(const NomiPackage* pkg);
// 应用被删、换新版本：结束它的进程
void Kill(const std::string& id);

// 系统的播放控件（收听页、ui_player）点了上一首 / 下一首：交给放这个的应用（code 2 下一首、3 上一首）。
// 现在放的不是应用放的返回 false
bool MediaCommand(int code);
// 首页卡片（应用画的「正在放」）上点了放 / 停：在放就停；停着的是卡片应用自己的就接着放；
// 别的交给卡片应用决定放什么（NOMI_MEDIA_PLAY）。LVGL 锁里调
void TileToggle();

// 大模型调了 self.app.command：交给那个应用（没在跑就在后台拉起来），等它回话。出错时返回 "!原因"；
// end_conversation 为 true 时应用希望这句说完就回待机（比如开始放歌了）
std::string VoiceCommand(const std::string& app, const std::string& command, const std::string& arg,
                         bool* end_conversation);

// 应用自带的网页（/app/<id>/）发来的请求：交给应用（NOMI_EV_WEB），等它回话（一般是 JSON）。出错时返回 "!原因"
std::string WebCall(const std::string& app, const std::string& body);

// 每轮对话的系统提示里加的：前台开着的应用说它在干什么（assistant_context），没有为空
std::string PromptContext();

// 在系统任务里跑一件事（开机扫描装好的包之类，要读 SD 卡的活）
void RunInWorker(void (*fn)(void*), void* arg);

}  // namespace NomiWasm
