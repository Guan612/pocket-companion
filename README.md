# Pocket Companion

放在桌上的语音小伙伴：喊一声「糯米糯米」或按住 BOOT 键说话，它用一张 Nomi 风格的脸回应你。设备直接调用 OpenAI 兼容的 API（语音识别、对话、语音合成、看图），中间不需要任何服务器。

![六个表情状态](docs/face-states.png)

从左到右、从上到下：待机、聆听、思考、说话、开心、疑惑（固件里的 `face.c` 在电脑上渲染的）。

## 能做什么

- **对话**：连续对话，长期记忆，大模型能用表情脚本让脸现场表演；说「退下吧」结束对话。
- **看东西**：说「这是什么」，它拍一张交给视觉模型。
- **时钟和提醒**：闹钟、倒计时提醒都靠说；天气、当前时间每轮都带给大模型。
- **应用**：像 APK 一样能装能删（WebAssembly 应用包）。预装音乐（本地、电台、播客、在线点歌）、小说、图片；可以从网页上传装到 SD 卡上的有地图（离线道路图 + 手机导航）、相机（取景器、拍照、「问它」）。
- **手机**：用开源的 Gadgetbridge 连上手机，地图应用就能收到导航和定位，不用另写手机 App。

## 硬件

SpotPear ESP32-S3 3.5 寸 AI 板（ESP32-S3R8、8MB PSRAM、480×320 屏、GT911 触摸、GC0308 摄像头、TF 卡座）。引脚见 [docs/hardware.md](docs/hardware.md)，闪存分区和哪些操作会清数据见 [docs/flash-partitions.md](docs/flash-partitions.md)。

## 上手

1. 烧好固件后按小智的流程配网：手机连设备的热点，在弹出的网页里选 Wi-Fi。
2. 连上网后屏幕会显示设置页地址，用同一局域网的浏览器打开 `http://设备IP/`。
3. 在「大模型」里添加供应商（预置几家，也能自定义），再给对话、看图、语音识别、语音合成各选一个模型。接口都是 OpenAI 兼容格式；语音合成要能出 opus 或 mp3，看图要支持 `image_url`（base64 JPEG）；只支持流式返回的模型暂时不行。设置页里的「测试 API」能先检查一遍。
4. 喊「糯米糯米」，或者按住 BOOT 键说话。

断网时设备照常能用，并在后台一直重试；断网满一分钟会开一个 `Nomi-XXXX` 热点，手机连上就能打开设置页。

屏幕操作照 Wear OS 的习惯：表情页是表盘，左右滑是卡片（历史对话、收听、时钟），往下拉是设置，往上推是桌面（应用），应用里从左边缘往右划返回。

## 开发

需要 ESP-IDF v6.1（小智 2.5 不支持 5.x）：

```powershell
. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1
cd firmware
python scripts/build.py spotpear/spotpear-esp32s3-max35 --language zh-CN
idf.py -p COMx flash monitor
```

- 固件底座是 [小智 xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) v2.5.2（git subtree 放在 `firmware/`）；我们的代码几乎都在 `firmware/main/nomi/` 和 `firmware/main/protocols/direct_api_*`。同步上游：`git subtree pull --prefix=firmware https://github.com/78/xiaozhi-esp32.git <tag> --squash`。
- 写应用：规范和系统接口见 [docs/app-sdk.md](docs/app-sdk.md)，`python tools/app-sdk/build.py apps/<名字>` 编译打包成 `.npk`，在网页设置页「设备 → 应用」上传安装。
- 离线地图：`python tools/map/make_map.py --name 城市 --bbox 南,西,北,东 --pbf 省份.osm.pbf`（说明在脚本开头）。
- 模拟器：`tools/device-sim`（整机：设备屏幕 + 手机打开设置页；先编译过一次固件，再 `python build.py && python serve.py`）、`tools/face-sim`（只看表情）。
- 接手开发先读 [docs/handoff.md](docs/handoff.md)：现状、架构、踩过的坑、没验证的地方。

## 致谢

- 固件底座：[78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)（MIT）
- 地图数据：© OpenStreetMap contributors（ODbL）

## 许可证

本仓库以 [GNU AGPL v3](LICENSE) 发布：可以自由使用、修改、再分发，但发布修改版（包括作为网络服务给别人用）时要以同样的协议公开源码。

`firmware/` 里来自小智上游的代码原本是 MIT 许可证，它的版权和许可声明保留在 [firmware/LICENSE](firmware/LICENSE)。
