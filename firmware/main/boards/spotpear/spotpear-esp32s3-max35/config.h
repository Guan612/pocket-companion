// SpotPear ESP32-S3 3.5 寸 AI 板（ESP32S3-MAX35），引脚来自官方原理图 ESP32S3-3.5inch-AI-1.pdf。
// 注意：小智里 spotpear 目录下的其他 3.5 寸配置是另一款板子，引脚对不上。
#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>
#include <driver/spi_master.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_16
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_45
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_9
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_10  // ES8311 ASDOUT → ESP32
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_8   // ESP32 → ES8311 DSDIN

// NS4150B CTRL，高电平打开。它也是 U0TXD，所以日志走 USB（见 config.json）
#define AUDIO_CODEC_PA_PIN      GPIO_NUM_43
#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_15
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_14
#define AUDIO_CODEC_ES8311_ADDR ES8311_CODEC_DEFAULT_ADDR
// ES7210（"语音打断 IC"）：MIC1/MIC2 两个麦克风，MIC3 接喇叭回采；AD0/AD1 下拉，地址 0x40
#define AUDIO_CODEC_ES7210_ADDR ES7210_CODEC_DEFAULT_ADDR

#define BUILTIN_LED_GPIO        GPIO_NUM_NC
#define BOOT_BUTTON_GPIO        GPIO_NUM_0

// ST7796S，4 线 SPI。复位脚是 RC 上电复位，不接 GPIO
#define DISPLAY_SPI_HOST    SPI2_HOST
#define DISPLAY_CLK_PIN     GPIO_NUM_46
#define DISPLAY_MOSI_PIN    GPIO_NUM_48
#define DISPLAY_CS_PIN      GPIO_NUM_5
#define DISPLAY_DC_PIN      GPIO_NUM_47
#define DISPLAY_RST_PIN     GPIO_NUM_NC
#define DISPLAY_SPI_HZ      (40 * 1000 * 1000)

// 背光经 APM2307（P 沟道 MOS）驱动，低电平点亮，所以 PWM 要反相
#define DISPLAY_BACKLIGHT_PIN           GPIO_NUM_42
#define DISPLAY_BACKLIGHT_OUTPUT_INVERT true

// 方向和颜色是按常见 ST7796S 模组给的初值，上机后画面翻转或偏色再调
#define DISPLAY_WIDTH        480
#define DISPLAY_HEIGHT       320
#define DISPLAY_SWAP_XY      true
#define DISPLAY_MIRROR_X     true
#define DISPLAY_MIRROR_Y     true
#define DISPLAY_RGB_ORDER    LCD_RGB_ELEMENT_ORDER_BGR
#define DISPLAY_INVERT_COLOR true
#define DISPLAY_OFFSET_X     0
#define DISPLAY_OFFSET_Y     0

// GT911 与 ES8311、摄像头 SCCB 共用 I2C。INT 没接 GPIO，RST 是 RC 上电复位，只能轮询
#define TOUCH_INT_PIN   GPIO_NUM_NC
#define TOUCH_RST_PIN   GPIO_NUM_NC
// 触摸原始坐标是竖屏 320×480，下面三项把它转成横屏坐标，上机后按实际方向调
#define TOUCH_X_MAX     320
#define TOUCH_Y_MAX     480
#define TOUCH_SWAP_XY   true
#define TOUCH_MIRROR_X  false
#define TOUCH_MIRROR_Y  true

// 电池：V_BAT 经 100K/100K 分压到 GPIO6（ADC1_CH5）；USB 5V 经 20K/30K 分压到 GPIO7，
// 插着 USB 时为高。充电芯片的 CHG 只接了指示灯，所以"充电中"用"插着 USB"近似
#define BATTERY_ADC_UNIT        ADC_UNIT_1
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_5
#define BATTERY_UPPER_RESISTOR  100000
#define BATTERY_LOWER_RESISTOR  100000
#define BATTERY_USB_DETECT_PIN  GPIO_NUM_7

// 休眠：调暗时的亮度。多久后调暗 / 息屏在设置页里选（默认 1 分钟 / 5 分钟）
#define DIM_BRIGHTNESS          5

// 摄像头（OV2640 / GC0308，DVP 8 位）。SCCB 复用上面的 I2C 总线；PWDN、RESET 板上已固定
#define CAMERA_PIN_D0     GPIO_NUM_12
#define CAMERA_PIN_D1     GPIO_NUM_4
#define CAMERA_PIN_D2     GPIO_NUM_11
#define CAMERA_PIN_D3     GPIO_NUM_17
#define CAMERA_PIN_D4     GPIO_NUM_18
#define CAMERA_PIN_D5     GPIO_NUM_3
#define CAMERA_PIN_D6     GPIO_NUM_1
#define CAMERA_PIN_D7     GPIO_NUM_2
#define CAMERA_PIN_PCLK   GPIO_NUM_38
#define CAMERA_PIN_VSYNC  GPIO_NUM_39
#define CAMERA_PIN_HREF   GPIO_NUM_40
#define CAMERA_PIN_XCLK   GPIO_NUM_41
#define CAMERA_PIN_PWDN   GPIO_NUM_NC
#define CAMERA_PIN_RESET  GPIO_NUM_NC
#define CAMERA_XCLK_HZ    20000000

#endif  // _BOARD_CONFIG_H_
