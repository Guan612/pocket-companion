# 硬件：SpotPear ESP32-S3 3.5 寸 AI 板（ESP32S3-MAX35）

- 原理图：[ESP32S3-3.5inch-AI-1.pdf](https://cdn.static.spotpear.cn/uploads/picture/learn/ESP32/ESP32S3-MAX35-Only-Board/ESP32S3-3.5inch-AI-1.pdf)
- 资料页：[SpotPear Wiki](https://spotpear.cn/wiki/ESP32-S3-3.5-inch-LCD-Display-TouchScreen-GC0308-Deepseek.html)

## 规格

| 项目 | 内容 |
|---|---|
| 主控 | ESP32-S3R8（内置 8MB 八线 PSRAM），16MB Flash |
| 屏幕 | 3.5 寸 ST7796S，480×320，4 线 SPI |
| 触摸 | GT911，电容触摸 |
| 音频 | ES8311 放音，ES7210 录音（双麦 + 喇叭回采，可做回声消除；部分版本没焊，退回 ES8311 自带麦克风），NS4150B 功放 |
| 其他 | GC0308/OV2640 摄像头接口、SD 卡、4G 模块接口、锂电池充电 |

## 引脚

全部来自原理图，代码里的定义在 [`config.h`](../firmware/main/boards/spotpear/spotpear-esp32s3-max35/config.h)。

| 功能 | GPIO | 备注 |
|---|---|---|
| LCD SCK / MOSI / CS / DC | 46 / 48 / 5 / 47 | |
| LCD 复位 | 无 | 10K + 22µF RC 上电复位，只能软复位 |
| LCD 背光 | 42 | 经 APM2307（P 沟道 MOS）驱动，**低电平点亮** |
| I2S MCLK / BCLK / WS | 16 / 9 / 45 | |
| I2S DOUT（→ES8311）/ DIN（←ES8311） | 8 / 10 | |
| 功放使能 | 43 | 高电平打开。它也是 U0TXD，所以日志走 USB |
| I2C SCL / SDA | 14 / 15 | ES8311、ES7210（0x40）、GT911、摄像头 SCCB 共用 |
| 触摸 INT / RST | 无 | INT 没接到 GPIO，只能轮询；RST 是 RC 上电复位 |
| USB D- / D+ | 19 / 20 | 原生 USB，烧录和日志都走这里 |
| 电池电压 | 6（ADC1_CH5） | V_BAT 经 100K/100K 分压 |
| USB 插入检测 | 7 | USB 5V 经 20K/30K 分压，插着为高；充电芯片的 CHG 只接了指示灯 |
| 摄像头 D0–D7 | 12 / 4 / 11 / 17 / 18 / 3 / 1 / 2 | OV2640 的 Y2–Y9 |
| 摄像头 PCLK / VSYNC / HREF / XCLK | 38 / 39 / 40 / 41 | SCCB 复用 I2C 14/15，PWDN、RESET 板上固定 |
| BOOT 键 | 0 | |

注意：SpotPear 另一个仓库里的 `sp-esp32-s3-lcd-3.5` 小智配置（屏幕 CS=21、背光 5 等）是另一款板子，和这块对不上。

## 还要上机确认的

- 屏幕方向和颜色：`BOARD_LCD_SWAP_XY / MIRROR_X / MIRROR_Y / INVERT / BGR` 是按常见 ST7796S 模组给的初值，画面翻转或偏色时改这几个。
- GT911 的 I2C 地址：INT 悬空时上电，地址可能是 0x5D 或 0x14，接触摸时两个都要试。
