// 应用包的运行时：乐鑫的 wasm-micro-runtime 解释执行 app.wasm。系统接口见 docs/app-sdk.md。
//
// 一个常驻任务（栈在 PSRAM）一次跑一个应用：打开时把应用交给它，它加载、调 nomi_main，之后一直等事件
// （点击、返回、定时器…）调 nomi_event，关掉后收回内存、回去等下一个。应用改界面时拿显示锁；
// 拿到锁先看应用是不是已经关了，关了就什么都不做——所以关应用不用等应用任务，界面不会卡。
#pragma once

#include "nomi_app.h"

class Display;

namespace NomiWasm {

// 运行时初始化、建常驻任务（板子构造时调一次）。display 用来拿显示锁：板子构造时还不能调
// Board::GetInstance()（单例还没建完，再进去会自己等自己），所以由板子直接传进来
void Init(Display* display);
Display* GetDisplay();

// 给应用包登记到桌面的 nomi_app_t 用（都在 LVGL 锁里调）
void Open(const nomi_app_t* app, lv_obj_t* root);
void Close(const nomi_app_t* app);
bool Back(const nomi_app_t* app);

// 在应用任务里跑一件事（开机扫描装好的包之类，要读 SD 卡、栈放 PSRAM 也行的活）
void RunInWorker(void (*fn)(void*), void* arg);

}  // namespace NomiWasm
