#include "adc_battery_monitor.h"
#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "mcp_server.h"
#include "esp32_camera.h"
#include "nomi_clock.h"
#include "nomi_display.h"
#include "nomi_system.h"
#include "nomi_ui.h"
#if CONFIG_NOMI_CAMERA
#include "nomi_camera.h"
#endif
#if CONFIG_NOMI_SDCARD
#include "nomi_sdcard.h"
#endif
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

// 这里只管这块板子的引脚和硬件；界面和系统能力（应用、播放、SD 卡上的东西……）在 main/nomi/，
// 开机最后交给 NomiSystem::Start，开哪些能力看 nomi/Kconfig 的 CONFIG_NOMI_*

// esp32-camera 内部的启停（driver/cam_hal.c，没进公开头文件）：只停 DMA 和场同步中断，
// 传感器、帧缓冲、采集任务都留着，再 cam_start() 从下一帧接着收
extern "C" {
void cam_stop(void);
void cam_start(void);
}

class SpotpearMax35Board : public WifiBoard {
private:
    Button boot_button_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    NomiDisplay* display_ = nullptr;
    AdcBatteryMonitor* battery_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    Esp32Camera* camera_ = nullptr;
    std::atomic<bool> sleeping_{false};
    std::atomic<bool> screen_off_{false};  // 到息屏时间把背光关了（不一定在睡眠里，见 WakeScreen）
    bool keep_awake_ = false;  // 前台应用要屏幕一直亮着（导航）：休眠计时停着（只在主循环里读写）
    bool holding_ = false;     // BOOT 键按住说话中
    bool alarm_press_ = false;  // 这次按 BOOT 键是为了关闹钟：松开、单击都不再当说话
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
        io_config.flags.psram_dma_direct = 1;  // LVGL 的绘制缓冲在 PSRAM，直接 DMA 发出去，不经内部 RAM 中转
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
#if CONFIG_NOMI_CAMERA
        NomiCamera::GatePanel(panel);  // 摄像头收帧时先别刷屏（两边的 DMA 抢 PSRAM，见 nomi_camera.h）
#endif

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
            WakeScreen();
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
            screen_off_ = false;
            esp_timer_stop(dim_timer_);
            display_->SetSleep(false);
            GetBacklight()->RestoreBrightness();
        });
        // 到点只息屏，不真关机（这块板没有软关机电路）
        power_save_timer_->OnShutdownRequest([this]() {
            screen_off_ = true;
            GetBacklight()->SetBrightness(0);
        });
        power_save_timer_->SetEnabled(!keep_awake_);
    }

    // 有人用了：重置休眠计时，屏幕关着的话亮起来。
    // 「多久后睡着」设成从不、「多久后息屏」有时间时，到点只关背光、不进睡眠，PowerSaveTimer::WakeUp
    // 不会回调 OnExitSleepMode，亮度要在这里还原——不然摸屏、唤醒词、按键都叫不亮，只能重启
    void WakeScreen() {
        power_save_timer_->WakeUp();
        if (screen_off_.exchange(false)) GetBacklight()->RestoreBrightness();
    }

    void InitializePowerSave() {
        const esp_timer_create_args_t dim_args = {
            .callback = [](void* arg) {
                auto* self = static_cast<SpotpearMax35Board*>(arg);
                // 电子相框在放就不调暗（当相框用就是要亮着看），到息屏时间照样关
                if (self->sleeping_ && !NomiSystem::KeepBright()) self->GetBacklight()->SetBrightness(DIM_BRIGHTNESS);
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "dim_backlight",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&dim_args, &dim_timer_));
        CreatePowerSaveTimer();
        NomiDisplay::on_user_activity = [this]() { WakeScreen(); };
        // 前台应用要屏幕一直亮着（地图导航时）：屏幕关着的话亮起来、休眠计时停掉，放开后从头数
        NomiDisplay::on_keep_awake = [this](bool on) {
            Application::GetInstance().Schedule([this, on]() {
                keep_awake_ = on;
                if (on) WakeScreen();
                power_save_timer_->SetEnabled(!on);
            });
        };
        // 设置页改了时间就换一个计时器；在主循环里换，避免和其他调用撞上
        NomiDisplay::on_power_timing_changed = [this]() {
            Application::GetInstance().Schedule([this]() {
                PowerSaveTimer* old = power_save_timer_;
                CreatePowerSaveTimer();
                delete old;
            });
        };
    }

