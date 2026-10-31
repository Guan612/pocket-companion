#include "adc_battery_monitor.h"
#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "mcp_server.h"
#include "esp32_camera.h"
#include "image_to_jpeg.h"
#if CONFIG_DIRECT_API
#include "direct_api_protocol.h"
#endif
#include "nomi_display.h"
#include "nomi_ui.h"
#include "power_save_timer.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_st7796.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_lcd_touch_gt911.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_lvgl_port.h>

#include <atomic>
#include <mutex>

#define TAG "SpotpearMax35"

class SpotpearMax35Board : public WifiBoard {
private:
    Button boot_button_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    NomiDisplay* display_ = nullptr;
    AdcBatteryMonitor* battery_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    Esp32Camera* camera_ = nullptr;
    std::mutex camera_mutex_;        // 取景任务和提问时抓拍不能同时拿帧
    std::atomic<bool> vision_on_{false};
    std::atomic<bool> sleeping_{false};
    bool holding_ = false;           // BOOT 键按住说话中
    esp_timer_handle_t dim_timer_ = nullptr;  // 睡着一会儿后调暗屏幕

    // ES8311、GT911、摄像头 SCCB 共用这条总线
    void InitializeI2c() {
        i2c_master_bus_config_t cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {.enable_internal_pullup = 1},
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = 0;
        io_config.pclk_hz = DISPLAY_SPI_HZ;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &panel_io));

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7796(panel_io, &panel_config, &panel));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));  // 没有复位脚时发软复位命令
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        display_ = new NomiDisplay(panel_io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                   DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    // INT 没接 GPIO，GT911 上电时的地址可能是 0x5D 也可能是 0x14，两个都探一下
    void InitializeTouch() {
        uint16_t addr = 0;
        for (uint16_t candidate : {ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS,
                                   ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP}) {
            if (i2c_master_probe(i2c_bus_, candidate, 50) == ESP_OK) {
                addr = candidate;
                break;
            }
        }
        if (addr == 0) {
            ESP_LOGW(TAG, "GT911 not found, touch disabled");
            return;
        }

        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
        tp_io_config.dev_addr = addr;
        tp_io_config.scl_speed_hz = 400 * 1000;
        esp_lcd_panel_io_handle_t tp_io = nullptr;
        if (esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create touch IO");
            return;
        }

        esp_lcd_touch_config_t tp_cfg = {};
        tp_cfg.x_max = TOUCH_X_MAX;
        tp_cfg.y_max = TOUCH_Y_MAX;
        tp_cfg.rst_gpio_num = TOUCH_RST_PIN;
        tp_cfg.int_gpio_num = TOUCH_INT_PIN;
        tp_cfg.flags.swap_xy = TOUCH_SWAP_XY;
        tp_cfg.flags.mirror_x = TOUCH_MIRROR_X;
        tp_cfg.flags.mirror_y = TOUCH_MIRROR_Y;
        esp_lcd_touch_handle_t tp = nullptr;
        if (esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create GT911 driver");
            return;
        }

        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "GT911 touch ready at 0x%02X", addr);
    }

    void InitializeBattery() {
        battery_ = new AdcBatteryMonitor(BATTERY_ADC_UNIT, BATTERY_ADC_CHANNEL, BATTERY_UPPER_RESISTOR,
                                         BATTERY_LOWER_RESISTOR, BATTERY_USB_DETECT_PIN);
        // 插拔 USB 时立刻刷新电量显示，并重置休眠计时
        battery_->OnChargingStatusChanged([this](bool charging) {
            power_save_timer_->WakeUp();
            display_->UpdateStatusBar(true);
        });
    }

    // cpu_max_freq 传 -1：休眠时不降频，唤醒词保持监听。时间按设置页的选项，-1 表示从不
    // 空闲三段：先正常亮度显示睡着动画，睡着一会儿后调暗，到息屏时间再关屏。
    // 小智的 PowerSaveTimer 只有"睡眠 / 关机"两档，中间的调暗用 dim_timer_ 补
    void CreatePowerSaveTimer() {
        int sleep_seconds, off_seconds;
        NomiDisplay::GetIdleSeconds(sleep_seconds, off_seconds);
        // 息屏不能早于"睡着再调暗"之后，保证顺序是先睡、再暗、再关
        if (sleep_seconds > 0 && off_seconds > 0 && off_seconds < sleep_seconds + DIM_AFTER_SLEEP_SECONDS) {
            off_seconds = sleep_seconds + DIM_AFTER_SLEEP_SECONDS;
        }
        power_save_timer_ = new PowerSaveTimer(-1, sleep_seconds, off_seconds);
        power_save_timer_->OnEnterSleepMode([this]() {
            sleeping_ = true;  // 取景也停掉
            display_->SetSleep(true);
            esp_timer_start_once(dim_timer_, DIM_AFTER_SLEEP_SECONDS * 1000000LL);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            sleeping_ = false;
            esp_timer_stop(dim_timer_);
            display_->SetSleep(false);
            GetBacklight()->RestoreBrightness();
        });
        // 到点只息屏，不真关机（这块板没有软关机电路）
        power_save_timer_->OnShutdownRequest([this]() { GetBacklight()->SetBrightness(0); });
        power_save_timer_->SetEnabled(true);
    }

    void InitializePowerSave() {
        const esp_timer_create_args_t dim_args = {
            .callback = [](void* arg) {
                auto* self = static_cast<SpotpearMax35Board*>(arg);
                if (self->sleeping_) self->GetBacklight()->SetBrightness(DIM_BRIGHTNESS);
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "dim_backlight",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&dim_args, &dim_timer_));
        CreatePowerSaveTimer();
        NomiDisplay::on_user_activity = [this]() { power_save_timer_->WakeUp(); };
        // 设置页改了时间就换一个计时器；在主循环里换，避免和其他调用撞上
        NomiDisplay::on_power_timing_changed = [this]() {
            Application::GetInstance().Schedule([this]() {
                PowerSaveTimer* old = power_save_timer_;
                CreatePowerSaveTimer();
                delete old;
            });
        };
    }

    void InitializeCamera() {
        camera_config_t config = {};
        config.pin_pwdn = CAMERA_PIN_PWDN;
        config.pin_reset = CAMERA_PIN_RESET;
        config.pin_xclk = CAMERA_PIN_XCLK;
        config.pin_sccb_sda = -1;  // 复用已初始化的 I2C 总线
        config.pin_sccb_scl = -1;
        config.sccb_i2c_port = I2C_NUM_0;
        config.pin_d7 = CAMERA_PIN_D7;
        config.pin_d6 = CAMERA_PIN_D6;
        config.pin_d5 = CAMERA_PIN_D5;
        config.pin_d4 = CAMERA_PIN_D4;
        config.pin_d3 = CAMERA_PIN_D3;
        config.pin_d2 = CAMERA_PIN_D2;
        config.pin_d1 = CAMERA_PIN_D1;
        config.pin_d0 = CAMERA_PIN_D0;
        config.pin_vsync = CAMERA_PIN_VSYNC;
        config.pin_href = CAMERA_PIN_HREF;
        config.pin_pclk = CAMERA_PIN_PCLK;
        config.xclk_freq_hz = CAMERA_XCLK_HZ;
        config.ledc_timer = LEDC_TIMER_1;  // 0 号被背光 PWM 占了
        config.ledc_channel = LEDC_CHANNEL_1;
        config.pixel_format = PIXFORMAT_RGB565;
        config.frame_size = FRAMESIZE_VGA;  // 640×480，识图够清楚，JPEG 约几十 KB
        config.jpeg_quality = 12;
        config.fb_count = 2;  // 取景和提问时抓拍可能同时要帧
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;  // 取景要最新的一帧，不要排队的旧帧
        camera_ = new Esp32Camera(config);

        vision_on_ = NomiDisplay::GetSavedInt(NOMI_KEY_MODE, NOMI_MODE_CHAT) == NOMI_MODE_VISION;
        NomiDisplay::on_mode_changed = [this](int mode) { vision_on_ = mode == NOMI_MODE_VISION; };
#if CONFIG_DIRECT_API
        // 视觉模式：每轮说完话时抓一帧，和问题一起发给支持图片输入的大模型
        DirectApiProtocol::image_provider = [this]() {
            return vision_on_ ? CaptureJpeg() : std::string();
        };
#endif
        xTaskCreatePinnedToCore(PreviewTask, "cam_preview", 4096, this, 2, nullptr, 1);
    }

    // 视觉模式且醒着时，约每 150 毫秒取一帧，缩成 320×240 送给界面做取景
    static void PreviewTask(void* arg) {
        auto* self = static_cast<SpotpearMax35Board*>(arg);
        auto* preview = static_cast<uint16_t*>(
            heap_caps_malloc(NOMI_PREVIEW_W * NOMI_PREVIEW_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM));
        while (preview != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(150));
            if (!self->vision_on_ || self->sleeping_) continue;
            std::lock_guard<std::mutex> lock(self->camera_mutex_);
            camera_fb_t* fb = esp_camera_fb_get();
            if (fb == nullptr) continue;
            if (fb->format == PIXFORMAT_RGB565 && fb->width >= NOMI_PREVIEW_W &&
                fb->height >= NOMI_PREVIEW_H) {
                // 按比例抽点缩小，同时把摄像头的大端 RGB565 换成本机字节序
                auto* src = reinterpret_cast<const uint16_t*>(fb->buf);
                for (int y = 0; y < NOMI_PREVIEW_H; y++) {
                    const uint16_t* row = src + (y * fb->height / NOMI_PREVIEW_H) * fb->width;
                    for (int x = 0; x < NOMI_PREVIEW_W; x++) {
                        preview[y * NOMI_PREVIEW_W + x] =
                            __builtin_bswap16(row[x * fb->width / NOMI_PREVIEW_W]);
                    }
                }
                self->display_->SetPreview(preview);
            }
            esp_camera_fb_return(fb);
        }
        vTaskDelete(nullptr);
    }

    // 取一帧 RGB565 转成 JPEG。摄像头输出是大端 RGB565，编码器要小端，先换字节
    std::string CaptureJpeg() {
        std::lock_guard<std::mutex> lock(camera_mutex_);
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb == nullptr) return "";
        std::string jpeg;
        if (fb->format == PIXFORMAT_RGB565) {
            size_t pixels = fb->width * fb->height;
            auto* buf = static_cast<uint16_t*>(heap_caps_malloc(pixels * 2, MALLOC_CAP_SPIRAM));
            if (buf != nullptr) {
                auto* src = reinterpret_cast<const uint16_t*>(fb->buf);
                for (size_t i = 0; i < pixels; i++) buf[i] = __builtin_bswap16(src[i]);
                uint8_t* out = nullptr;
                size_t out_len = 0;
                if (image_to_jpeg(reinterpret_cast<uint8_t*>(buf), pixels * 2, fb->width,
                                  fb->height, V4L2_PIX_FMT_RGB565, 80, &out, &out_len)) {
                    jpeg.assign(reinterpret_cast<char*>(out), out_len);
                    free(out);
                }
                heap_caps_free(buf);
            }
        }
        esp_camera_fb_return(fb);
        return jpeg;
    }

    // BOOT 键说话：默认按住说话（像对讲机），设置里可改成点一下开始、说完自动结束
    bool HoldToTalk() { return NomiDisplay::GetSavedInt(NOMI_KEY_TALK_KEY, 0) == 0; }

    void InitializeButtons() {
        boot_button_.OnPressDown([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (HoldToTalk() && app.GetDeviceState() != kDeviceStateStarting) {
                holding_ = true;
                app.StartListening();
            }
        });
        boot_button_.OnPressUp([this]() {
            if (holding_) {
                holding_ = false;
                Application::GetInstance().StopListening();
            }
        });
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            if (!HoldToTalk()) app.ToggleChatState();
        });
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.system.reconfigure_wifi",
                           "End this conversation and enter WiFi configuration mode.\n"
                           "**CAUTION** You must ask the user to confirm this action.",
                           PropertyList(), [this](const PropertyList& properties) {
                               EnterWifiConfigMode();
                               return true;
                           });
        // 表情脚本：主人明确要表演时大模型调这个工具；平时它在回复开头写 <face>…</face>（见 NomiDisplay::FacePrompt）
        mcp_server.AddTool("self.face.play",
                           "让你的脸表演一段动作。主人明确要你做表情、做鬼脸、跳舞、弹吉他之类时用；"
                           "script 是表情脚本 JSON（步骤数组，格式和回复里的 <face> 一样）。",
                           PropertyList({Property("script", kPropertyTypeString)}),
                           [this](const PropertyList& properties) -> ToolResult {
                               std::string error;
                               if (!display_->PlayFace(properties["script"].value<std::string>(), error)) {
                                   return std::unexpected(error);
                               }
                               return std::string("正在表演");
                           });
