// 摄像头（系统能力）：给大模型看一眼、应用拍照存卡、取景器实时画面。
//
// - 只有一块帧缓冲（板子初始化时 fb_count = 1）：拿着它时驱动没地方写，DMA 一定停着。
// - 平时采集停着（cam_stop），拍照、取景器开着时才开。
// - 花屏：DMA 直接写 PSRAM（CONFIG_CAMERA_PSRAM_DMA）时，屏幕刷新的 DMA 从 PSRAM 读绘制缓冲，两边抢 PSRAM，
//   摄像头接口的小 FIFO 等不及就整段丢数据（一丢几十上百行），驱动这个模式不数字节，照样当整帧交出来。
//   所以收帧的那一会儿关上「刷屏闸门」（GatePanel 包了面板的 draw_bitmap）：LVGL 照常算、照常读触摸，
//   要刷屏的先等着（最多 1 秒，闸门出了岔子也不会把界面卡死）。
//   别用"把摄像头 DMA 通道优先级提到最高"：也能拍干净，但屏幕那条 DMA 被饿住，一次刷屏再也完不成，
//   LVGL 卡死在 wait_for_flushing，屏幕留一块蓝的（2026-11-05 上机踩过）。
// - 帧尾记号：每一帧还给驱动前在最后 16 字节写上记号并把缓存写回，DMA 收满一整帧会把它盖掉；
//   拿到的帧记号还在就是收短了，丢掉再收。开起来后的第一帧总是丢掉（上次停下时收到一半的帧会在这时交出来）。
// - 顶上 2～3 行偶发颜色错（品红 / 绿，像字节高低位错开；停住刷屏时也有，原因没查到）：丢掉顶上 16 行。
#pragma once

#include <esp_lcd_panel_ops.h>

#include <functional>
#include <string>

#include "lvgl.h"

namespace NomiCamera {

// 建好屏幕面板后调一次：包上刷屏闸门
void GatePanel(esp_lcd_panel_handle_t panel);
// 摄像头初始化成功（采集已停）后调
void Init();
bool Ready();

// 拍一张 JPEG（VGA 去掉顶上 16 行：640×464，传感器原样，横的）。要零点几秒到一两秒，别在界面任务、主循环里调；
// 失败返回空
std::string Capture();

// 取景器（显示锁里调）：占满 parent（应用的一页）。实时画面铺满整屏（横的，多出来的上下裁掉），右边浮一列放
// 应用的按钮——side 返回这一列（应用 ui_target 它）。控件删了预览就停、摄像头关掉
lv_obj_t* CreateViewfinder(lv_obj_t* parent, lv_obj_t** side);

// 应用的「问它」：拍好的照片交给大模型、叫起对话听主人问；主人没开口就问 question（空 = 默认问题）。
// 板子设（直连协议挂照片、主循环里开始对话），没设时应用的 camera_ask 返回找不到
extern std::function<bool(std::string jpeg, const std::string& question)> ask_handler;

}  // namespace NomiCamera
