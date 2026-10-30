// Nomi 显示：用 nomi_ui（和浏览器模拟器同一份界面代码）替换小智自带的界面。
// 小智的界面照常创建但整体隐藏，它调的 SetStatus / SetChatMessage / SetEmotion 等转给 nomi_ui。
#pragma once

#include "display/lcd_display.h"

#include <functional>
#include <memory>
#include <string>

class NomiDisplay : public SpiLcdDisplay {
public:
    NomiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                bool swap_xy);

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    bool SetTextFont(std::shared_ptr<LvglFont> text_font) override;
    void UpdateStatusBar(bool update_all = false) override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

    // 由板子的休眠计时器调用
    void SetSleep(bool sleeping);
    // 以下由板子设置
    static bool aec_supported;                          // 有没有喇叭回采（焊了 ES7210）
    static std::function<void()> on_user_activity;      // 触摸时，用来重置休眠计时
    static std::function<void()> on_take_photo;         // 设置页里点"拍照"
    static std::function<void()> on_power_timing_changed;  // 设置页里改了调暗 / 息屏时间

    // 设置页里选的调暗 / 息屏时间（秒），-1 表示从不
    static void GetIdleSeconds(int& dim_seconds, int& off_seconds);

private:
    uint16_t* face_buf_ = nullptr;
    bool ui_ready_ = false;
};
