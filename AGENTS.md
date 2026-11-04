# AGENTS.md

给接手这个仓库的 AI 助手（和人）看的。只放最要紧的规矩和入口，细节在 docs 里，别在这里抄一遍。

## 项目

Pocket Companion：放在桌上的语音小伙伴。底座是小智 xiaozhi-esp32 v2.5.2（`firmware/`，git subtree），硬件是 SpotPear ESP32-S3 3.5 寸板（8MB PSRAM、480×320 屏、16MB 闪存），界面是自己画的 Nomi。设备**直连** OpenAI 兼容接口（识别、对话、合成），**不要任何服务端**——别提议加服务器、别接 xiaozhi.me。

现在的方向是**应用化**：固件只做系统能力，业务做成可安装的应用包（WebAssembly，`.npk`）。

## 先读什么

1. `docs/handoff.md`：现状、命令、架构、正在做的、已知问题、踩过的坑、约定。**接手先读它，做完一步记得更新它。**
2. `docs/app-sdk.md`：应用包规范、系统接口、实现进度（写应用、加系统接口时看）。
3. 按需：`docs/hardware.md`（引脚）、`docs/flash-partitions.md`（分区、哪些操作会清数据）、`docs/online-music.md`。
4. `firmware/AGENTS.md` 是小智上游的规矩（板子身份、状态机、主循环不能堵……），改 `firmware/main/` 里板子目录以外的代码时照它来。

## 代码在哪

| 位置 | 是什么 |
|---|---|
| `firmware/main/boards/spotpear/spotpear-esp32s3-max35/` | 本项目自己的固件代码几乎都在这：界面（`nomi_ui.c`、`nomi_app.c`）、应用运行时（`nomi_wasm.cc`、`nomi_pkg.cc`）、播放器（`nomi_radio.cc`、`nomi_mp4.cc`）、阅读器（`nomi_reader.cc`、`nomi_book.cc`、`nomi_font.cc`）、SD 卡（`nomi_sdcard.cc`）、板子初始化（`spotpear_esp32s3_max35.cc`） |
| 同上 `preinstall/` | 打进资源分区的东西：预装应用包、GBK 对照表（`gbk.tab`） |
| `firmware/main/protocols/direct_api_*` | 直连协议、网页设置页（`direct_api_config_page.h`，设备和模拟器共用） |
| `apps/<名字>/` | 应用包源码（`music`、`novel`），C 编成 WebAssembly |
| `tools/app-sdk/` | SDK 头文件、`build.py`（编译打包）、`wasm_info.py` |
| `tools/device-sim/` | 浏览器整机模拟器 |

`firmware/` 是 subtree：更新底座用 `git subtree pull --prefix=firmware https://github.com/78/xiaozhi-esp32.git <tag> --squash`。改小智自己的文件要克制，能放板子目录就放板子目录。

## 最重要的约定

- **系统只给能力，业务归应用。** 解码播放、联网、存储、排版、字体、控件、大模型接口在固件里；台单、队列、书架、书签、订阅这类业务在应用里。别把业务写回固件（用户明确定的方向，改过好几次）。
- **设计要通用**：任意 SD 卡、任意应用、任意书。用户卡里的歌和书只是测试数据。
- **交互照 Wear OS**：表情页是表盘，左右滑是卡片，往上推是桌面，下拉是设置；应用里上下走、左边缘右划返回，不放返回按钮；对话从底下升浮层，不翻页、不关应用。
- **只改用户点名的东西**。界面尺寸、布局先问清改哪；让改文案时别顺手改界面。
- **用户审美**：细线条、像蔚来 NOMI，嫌粗、嫌偷懒（拿现成小控件凑数被批评过）。

## 和用户打交道