#if CONFIG_DIRECT_API
        DirectApiProtocol::face_prompt = NomiDisplay::FacePrompt();
        DirectApiProtocol::face_handler = [this](const std::string& json, std::string& error) {
            return display_->PlayFace(json, error);
        };
#endif
    }

public:
    SpotpearMax35Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializePowerSave();
        InitializeBattery();
        InitializeCamera();
        InitializeButtons();
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    // 焊了 ES7210 的版本由它录音：双麦克风 + 喇叭回采（MIC3），能做设备端回声消除；
    // 没焊就用 ES8311 自带的麦克风输入。两颗芯片的录音输出共用 GPIO10，板上只会焊其中一路
    AudioCodec* CreateAudioCodec() {
        if (i2c_master_probe(i2c_bus_, AUDIO_CODEC_ES7210_ADDR >> 1, 100) == ESP_OK) {
            ESP_LOGI(TAG, "ES7210 found, using dual-mic input with AEC reference");
            NomiDisplay::aec_supported = true;
            return new BoxAudioCodec(i2c_bus_, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                     AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                                     AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
                                     AUDIO_CODEC_ES8311_ADDR, AUDIO_CODEC_ES7210_ADDR, true);
        }
        ESP_LOGI(TAG, "ES7210 not found, using ES8311 microphone");
        return new Es8311AudioCodec(i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE,
                                    AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
                                    AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
                                    AUDIO_I2S_GPIO_DIN, AUDIO_CODEC_PA_PIN,
                                    AUDIO_CODEC_ES8311_ADDR, true, false);
    }

    virtual AudioCodec* GetAudioCodec() override {
        static AudioCodec* codec = CreateAudioCodec();
        return codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Camera* GetCamera() override { return camera_; }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = battery_->IsCharging();
        discharging = !charging;
        level = battery_->GetBatteryLevel();
        return true;
    }

    // 进入对话等高功耗状态时说明有人在用，重置休眠计时
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(SpotpearMax35Board);
