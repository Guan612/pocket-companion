# Pocket Companion

一个放在桌上的语音小伙伴：说话唤醒或按键，它用一张方形 Nomi 风格的脸回应你。设备直接调用 OpenAI 兼容的 API，中间不需要任何服务器。

![六个表情状态](docs/face-states.png)

从左到右、从上到下依次是：待机、聆听、思考、说话、开心、疑惑。这张图是固件里的 `face.c` 在电脑上直接渲染出来的。

## 架构

```text
┌──────────────────────── 设备（ESP32-S3） ────────────────────────┐
│ 小智固件：唤醒词 / 回声消除 / VAD / 配网 / 屏幕 / 触摸            │
│                                                                  │
│ 录音 ──▶ DirectApiProtocol ──HTTPS──▶ /audio/transcriptions      │
│                │            ──HTTPS──▶ /chat/completions         │
│                │            ──HTTPS──▶ /audio/speech（Ogg Opus） │
│                ▼                                                 │
│ NomiDisplay（表情跟随设备状态和 LLM 情绪）  ◀── 播放              │
└──────────────────────────────────────────────────────────────────┘
```

- 固件底座是 [小智 xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) v2.5.2，以 git subtree 放在 `firmware/`。
- 我们加的代码：
  - 板子适配、表情和界面：`firmware/main/boards/spotpear/spotpear-esp32s3-max35/`（`nomi_ui.c` 和模拟器共用）
  - 直连 API 协议和网页设置页：`firmware/main/protocols/direct_api_*`
  - 启动流程里的开关：`firmware/main/application.cc` 中的 `CONFIG_DIRECT_API`
- 打开 `DIRECT_API`（这块板默认打开）后，设备不连小智服务器，不激活，也不会被官方 OTA 覆盖。

## 硬件

SpotPear ESP32-S3 3.5 寸 AI 板（ESP32-S3R8，8MB PSRAM，480×320 ST7796S，ES8311 + NS4150B，GT911 触摸）。引脚和注意事项见 [docs/hardware.md](docs/hardware.md)。

## 使用

1. 烧录固件后，按小智的流程配网：手机连设备热点，在网页里填 Wi-Fi。
2. 设备连上网后，屏幕会显示设置页地址。用同一局域网的浏览器打开 `http://设备IP/`。
3. 先添加供应商（选预置的一家或自定义，填 Key；模型列表由设备去供应商那里拉，勾选要用的），再给对话、带图提问、语音识别、语音合成各选一个"供应商 / 模型"。对话模型能看图的话，带图提问选"和对话模型相同"。已存过 Key 的供应商改了地址，必须同时重填 Key。
4. 说「你好小智」或按住 BOOT 键说话（设置里可改成点一下开始）。

屏幕操作：

- 左右滑切换页面：**Nomi 页**是表情和字幕；**历史对话页**列出聊过的内容（本次开机内），新消息来了自动更新
- 从顶部往下拉打开设置，按住底部小白条往上推收起
- 空闲后先以正常亮度睡着（闭眼、飘 Z），睡着 30 秒后调暗，到息屏时间关屏；点屏幕唤醒
- 字幕只显示正在念的两行，长句从末尾截取；完整内容看历史对话页

所有接口都是 OpenAI 兼容格式。语音合成必须支持 `response_format: "opus"`。图片按 OpenAI 的 `image_url`（base64 JPEG）发送，gpt-4o、qwen-vl、glm-4v、doubao-vision 等支持图片输入的模型都能用；网页设置页里可以给看图识图单独配一个模型，留空就用对话模型。只支持流式返回的模型（部分 omni 模型）暂时不行。设置页「系统」里的「测试 API」可以检查大模型和 opus 合成。

让它看东西：说「帮我看看」「这是什么」之类的话，大模型会调 `self.camera.look` 工具拍一张（设备端硬件 JPEG 编码，一两秒），照片和问题一起发给视觉模型，回答带着人设说出来。不是实时取景，模型不主动调就不会碰摄像头。

## 编译

