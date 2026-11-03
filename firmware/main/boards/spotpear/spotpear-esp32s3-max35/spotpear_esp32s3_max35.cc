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
#include "direct_api_config_server.h"
#include "direct_api_protocol.h"
#endif
#include "nomi_clock.h"
#include "nomi_display.h"
#include "nomi_music.h"
#include "nomi_pkg.h"
#include "nomi_podcast.h"
#include "nomi_radio.h"
#include "nomi_sdcard.h"
#include "nomi_wasm.h"
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
    std::mutex camera_mutex_;  // 抓拍不能和别的抓拍同时拿帧
    bool camera_ready_ = false;  // 摄像头初始化成功（采集平时停着，抓拍时才开）
    std::atomic<bool> sleeping_{false};
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
        config.fb_count = 2;  // 双缓冲：拍的那帧 DMA 不会正在被覆写（单缓冲会拍到写一半的帧）
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;  // 总是拿到最新的画面
        camera_ = new Esp32Camera(config);
        // esp_camera_init 一结束就开始连续采集：VGA RGB565 每秒十几帧，驱动的采集任务（优先级 23）
        // 用 CPU 把每帧 600KB 拷进 PSRAM（板上不支持 PSRAM DMA）。一直这么跑，抢走大量 CPU 和 PSRAM 带宽：
        // 音频编码掉帧（Encode queue is full）、AFE 喂不进（Ringbuffer full）、界面滑动也跟着卡。
        // 只有大模型要看一眼时才需要画面，平时停着
        if (esp_camera_sensor_get() != nullptr) {
            cam_stop();
            camera_ready_ = true;
        }
    }

    // 拍一张 JPEG 给看图模型。摄像头输出 RGB565 是大端，编码器要小端，先换字节
    std::string CaptureJpeg() {
        std::lock_guard<std::mutex> lock(camera_mutex_);
        if (!camera_ready_) return "";
        // 平时采集是停着的：开起来，先丢掉几帧——队列里压着停之前的旧帧，停的那一刻收到一半的帧也作废。
        // 传感器一直在出图，曝光早就稳了，丢三帧就是当下的画面
        cam_start();
        camera_fb_t* fb = nullptr;
        for (int i = 0; i < 4; i++) {
            if (fb != nullptr) esp_camera_fb_return(fb);
            fb = esp_camera_fb_get();
            if (fb == nullptr) break;
        }
        if (fb == nullptr) {
            cam_stop();
            return "";
        }
        std::string out;
        if (fb->format == PIXFORMAT_RGB565) {
            size_t pixels = fb->width * fb->height;
            auto* buf = static_cast<uint16_t*>(heap_caps_malloc(pixels * 2, MALLOC_CAP_SPIRAM));
            if (buf != nullptr) {
                auto* src = reinterpret_cast<const uint16_t*>(fb->buf);
                for (size_t i = 0; i < pixels; i++) buf[i] = __builtin_bswap16(src[i]);
                uint8_t* jpeg = nullptr;
                size_t jpeg_len = 0;
                if (image_to_jpeg(reinterpret_cast<uint8_t*>(buf), pixels * 2, fb->width, fb->height,
                                  V4L2_PIX_FMT_RGB565, 80, &jpeg, &jpeg_len)) {
                    out.assign(reinterpret_cast<char*>(jpeg), jpeg_len);
                    free(jpeg);
                }
                heap_caps_free(buf);
            }
        }
        esp_camera_fb_return(fb);
        cam_stop();
        return out;
    }

    // BOOT 键说话：默认按住说话（像对讲机），设置里可改成点一下开始、说完自动结束
    bool HoldToTalk() { return NomiDisplay::GetSavedInt(NOMI_KEY_TALK_KEY, 0) == 0; }

    void InitializeButtons() {
        boot_button_.OnPressDown([this]() {
            power_save_timer_->WakeUp();
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
        // 时钟页：闹钟、倒计时提醒、天气城市
        auto& clock = NomiClock::GetInstance();
        clock.RegisterTools();
        clock.on_ring_start = [this](const std::string& time, const std::string& label) {
            NomiRadio::GetInstance().SetAlarmHold(true);  // 电台先停，别和铃声混在一起
            power_save_timer_->WakeUp();  // 睡着 / 息屏也要亮起来
            GetBacklight()->RestoreBrightness();
            display_->ShowAlarm(time, label);
        };
        clock.on_ring_stop = [this]() {
            display_->HideAlarm();
            NomiRadio::GetInstance().SetAlarmHold(false);
        };
        // 电台：放、停、换台、收藏；播客：放、订阅、快进快退；在线音乐：点歌
        NomiRadio::GetInstance().RegisterTools();
        NomiPodcast::GetInstance().RegisterTools();
        NomiMusic::GetInstance().RegisterTools();
        // TF 卡卸载后把 GPIO43 还给功放（R30 焊上的板子它是功放开关），电平按现在出不出声来
        NomiSdCard::GetInstance().on_bus_released = [this]() {
            gpio_set_direction(AUDIO_CODEC_PA_PIN, GPIO_MODE_OUTPUT);
            gpio_set_level(AUDIO_CODEC_PA_PIN, GetAudioCodec()->output_enabled() ? 1 : 0);
        };
#if CONFIG_DIRECT_API
        // 每轮对话的系统提示词里带上现在几点、天气、已设的闹钟、电台（设"明早 7 点"要知道今天是哪天）
        DirectApiProtocol::context_provider = []() {
            return NomiClock::GetInstance().PromptContext() + "\n" + NomiRadio::GetInstance().PromptContext() + "\n" +
                   NomiPodcast::GetInstance().PromptContext();
        };
        DirectApiConfigServer::on_city_changed = []() { NomiClock::GetInstance().RefreshWeather(); };
        // 网页设置页的「应用」卡片：上传 .npk 安装、列出、删除
        DirectApiConfigServer::AddUploadHandler(
            "/apps/install", [](size_t length, const std::function<int(char*, size_t)>& read) {
                return NomiPkg::GetInstance().InstallJson(length, read);
            });
        DirectApiConfigServer::AddJsonHandler("/apps.json", false,
                                              [](const std::string&) { return NomiPkg::GetInstance().ListJson(); });
        DirectApiConfigServer::AddJsonHandler("/apps/remove", true, [](const std::string& body) {
            return NomiPkg::GetInstance().RemoveJson(body);
        });
        DirectApiConfigServer::AddJsonHandler("/apps/open", true, [](const std::string& body) {
            return NomiPkg::GetInstance().OpenJson(body);
        });
        // 网页设置页的「SD 卡」卡片：每次读都当场检测
        DirectApiConfigServer::AddJsonHandler("/sd.json", false,
                                              [](const std::string&) { return NomiSdCard::GetInstance().StatusJson(); });
        // 网页设置页的「电台」卡片
        DirectApiConfigServer::AddJsonHandler("/radio.json", false,
                                              [](const std::string&) { return NomiRadio::GetInstance().ListJson(); });
        DirectApiConfigServer::AddJsonHandler("/radio/add", true, [](const std::string& body) {
            return NomiRadio::GetInstance().AddFromJson(body);
        });
        DirectApiConfigServer::AddJsonHandler("/radio/delete", true, [](const std::string& body) {
            return NomiRadio::GetInstance().DeleteFromJson(body);
        });
        DirectApiConfigServer::AddJsonHandler("/radio/move", true, [](const std::string& body) {
            return NomiRadio::GetInstance().MoveFromJson(body);
        });
        DirectApiConfigServer::AddJsonHandler("/radio/play", true, [](const std::string& body) {
            return NomiRadio::GetInstance().PlayFromJson(body);
        });
        // 「播客」卡片：搜索订阅、删除、放最新一期
        DirectApiConfigServer::AddJsonHandler("/podcast.json", false,
                                              [](const std::string&) { return NomiPodcast::GetInstance().ListJson(); });
        DirectApiConfigServer::AddJsonHandler("/podcast/search", true, [](const std::string& body) {
            return NomiPodcast::GetInstance().SearchJson(body);
        });
        DirectApiConfigServer::AddJsonHandler("/podcast/subscribe", true, [](const std::string& body) {
            return NomiPodcast::GetInstance().SubscribeJson(body);
        });
        DirectApiConfigServer::AddJsonHandler("/podcast/delete", true, [](const std::string& body) {
            return NomiPodcast::GetInstance().DeleteJson(body);
        });
        DirectApiConfigServer::AddJsonHandler("/podcast/play", true, [](const std::string& body) {
            return NomiPodcast::GetInstance().PlayJson(body);
        });
        // 「在线音乐」卡片：填音乐网站、试搜、试放
        DirectApiConfigServer::AddJsonHandler("/music.json", false,
                                              [](const std::string&) { return NomiMusic::GetInstance().InfoJson(); });
        DirectApiConfigServer::AddJsonHandler("/music/site", true, [](const std::string& body) {
            return NomiMusic::GetInstance().SetSiteJson(body);
        });
        DirectApiConfigServer::AddJsonHandler("/music/search", true, [](const std::string& body) {
            return NomiMusic::GetInstance().SearchJson(body);
        });
        DirectApiConfigServer::AddJsonHandler("/music/play", true, [](const std::string& body) {
            return NomiMusic::GetInstance().PlayJson(body);
        });
        DirectApiProtocol::face_prompt = NomiDisplay::FacePrompt();
        DirectApiProtocol::face_handler = [this](const std::string& json, std::string& error) {
            return display_->PlayFace(json, error);
        };
        // 让大模型看一眼摄像头：拍照和编码要一两秒，放到独立任务里做，别卡主循环；
        // 拍好的 JPEG 经 AttachImage 交给直连协议，由它随下一轮请求发给视觉模型（见 Chat()）
        mcp_server.AddTool("self.camera.look",
                           "看一眼摄像头拍到的画面并说出看到了什么。主人让你看东西、认东西、读画面上的文字、"
                           "描述眼前的场景时调用；question 写上你想从画面里知道的问题。",
                           PropertyList({Property("question", kPropertyTypeString)}),
                           [this](const PropertyList& properties) -> ToolResult {
                               (void)properties;
                               if (xTaskCreate(
                                       [](void* arg) {
                                           auto* self = static_cast<SpotpearMax35Board*>(arg);
                                           std::string jpeg = self->CaptureJpeg();
                                           if (!jpeg.empty()) {
                                               DirectApiProtocol::AttachImage(std::move(jpeg));
                                           }
                                           vTaskDelete(nullptr);
                                       },
                                       "cam_look", 8192, this, 3, nullptr) != pdPASS) {
                                   return std::unexpected("相机忙，稍后再试");
                               }
                               return std::string("已拍下画面，照片随后作为用户消息发给你");
                           });
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
        NomiClock::GetInstance().Start();  // 时区、闹钟；联网后自己对时、拉天气
        NomiRadio::GetInstance().Start();    // 台单、收流 / 解码任务（平时睡着）
        NomiPodcast::GetInstance().Start();  // 订阅、后台联网任务
        NomiMusic::GetInstance().Start();    // 音乐网站地址、下封面的后台任务
        NomiWasm::Init(display_);            // 应用包的运行时（docs/app-sdk.md）
        NomiPkg::GetInstance().Start();      // 扫 SD 卡上装好的应用包，登记到桌面
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