#if CONFIG_NOMI_CAMERA
    // 摄像头只给 Nomi 用（大模型看一眼、相机应用），关了 CONFIG_NOMI_CAMERA 就不初始化
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
        // 单缓冲：拿着这一帧时驱动没地方写，DMA 一定是停着的；每一帧都经过 nomi_camera 的手、帧尾都打得上记号
        // （双缓冲时驱动自己丢掉的帧不过手，记号没法保证）。也省下 600KB PSRAM
        config.fb_count = 1;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
        camera_ = new Esp32Camera(config);
        // esp_camera_init 一结束就开始连续采集：VGA RGB565 每秒十几帧，每帧 600KB 写进 PSRAM。
        // 原来驱动的采集任务（优先级 23）用 CPU 从内部内存的 DMA 缓冲往 PSRAM 拷，抢走大量 CPU 和 PSRAM 带宽：
        // 音频编码掉帧（Encode queue is full）、AFE 喂不进（Ringbuffer full）、界面滑动也跟着卡。
        // 2026-11-04 起 DMA 直接写 PSRAM（CONFIG_CAMERA_PSRAM_DMA，省下内部内存里 30KB 的 DMA 缓冲），
        // 但一直采集还是占 PSRAM 带宽。只有拍照、取景器开着时才需要画面，平时停着
        if (esp_camera_sensor_get() != nullptr) {
            cam_stop();
            NomiCamera::Init();  // 拍照、取景器都在 nomi_camera.cc
        }
    }
#endif

#if CONFIG_NOMI_SDCARD
    // TF 卡的引脚交给 Nomi（用的时候它自己挂载）。卸载后把 GPIO43 还给功放（R30 焊上的板子它是功放开关），
    // 电平按现在出不出声来
    void InitializeSdCard() {
        auto& sd = NomiSdCard::GetInstance();
        sd.Configure({SD_SPI_HOST, SD_CS_PIN, SD_MISO_PIN, SD_CLK_PIN, SD_MOSI_PIN});
        sd.on_bus_released = [this]() {
            gpio_set_direction(AUDIO_CODEC_PA_PIN, GPIO_MODE_OUTPUT);
            gpio_set_level(AUDIO_CODEC_PA_PIN, GetAudioCodec()->output_enabled() ? 1 : 0);
        };
    }
#endif

    // BOOT 键说话：默认按住说话（像对讲机），设置里可改成点一下开始、说完自动结束
    bool HoldToTalk() { return NomiDisplay::GetSavedInt(NOMI_KEY_TALK_KEY, 0) == 0; }

    void InitializeButtons() {
        boot_button_.OnPressDown([this]() {
            WakeScreen();
            // 闹钟在响：按一下就是关掉它
            alarm_press_ = NomiClock::GetInstance().IsRinging();
            if (alarm_press_) {
                NomiClock::GetInstance().StopRinging();
                return;
            }
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
            if (alarm_press_) return;
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
    }

public:
    SpotpearMax35Board() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializePowerSave();
        InitializeBattery();
#if CONFIG_NOMI_CAMERA
        InitializeCamera();
#endif
#if CONFIG_NOMI_SDCARD
        InitializeSdCard();
#endif
        InitializeButtons();
        InitializeTools();
        NomiSystem::Start(display_, {
                                        // 闹钟响了：睡着 / 息屏 / 调暗了都亮起来
                                        .wake_screen =
                                            [this]() {
                                                WakeScreen();
                                                GetBacklight()->RestoreBrightness();
                                            },
                                    });
        GetBacklight()->RestoreBrightness();
    }

    // 离线热点的事件要挂在默认事件循环上，而循环是 WifiManager::Initialize 建的（构造函数里还没有，
    // 在那儿注册会 abort——崩过一轮），所以等网络栈起来再交给 Nomi
    void StartNetwork() override {
        WifiBoard::StartNetwork();
        NomiSystem::OnNetworkStarted();
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
            WakeScreen();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(SpotpearMax35Board);