需要 ESP-IDF v6.1（小智 2.5 已不支持 5.x）。

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd firmware
python scripts/build.py spotpear/spotpear-esp32s3-max35 --language zh-CN
idf.py -p COMx flash monitor
```

## 模拟器

### 整机模拟（设备 + 手机）

在浏览器里把整台设备跑一遍：左边是设备屏幕，跑的是设备上同一份界面代码（`nomi_ui.c` + `face.c`，LVGL 9.5，小智的中文字体），鼠标当触摸，底下是串口日志；右边是一部手机，打开设备的网页。

- 开机流程照固件走：没存 Wi-Fi 就进配网模式，屏幕提示连热点；手机自动弹出配网页（固件里小智 esp-wifi-connect 的同一份 HTML），选 Wi-Fi 填密码（8 位以上算对）
- 联网后启动直连协议，手机打开网页设置页（`direct_api_config_page.h` 的同一份 HTML），填大模型地址和模型后才能对话；没填就按 BOOT 会看到和设备一样的报错
- 右侧圆钮或空格键是 BOOT 键，默认按住说话；开机阶段点一下进配网
- 左右滑切换表情 / 历史对话页；顶部下拉打开设置（BOOT 键用法、随时插话打断、字幕、人设、音色、亮度、调暗 / 息屏时间、音量、Wi-Fi 和设置页二维码、测试 API、重新配网、恢复出厂）
- 空闲后闭眼调暗（按 Z 键立刻睡着），点屏幕唤醒
- 「断电重启」保留设备存储，「清除 Wi-Fi 重启」重走配网，「恢复出厂」全部清空；设备存储存在 `tools/device-sim/.sim-nvs.json`（已忽略，里面有你填的 Key），重启 `serve.py` 也还在
- 「网络环境」：路由器掉线（设备照固件重连 5 次，再按 10 秒起翻倍的间隔扫描；开机 60 秒连不上进配网）、外网不通、API 真实调用 / 模拟回答
- 真实 API：设备由这台电脑代为上网，照 `DirectApiProtocol` 的格式调设置页填的接口（语音识别 → 大模型含工具调用 → 逐句 opus 合成并播放），长期记忆、调音量 / 亮度工具都会生效；浏览器麦克风录音，没有麦克风就在输入框打字（跳过语音识别）

「表情脚本试播」框里点例子或粘一段 JSON，能直接看大模型写的表情会怎么演。模拟器没有摄像头，大模型调 `self.camera.look` 会按"没拍到"的分支回答；屏幕之外的设备行为（联网、对话流程）是照固件代码在模拟器里重写的，不是固件本身在跑。

```bash
# 先编译过一次固件（需要 managed_components 里的 LVGL、小智字体和配网页）
cd tools/device-sim
pip install ziglang
python build.py
python serve.py               # 打开 http://127.0.0.1:8801/
python serve.py --lan         # 可选：同一局域网的真手机也能扫屏幕上的二维码打开设置页
```

### 表情模拟器

只看表情动画，不依赖固件编译：

```bash
cd tools/face-sim
python build.py               # --frames 另外渲染六个状态的截图到 out/
python -m http.server 8800    # 打开 http://127.0.0.1:8800/sim.html
```

## 同步小智上游

```bash
git subtree pull --prefix=firmware https://github.com/78/xiaozhi-esp32.git <tag> --squash
```

可能冲突的地方只有我们在小智文件里加的几段：`firmware/main/CMakeLists.txt`、`Kconfig.projbuild`、`application.cc`，以及 `main/audio/audio_service.{h,cc}` 里给 mp3 / wav 合成用的 `PushPcmToPlaybackQueue`。

## 进度

- [x] 方形 Nomi 表情、板子适配、直连 API、网页设置页（ESP-IDF v6.1 编译通过）
- [x] 浏览器设备模拟器：和设备同一份界面代码，可点、可滑、可下拉设置
- [x] 电池电量显示、空闲闭眼调暗 / 息屏、唤醒词（你好小智）
- [x] 触摸设置页（模拟器里的设置页和设备是同一份代码）
- [x] 视觉识图：大模型按需调 `self.camera.look` 工具拍一张，照片随请求发给支持图片输入的模型（可单独配视觉模型）
- [x] 说话改为 BOOT 键（按住说话 / 点一下）+ 唤醒词；触摸负责切页、下拉设置
- [x] 随时插话打断：焊了 ES7210 的板子用设备端回声消除，说话时开口即打断
- [x] 大模型工具调用：直连 API 桥接设备端 MCP 工具（调音量 / 亮度、查设备状态、看摄像头），模型不支持时自动降级为普通聊天
- [x] 历史对话页：本次开机内聊过的内容左右滑可看，新消息自动更新
- [x] 长期记忆：模型用 `self.memory.remember` 工具记事实，存 NVS（上限 2KB），每轮拼进 system prompt；网页设置页可查看和手改
- [x] 表情脚本：大模型在回复开头写 `<face>JSON</face>`，或调 `self.face.play` 工具，让脸现场表演——眼型（含 NOMI 式空心"0"、螺旋、爱心、墨镜）、眉毛、汗滴、手势、整脸动作，加一个白色线条道具（内置图标或大模型自己画的线条）。格式见 `face_script.h`，模拟器里有试播框
- [x] 语音合成支持 mp3 / wav：先要 opus，服务不支持就自动改要 mp3，设备上用乐鑫音频库解码（真机播放待验证）
- [ ] 上机验证：屏幕方向和颜色、触摸坐标、音频
- [ ] SD 卡功能包：照片 / 录音 / 音乐落卡。设计约定：SD 总线和摄像头共用 GP2/GP4、和电池 ADC 共用 GP6、和 4G 座共用整条总线，所以同一时刻只挂一个——识图拍照先暂存 RAM，再挂 SD 落盘；SD 挂载期间跳过电量读取；TF 座没有卡检测脚，挂载失败当没插
- [ ] 本地工具：红外 / 继电器（板上没有红外电路，需要外接）

## 致谢

- 固件底座：[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（MIT）

许可证见 [LICENSE](LICENSE)。