- **一律用中文**：回复、汇报、表格、提交说明、文档注释。
- **提交**：没让提交就不提交；让提交时直接推 `main`（用户一个人的仓库，不开分支）。提交说明中文，写清原因和验证情况，末尾带 `Co-Authored-By`。
- **上机测试先打招呼**：刷机会重启、测播放会出声和切歌，用户经常同时在用这台机器。
- **汇报要老实**：编译通过不等于上机验证过；说清测了什么、没测什么、在哪测的（电脑上 / 真机日志 / 用户亲眼看）。

## 编译和测试

Windows + ESP-IDF v6.1（小智 2.5 不支持 5.x）。PowerShell 里先 `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`，在 `firmware/` 下：

```powershell
idf.py build                       # 平时
idf.py reconfigure build           # 板子目录里新加 / 删了源文件（源文件是 glob 的）
python scripts/build.py spotpear/spotpear-esp32s3-max35 --name spotpear-esp32s3-max35
                                   # 改了板子 config.json（sdkconfig 项、分区表）、或拉了带新组件的代码
idf.py -p COM11 flash              # 刷机（不动 nvs 和应用分区）
```

应用包（要 zig：`pip install ziglang`，系统 Python 里有，IDF 的 Python 里没有）：

```bash
python tools/app-sdk/build.py apps/novel        # 打到 apps/novel/build/
python tools/app-sdk/build.py apps/novel -o firmware/main/boards/spotpear/spotpear-esp32s3-max35/preinstall/nomi.novel.npk
                                                # 改了预装应用：先升 manifest 的版本号（不升机器里不会换），再重打、再编固件
```

改完之前：

- 固件 `idf.py build` 没有新警告；看一眼固件余量（应用分区剩 6% 左右，加东西前想想放不放得下，大表放资源分区）。
- 能在电脑上测的先测：`nomi_book.cc`、`nomi_mp4.cc` 不依赖设备，用 `python -m ziglang c++` 编个小测试跑真文件。网页设置页的脚本可以抠出来 `node --check`，交互用 Playwright 连真设备测。
- 加了系统接口：同时改 `nomi_wasm.cc`（实现和 `kNatives`）、`tools/app-sdk/nomi_sdk.h`、`docs/app-sdk.md`；不兼容的新接口要升接口版本（`nomi_pkg.cc` 的 `kApi`、`NOMI_API`、`build.py` 的 `API`）。
- 做完一步更新 `docs/handoff.md`（新的现状、上机验证清单、踩到的坑）。

上机（这台电脑）：串口 COM11（打开前 DTR、RTS 设 False，不然会复位）；设备网页 `http://192.168.1.40/`（会变，看串口日志 `Config page:`）；这台电脑有本机代理，curl 访问设备要加 `--noproxy '*'`。设备在弱 Wi-Fi 下（-80 dBm 左右），网络慢、偶尔断流不一定是代码的事。

## 最容易踩的坑（完整的在 handoff）

- **栈在 PSRAM 的任务不能碰内部闪存和 NVS**（应用进程、`nomi_jobs`、收流解码任务都是）：碰 `/int` 走 `NomiFlashIo::Run` / `ForPath`，写 NVS 排到主循环（`Application::Schedule`）。碰了就断言重启。
- **`int32_t` 在 ESP32 上是 `long`**：`std::max(int, int32_t)` 编不过，写 `std::max<int32_t>(…)`。
- **内部 RAM 很紧**（平时空闲约 50KB），TLS 同时只能一条；大缓冲一律放 PSRAM。PSRAM 也不宽裕（音乐、小说都开着时剩 1MB 左右）。
- **屏幕 SPI 别上 80MHz**（画面不跟着变，看起来像触摸坏了）。
- `firmware/.gitignore` 忽略所有 `*.bin`：放进仓库的资源文件别用 `.bin` 后缀。
- 用 `mv` 改名的资源文件修改时间不变，资源分区不会重打；`touch` 一下再编。
- 板子构造函数里别调 `Board::GetInstance()`（单例没建完，自己等自己）。
