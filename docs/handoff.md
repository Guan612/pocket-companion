# 交接文档（2026-11-04）

接手的人先看这份，再按需看 [app-sdk.md](app-sdk.md)（应用规范和实现进度）、[flash-partitions.md](flash-partitions.md)（分区）、[hardware.md](hardware.md)（引脚）、[online-music.md](online-music.md)（音乐网站格式）。

## 现状

Pocket Companion 是一个桌面语音伙伴：底座是小智 xiaozhi-esp32 v2.5.2（`firmware/`，git subtree），硬件是 SpotPear ESP32-S3 3.5 寸板，界面是自己画的 Nomi，设备直连 OpenAI 兼容接口，不要任何服务端。

最近在做的是**应用化**：固件只做系统能力（解码播放、联网、存储、界面控件、大模型），业务做成可安装的应用包（WebAssembly，`.npk`）。音乐（本地、电台、播客、在线点歌）已经搬进了音乐应用，并且预装在机器里，不插 SD 卡也能用。2026-11-04 又加了蓝牙连手机（Gadgetbridge）和地图应用，见「手机连接、地图」。

迁移分五步，进度：

| 步骤 | 内容 | 状态 |
|---|---|---|
| ① 平台 | 每个应用一个进程、后台常驻、首页卡片、语音指令交给应用、联网接口、播放会话（放完 / 上一首下一首通知所属应用） | ✓ `1b9cdde` |
| ② 音乐应用 v1 | 本地 + 电台，收听卡片换成应用的 | ✓ `1b9cdde` |
| ③ 播客、在线 | 搬进音乐应用；应用自带网页设置页 | ✓ `0bf21ee` |
| 预装 | 机器内部的应用分区（2MB FAT）、预装应用、从 SD 卡搬数据 | ✓ `3edbf6e` |
| ④ 删固件里的旧业务 | 删固件的电台台单 / 播客 / 在线，老 NVS 数据不迁移（用 `tools/erase_legacy_nvs.py` 清一次） | ✓（应用 0.4.0：台单进 kv 可在网页增删排序、radio-browser 搜台、电台语音） |

## 仓库地图

| 位置 | 是什么 |
|---|---|
| `firmware/main/nomi/` | 自研固件代码，按能力分文件夹（下面几行都在这里，括号里是文件夹）。每项能力一个开关（`nomi/Kconfig`），见「能力开关」 |
| `nomi_system.cc/.h`（core） | Nomi 怎么起来：板子最后调 `NomiSystem::Start`，这里按开着的能力启动各模块、登记大模型工具和网页接口 |
| `nomi_wasm.cc/.h`（apps） | 应用运行时：进程、所有系统接口（`ui_*` `media_*` `net_*` `kv_*` `voice_*` `web_*`）、语音 / 网页请求转给应用 |
| `nomi_pkg.cc/.h`（apps） | 包管理：安装（tar 流）、扫描、预装、删除、应用网页 `/app/<id>/`、内部分区挂载 |
| `nomi_flash_io.cc/.h`（core） | 替栈在 PSRAM 的任务读写内部闪存（见「踩过的坑」） |
| `nomi_radio.cc/.h`（player） | 系统播放器：收流、本地文件（FLAC / WAV / MP3）、m4a、解码、对话时让路；放什么归应用（`Station.owner`），大模型工具只留 `self.radio.stop` |
| `nomi_music.cc` / `nomi_music_local.cc`（player） | 「正在放」的封面、歌词；本地文件读标签。放哪首、音乐网站都归应用 |
| `nomi_ui.c/.h`（core） | 主界面（表情、卡片、设置、对话浮层）；`ui_now_playing` 组件（系统收听页和应用卡片共用的「正在放」）；系统收听页只剩「正在放」（没装音乐应用时给提示） |
| `nomi_app.c/.h`（core） | 桌面和应用层（打开、右划返回、应用管理） |
| `nomi_display.cc`（core） | 界面和系统的胶水：界面要的文字、图片、按钮动作 |
| `boards/spotpear/spotpear-esp32s3-max35/` | 板子：引脚（`config.h`）、硬件初始化（屏幕、触摸、声音、电池、背光、按键、休眠，有的话摄像头、SD 卡引脚），最后 `NomiSystem::Start` |
| `nomi_phone.cc/.h`（phone） | 手机连接：蓝牙（NimBLE）装成 Bangle.js 手表，收发 Gadgetbridge 的消息、对时 |
| `nomi_canvas.cc/.h`（apps） | 画布控件：应用给一份线、多边形、圆、字的清单，屏幕刷新时照着画 |
| `nomi/preinstall/nomi.music.npk` | 预装的音乐应用包（**手动打的，改了应用要重打**） |
| `firmware/main/protocols/direct_api_config_*` | 网页设置页（页面 HTML 在 `direct_api_config_page.h`） |
| `firmware/partitions/v2/16m_apps.csv` | 这块板子的分区表 |
| `apps/music/` | 音乐应用源码：`main.c`（界面、分发、本地、电台）`podcast.c` `online.c` `assets/`（台单、网页） |
| `apps/map/` | 地图应用（不预装，网页上传装在 SD 卡上）：道路图、导航转向、手机定位 |
| `apps/camera/` | 相机应用（不预装，网页上传装在 SD 卡上）：取景器、拍照存卡、看照片、「问它」 |
| `tools/map/make_map.py` | 用 OpenStreetMap 做离线道路包（`.nmap`），`--upload 设备IP` 直接传到卡上 `maps/` |
| `tools/app-sdk/` | SDK：`nomi_sdk.h`（接口）`nomi_text.h`（文字工具）`build.py`（编译打包）`examples/counter` |
| `tools/device-sim/` | 浏览器整机模拟器（应用平台还没接进去） |

## 环境和常用命令

Windows，ESP-IDF v6.1。每个 PowerShell 先 `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`。

```powershell
cd firmware
idf.py build                      # 平时
idf.py reconfigure build          # nomi/ 或板子目录里新加了 .cc（源文件是 glob 的，配置时才扫）
python scripts/build.py spotpear/spotpear-esp32s3-max35 --name spotpear-esp32s3-max35
                                  # 改了板子 config.json（分区表、sdkconfig 项）：重新生成 sdkconfig，全量编译
idf.py reconfigure -DCMAKE_NINJA_FORCE_RESPONSE_FILE=1 build
                                  # build.py 重建了编译缓存，编到最后报 CreateProcess failed 时接着跑这个（见「踩过的坑」）
idf.py -p COM11 flash             # 刷机（会写程序、资源、分区表；不写 nvs 和应用分区）
```

应用：

```bash
python tools/app-sdk/build.py apps/music        # 打到 apps/music/build/，网页「设备 → 应用」上传安装
python tools/app-sdk/build.py apps/music -o firmware/main/nomi/preinstall/nomi.music.npk
                                                # 改了音乐应用：重打预装包，再编固件（版本号要升，机器里才会换）
python tools/app-sdk/wasm_info.py apps/music/build/app.wasm   # 看导入导出（查有没有意外的 memset 之类）
```

`build.py` 要 zig（`pip install ziglang`，系统 Python 里有）。IDF 的 Python 环境里没有，所以固件编译时不自动打应用包。

上机测试（这台电脑的环境）：
- 串口 COM11。用 IDF venv 的 Python（有 pyserial）：`C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`。打开串口前先把 DTR、RTS 设成 False，不然会复位芯片；要从开机收日志就主动拉一下 RTS。
- 设备在局域网 192.168.1.40（会变，看屏幕下拉设置或串口日志 `Config page:`）。网页设置页就是这个地址。
- 常用接口：`POST /apps/install`（请求体是 .npk）、`GET /apps.json`、`POST /app/<id>/call`（应用自己的网页请求，比如 `{"cmd":"pod_play","feed":"…"}`、`{"cmd":"station_list"}`）、`GET /app/<id>/`。固件的 `/radio/*` `/podcast/*` `/music.json` 第 ④ 步删掉了。
- 清老固件的 NVS 数据（只做一次）：IDF 环境里 `python tools/erase_legacy_nvs.py --port COM11`（先加 `--dry-run` 看会删什么；会自动留备份，恢复 `esptool write-flash 0x9000 备份.bin`）。
- 测 CPU：板子上临时加 `SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(2000))`（`system_info.h`），测完删。
- **用户经常同时在用这台机器。上机测试会出声、切歌、重启的，先跟用户说一声。**

## 同步小智上游、唤醒词（2026-11-05 从 README 挪来）

- 同步上游：`git subtree pull --prefix=firmware https://github.com/78/xiaozhi-esp32.git <tag> --squash`。可能冲突的只有我们在小智文件里加的几段：`firmware/main/CMakeLists.txt`、`Kconfig.projbuild`、`application.cc`（`CONFIG_DIRECT_API` 开关），以及 `main/audio/audio_service.{h,cc}` 里给 mp3 / wav 合成和播放器用的 `PushPcmToPlaybackQueue`。
- 唤醒词「糯米糯米」：乐鑫没有现成的 WakeNet 模型，用小智的自定义唤醒词——MultiNet7 中文命令词模型认拼音 `nuo mi nuo mi`，门槛 0.12（板子 `config.json` 里配，资源分区带 mn7_cn；改了要 `scripts/build.py` 重新生成配置、连资源分区一起刷）。MultiNet 每 32ms 要算 11ms，所以只在 VAD 判定有人说话时才喂：安静时存着最近 0.4 秒，开口时补进去，说完再多喂 0.6 秒。AFE 任务固定在核 0；建模型时的小块内存临时放 PSRAM。试过「嗨诺米」，置信度大多 0.2 上下，不如四个字稳。

## 架构速览

### 应用平台

- **进程**：每个应用一个 pthread（栈 32KB 在 PSRAM，WAMR 要求在 pthread 里跑），自己的模块实例和事件队列。界面挂上 / 拿下（`NOMI_EV_SHOW` / `HIDE`）。留在后台的理由：manifest 里 `"tile": true`，或者它放的东西还在放；没理由的在后台闲 30 秒结束。有事找没在跑的应用（放完、点了下一首、语音、网页）先在后台拉起来。最多 4 个。
- **界面**：`ui_*` 拿显示锁直接建 LVGL 控件；界面拿下后调用返回 `NOMI_E_NO_UI`。进程对象 `shared_ptr`，控件回调拿 `weak_ptr`。
- **语音**：manifest 的 `voice` 声明指令，系统提示里每轮列出（`NomiPkg::PromptContext`），大模型统一调 `self.app.command(app, command, arg)`，在主循环里同步等应用回话（最多 25 秒）。
- **网页**：应用包里 `assets/web/`，网页设置页「设备 → 应用 → 设置」打开 `/app/<id>/`，网页 POST `call` 交给应用（`NOMI_EV_WEB`，最多等 60 秒）。网页服务器的前缀接口是 `AddPrefixHandler`（通配匹配）。
- **存储**：两处装应用——机器内部 `/int/nomi/apps/<id>/`（应用分区 2MB FAT）、SD 卡 `/sd/nomi/apps/<id>/`。每个应用目录里 `manifest.json` `app.wasm` `assets/` `data/`（`kv.json`）。预装包在资源分区，开机比版本装进 `/int`。

### 播放

- `NomiRadio` 是唯一的播放器。`Station.owner` 记着是哪个应用放的；空的是固件自己的电台 / 播客 / 在线（第 ④ 步后就没有了）。
- 放完 → `on_finished` → 发给 owner 应用（`NOMI_MEDIA_FINISHED`）；收听卡片、`ui_player` 上的上一首 / 下一首 → `NomiWasm::MediaCommand` → 应用。应用放的节目，进度由应用记，系统不记。
- 网上的歌、节目放之前先探：https 先试 http，按文件头认格式（`NomiRadio::SniffFormat`）。
- 本地 FLAC / WAV：给解码器只喂拼出来的最小文件头（FLAC 只留 STREAMINFO），内嵌大封面的 FLAC 跳转才不出错；连续 128KB 解不出来就停，别让解码库刷屏。
- 解码、收流任务优先级 1，低于界面（2）。

### 界面

- 首页左右翻：历史、表情、收听卡片、时钟；往上推是桌面（应用网格）；下拉是设置；任何地方叫它说话从底下升起对话浮层，不翻页。
- 收听卡片：装了带 `tile` 的应用就盖上一层（`nomi_ui_listen_slot`），系统收听页整层藏起来。音乐应用在卡片里用 `ui_now_playing`（和系统收听页同一个组件：左边封面、控制、进度，右边 7 行歌词），顶上一排放「本地 / 电台 / 播客 / 在线」入口。

## 能力开关（2026-11-05，编译通过，未上机）

Nomi 从板子目录挪到了 `firmware/main/nomi/`，按能力分文件夹，每项能力一个 Kconfig 开关（`nomi/Kconfig`，在小智的 `Kconfig.projbuild` 里 `rsource`）。`main` 组件是 `WHOLE_ARCHIVE` 链接的，编了就进固件，所以关掉的能力在 `nomi/nomi.cmake` 里整个文件夹不编。为的是以后换板子（小屏、没 SD 卡、没摄像头的）时能按需裁。

| 开关 | 文件夹 | 依赖 | 关掉后 |
|---|---|---|---|
| `CONFIG_NOMI` | `core/` | PSRAM | 不用 Nomi（别的小智板子）。打开就有表情、对话记录、时钟闹钟天气、设置、对话浮层 |
| `CONFIG_NOMI_SDCARD` | `sdcard/` | | 没有 SD 卡、网页文件管理、系统应用「文件」；看书补字的 `fonts/*.ttf` 不读 |
| `CONFIG_NOMI_APPS` | `apps/` | SDCARD、S3 | 没有应用包、应用管理、打开方式；首页没有卡片位；桌面只剩「文件」（SD 也关了就没有桌面） |
| `CONFIG_NOMI_PLAYER` | `player/` | APPS | 卡片位里没有系统的「正在放」（只等应用插卡片）、表情页没有「正在放」小条；`media` 权限的应用装不上；不预装音乐 |
| `CONFIG_NOMI_READER` | `reader/` | APPS | 没有 `ui_reader` 等接口；不带 GBK 表、不预装小说 |
| `CONFIG_NOMI_PHOTO` | `photo/` | APPS | 没有看图接口、电子相框（设置里「睡着时显示」没有「相册」）、网页预览；不预装图片 |
| `CONFIG_NOMI_CAMERA` | `camera/` | S3 | 板子不初始化摄像头，没有 `self.camera.look`；`camera` 权限的应用装不上 |
| `CONFIG_NOMI_PHONE` | `phone/` | NimBLE | 不连 Gadgetbridge；`phone` 权限的应用装不上 |
| `CONFIG_NOMI_HOTSPOT` | `hotspot/` | | 断网只剩小智的配网热点 |

- **板子和 Nomi 的分工**：板子文件只管引脚和硬件（屏幕、触摸、声音、电池、背光、按键、休眠），建 `NomiDisplay`，有 SD 卡就 `NomiSdCard::Configure` 给引脚，有摄像头就初始化后 `NomiCamera::Init`，最后 `NomiSystem::Start(display, {wake_screen})`；`StartNetwork` 之后调 `NomiSystem::OnNetworkStarted`；调暗前问 `NomiSystem::KeepBright`。启动顺序、大模型工具、网页接口都在 `core/nomi_system.cc`。
- **跨能力的引用**一律 `#if CONFIG_NOMI_*` 包着；表达式里要 0 / 1 用 `core/nomi_caps.h` 的 `NOMI_HAS_*`（关掉的 `CONFIG_` 宏根本不存在）。头文件目录全加，只是不编源文件。
- **界面**（`nomi_ui.c`、`nomi_app.c` 和模拟器共用，不能读 sdkconfig）：用 `get_int` 问 `has_tiles`、`has_player`、`has_launcher`、`has_apps`、`has_frame`，默认值都是 1，模拟器不认这几个键就照旧全有。卡片位跟应用平台（卡片是应用插的），系统的「正在放」兜底才跟播放器。现在只有一个卡片位，谁 `tile` 谁占。
- **应用包**：安装时（网页上传、预装都走 `Install`）权限对应的能力没有就拒绝（`media` 播放器、`phone`、`camera`）；打开时 `MissingImports` 查导入的系统接口有没有接上，缺了就提示「这台设备没有…要用的功能」，不等调到才出错。开机扫描已装的不拦。
- **预装**：`nomi.cmake` 按开关把 `nomi/preinstall/` 里要的文件拷进 `build/nomi_preinstall/` 再打进资源分区，`nomi_pkg.cc` 的 `kPreinstalled` 按同样的开关列。
- **验证几种组合**：`python tools/nomi_variants.py --build`（IDF 环境里，在仓库根目录跑）。从全开编过的 `firmware/sdkconfig` 改出三份放在 `firmware/build/v*`（git 不管）：`min` 全关、`apps` 只有 SD 卡和应用平台、`sd` 有 SD 卡摄像头蓝牙热点但没有应用平台，逐个编。改了跨能力的代码后跑一遍。2026-11-05 程序大小：全开 4,210,240 字节；`sd` 3,957,232；`apps` 3,779,040；`min` 3,557,600（都还开着这块板子的蓝牙、摄像头 sdkconfig，换板子时连同 config.json 一起关能再小）。
- **还没做**：屏幕尺寸（`FACE_W/H`、`nomi_ui.c` 里几十处像素按 480×320 写）、输入方式（全靠触摸手势）没抽象；卡片位只有一个。

## 手机连接、地图（2026-11-04）

用户要的：板子上看「该怎么走」——转向、距离、速度、朝向，还有周围道路的大概样子（参考过 moto-gps-waveshare 的道路拓扑画法，只借了这个思路，没用它的代码）。定下来的做法：**不写手机 App**，手机装开源的 Gadgetbridge + OsmAnd / CoMaps 导航，板子用蓝牙装成 Bangle.js 手表收它的消息；道路图用 OpenStreetMap 做成离线包放 SD 卡。导航数据只从 Gadgetbridge 来，地图数据只在卡上，地图应用不预装（用户定的）。

- **手机连接**（系统，`nomi_phone.cc`）：NimBLE 外设，Nordic UART 服务，名字「Bangle.js Nomi」（Gadgetbridge 按服务 UUID + 名字开头认 Bangle.js）。手机发来一行一条 `\x10GB({...})`，系统按 `"t"` 交给 `phone_listen` 过这一类的应用（`nomi_wasm.cc` 的 `OnPhoneMessage`），自己只认连上时那行 `setTime(秒)` 对时。发回去的行尾要 `\r\n`。不加密：Gadgetbridge 加设备时选「不配对」。
- **内存**：蓝牙开起来要约 28KB 内部 RAM，原来平时只空 40～50KB、开机最低到过 4.8KB，直接开就起不来。做了这几样（板子 `config.json`）：
  - `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y`：控制器代码放闪存，省约 20KB IRAM（代价是程序变大，见下面分区）。
  - `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`：普通 `malloc` 不论大小都先给 PSRAM（原来小块的给内部）。
  - `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`：Wi-Fi、lwIP 的缓冲放 PSRAM。
  - `CONFIG_CAMERA_PSRAM_DMA=y`：摄像头 DMA 直接写 PSRAM，省掉内部 30KB 的 DMA 缓冲。
  - NimBLE 的内存放 PSRAM，只开外设角色、一个连接。
  - 结果：平时内部空闲 59.7KB、开机最低 9.6KB。蓝牙要开机就开（`NomiPhone::Start` 在 `NomiPkg` 之后）：试过推迟开，那时内部 RAM 已经碎了，控制器要的那块连续内存申请不到。
- **分区**：程序 4.01MB 超了 3.94MB 的程序槽，资源分区 6MB → 5MB、两个程序槽各 4.44MB（还能 OTA），见 [flash-partitions.md](flash-partitions.md)。现在剩 10%。
- **拍照**：内存改完后拍照失败了（`image_to_jpeg` 要好几块 600KB 的临时缓冲，PSRAM 拿不出）。改成在摄像头这一帧上原地把 RGB565 换算成 YUYV，直接交给 esp_new_jpeg 编码，额外只要放结果的 256KB。
- **拍照花屏 / 重启（2026-11-05 修）**：用户反馈拍照有时重启、有时模型说画面是花的。串口抓到重启前一行是 `Sending photo (136682 bytes JPEG)` 接 `abort()`（正常一张二十几 KB）。连拍 10 张全花：画面断成几段、有段满屏彩色噪点。根因：`CAMERA_PSRAM_DMA` 下界面刷新（LVGL 画进 PSRAM、屏幕 DMA 从 PSRAM 读）和摄像头 DMA 抢 PSRAM，摄像头接口的小 FIFO 整段丢数据（几十上百行），驱动这个模式不数字节、照样当整帧交出来；JPEG 涨到 100KB+，拼请求时 cJSON 打印要一块两倍大的连续内存，推测是要不到返回空指针、拿它构造 string 就 abort（崩溃地址对不上本机 ELF，没能逐行确认）。改法（`spotpear_esp32s3_max35.cc` 的 `CaptureJpeg`）：
  - 收帧的那一会儿关上「刷屏闸门」（`NomiCamera::GatePanel` 包了面板的 `draw_bitmap`）：要刷屏的先等着（最多 1 秒），LVGL 照常算、照常读触摸。最早的版本是拿显示锁停住整个界面，验证干净后为了取景器（要一直收帧）改成只挡刷屏，取景器整屏刷新时连拍照样干净。
  - **别把摄像头 GDMA 通道优先级（`rx_pri`）提到最高**：也能拍干净，但表情在动时屏幕那条 DMA 被饿住，一次刷屏再也完不成，LVGL 卡死在 `wait_for_flushing`、屏幕留一块蓝的（上机踩过，用户看到了）。改 32 字节 PSRAM 块（`in_ext_mem_bk_size`）也能防丢，但帧缓冲只按 16 字节对齐，没用。
  - 帧缓冲 2 块 → 1 块（PSRAM 平时多空约 600KB，对话前 psram free 从 ~1.0MB 到 1.68MB）；每帧还给驱动前在帧尾写 16 字节记号并把缓存写回，拿到的帧记号还在就是收短了，丢掉重拍；开头第一帧总是丢。
  - 顶上 2～3 行偶发颜色错（品红/绿，暂停刷新时也有，根因没查到）：丢掉顶上 16 行，出 640×464。
  - JPEG 超 96KB 降到质量 50 重编；`Chat()` 里 data URL 一次分配、`cJSON_PrintBuffered` 按估计大小一次要够，要不到就提示「内存不够，照片没发出去」而不是重启。
  - 验证：开机表情在动时连拍 35 张全干净、无看门狗；用户语音「拍照帮我看看这是什么」走通（40KB 照片、视觉模型正常回答）。没测：放歌时拍照。画面转了 90°（原来就这样，`jpeg_enc` 的 `rotate` 能转，用户没要求没动）。
  - 拍照的代码 2026-11-05 从板子类挪进 `nomi_camera.cc`（拍照、刷屏闸门、取景器），见下面「相机应用」。
- **接口 api 4**：`phone_*`、画布 `ui_canvas` `canvas_*`（`nomi_canvas.cc`）、`screen_keep_on`（前台应用要屏幕常亮：停掉休眠计时、屏幕关着的话叫亮；退到后台、退出自动放开）。见 [app-sdk.md](app-sdk.md)。
- **地图应用**（`apps/map` 0.1.2）：
  - 道路：卡上 `maps/*.nmap`，按定位读周围 3×3 块（约 3 公里见方）；车头朝上，自己在下方、地图跟着朝向转；按出行方式（步行 / 骑车 / 开车）画能走的路——**能走的亮、不能走的压暗到 55% 垫底**（两遍绘制：先全部暗线再亮线），缩放跟着变（步行看近、开车看远）；右上角指北。改暗线方案的原因：成都的路 OSM 里 95% 以上三种方式都能走，旧版「只画能走的」切模式时几乎只剩缩放在变，看不出线的变化（0.1.0 的小路颜色 0x3b404c 太暗，出现了也看不见）；暗线亮暗是「能不能走」的信号，小路亮色提到 0x474d5a（仍压在支路之下）。
  - 没有轨迹线（0.1.2 删了）：走过轨迹是 240 点环形缓冲叠在图上，用户说导航看的是「还没走的路」，走过的不该显示还白占内存——0.1.2 整个删掉（不存、不画，省 ~1.9KB 和每帧 240 点的画线），朝向计算改成只记上一个定位点（≥3 米才动基准点，滤原地飘动）。**「还没走的路线」Gadgetbridge 也不给**：`t:"nav"` 消息只有 action / instr / distance / eta，没有路线坐标（espruino.com/Gadgetbridge 文档确认），所以地图上没有路线高亮，该往哪走看底栏；想要真画路线得动手机端（违反「不写手机 App」的约定），真轨迹记录（存卡/导出）同理是没做的新功能。
  - 导航：底栏大箭头（17 种 action 都画了）、「X 米后左转」、路名、到达时间、速度。Gadgetbridge 不给路线形状，要走的那条路画不出高亮。
  - 定位：界面挂着时发 `gps_power` 让手机推、拿下时停，30 秒没来再要一次；导航中或定位一直在来时屏幕常亮。
  - 点屏幕出菜单换出行方式（存 kv `mode`），菜单里有 OSM 的署名。
- **地图包**（`tools/map/make_map.py`）：从 Overpass 取 OSM 道路（几个镜像轮着试、失败重试），按 0.01° 切块、裁到块边、抽稀，格式见脚本开头。每条路记着谁能走（按 OSM 默认，再看 foot / bicycle / motor_vehicle 这些标签）。成都 30.61～30.69, 104.03～104.12：6468 条路、134KB，已传到设备卡上 `maps/成都.nmap`。坐标 WGS-84，和手机 GPS 一致。2026-11-05 用户嫌范围太小，换成大范围：30.38～30.95, 103.70～104.40（到二绕附近，约 60×65 公里），74225 条路、58×71 块、2.4MB，已传到卡上 `maps/成都.nmap`（盖掉旧的，下载回来 md5 对过）；最密的 3×3 块合计 33.8KB（应用缓冲 96KB）、单条路最多 113 点（上限 1024），设备端不用改。**做法改成 `--pbf` 读本地 OSM 整包**：省级包在 download.openstreetmap.fr/extracts/asia/china/（四川 112MB，一分钟下完），`pip install osmium --index-url https://pypi.org/simple`（清华镜像没有 3.14 的包），整包过一遍二十秒；同范围和 Overpass 版对过（中心 6482 vs 6468 条路）。公共 Overpass 每次请求光排队几十秒、时常 502/超时，只适合小范围（脚本里留着，加了按片缓存、`--chunk`）。输出别放仓库根目录的 `build/`（没被 git 忽略）。
- **上机验证过**：Gadgetbridge 连上、对时；`gps_power` 手机那边收到了（logcat 里定位监听起来了）；连着蓝牙时对话（HTTPS）、开机取天气、放本地 FLAC、拍照都正常；用假消息喂过 17 种转向箭头、一段走路定位、导航底栏，截图看过道路图（车头朝上、按方式过滤）；常亮有日志 `Keep screen on: yes/no`。
- **没验证**：真的导航消息（OsmAnd / CoMaps → Gadgetbridge → 板子），尤其中文路名会不会乱码（Gadgetbridge 给 Bangle.js 发字可能会转码）、`distance` 的真实格式；真的 GPS（测的时候手机在室内没定位）；0.1.1 的暗线效果只在电脑上按同一算法渲过图，上机还得看（2026-11-05 装的，用户在真机切换过 0.1.0 的出行方式，过滤工作正常只是看不出差别）；0.1.2 删轨迹只编译过（wasm 23.4KB），没上机；连着蓝牙放在线的歌；常亮时屏幕已经关了能不能叫亮（提交前才补的，只编译过）。
- **调试**：手机 USB 连着这台电脑时，`adb logcat` 能看 Gadgetbridge 收发（别 `adb uninstall`、`pm clear`，会清掉用户的数据）。调试时临时加过两个网页接口（假装手机发一行、截屏成 BMP），提交前删了，要用再临时加。

## 相机应用、接口 api 5（2026-11-05）

用户要的：相机做成应用装在 SD 卡上（不预装）。定的分工：语音「看看这是什么」仍走系统（`self.camera.look`，不依赖卡和应用）；应用只做界面，系统给能力。

- **系统**（`nomi_camera.cc/.h`）：
  - `Capture()`：拍一张 640×464 的 JPEG（传感器原样，横的；顶上 16 行丢掉）。语音看图、应用拍照、应用问它都走它。
  - 取景器 `CreateViewfinder`：画面铺满整屏 480×320（照片比屏幕略方，上下各裁约 19 行），按钮那一列浮在右边。预览任务（栈在 PSRAM）一直开着摄像头：关闸门收一帧 → 缩到画布（最近邻）→ 隔 80ms 再来，每秒大约三四帧（没实测）。取景器被压在下面（开了别的页）时摄像头关掉；控件删了预览就停。只支持同时一个取景器。
  - 方向：先按房间照片猜着转成竖的（逆时针 90°），用户看了要「转回去、最大利用」，现在取景器和照片都是传感器原样。要转的话 JPEG 编码器能顺手转（`cfg.rotate`，宽高是 16 的倍数时），取景器取样要跟着改。
  - 「问它」：板子设 `NomiCamera::ask_handler`——照片交给 `DirectApiProtocol::AttachImage(jpeg, ask=true, question)`，主循环里待机就开始对话、在说话就打断转去听、已经在听就跟着这一轮发。`RunTurn` 里这一轮主人没开口、又挂着「问它」的照片，就不做语音识别，直接拿应用给的问题（空 = 「你看到了什么」）问。
- **接口 api 5**（权限 `camera`）：`ui_camera()`（取景器，返回右边那一列的 id，`ui_target` 它再建按钮）、`camera_shot(path)`（拍一张存 JPEG，`sd:/` 要 `sd.write`）、`camera_ask(question)`（还要 `assistant`）；另有 `open_file(path)`（要 `sd.read`）：走系统打开方式交给别的应用，`nomi_app_set_return` 记下调用方，被打开的应用从首页划回去时回到调用方（只记一层，任何关应用都清掉）。`kApi`、`NOMI_API`、`build.py` 的 `API` 都是 5。
- **应用**（`apps/camera` 0.2.0，`app.wasm` 2.4KB）：只有取景页（拍照 / 问它 / 照片）和语音指令 `shoot`（不在前台也能拍）。照片存卡上 `DCIM/IMG_0001.jpg` 起，编号接着卡上最大的往下排；取景页屏幕常亮。0.1.0 自己做过一套看照片（翻、菜单、删除），用户说和「图片」重复了，0.2.0 删掉：「照片」用 `open_file` 把最新那张交给图片应用（它打开 DCIM 相册、翻到这张）。从图片应用划回来要 3 下（照片 → 相册网格 → 相册列表 → 相机），问过用户，保持 3 层、不改图片应用。删照片在图片应用里没有，用「文件」或网页文件管理。
- **上机验证过**（截屏 + 串口）：取景器（竖版、横版都看过，用户看横版说 OK）、拍照存卡（取景器开着 0.4 秒一张，连拍 5 张大小稳定 47.4KB、画面干净、无看门狗）、语音 `shoot`、问它（竖版固件上用户问「这是什么？」，照片跟着发出去，模型答了桌上有什么）、「照片」交给图片应用（点下到看见照片约 2.3 秒，其中媒体库重扫 1 秒——卡上刚多了一张）、从图片应用划 3 下回到相机。
- **没验证**：横版固件上的问它；取景器开着时按钮跟不跟手。问它那次之后连着十几轮「听到声音但识别为空」，没查是环境声音还是预览影响了收音。
- 卡上 `DCIM/IMG_0001.jpg` 是调试时拍的，可以删。

## 已修：对话浮层的交互（2026-11-04，待上机验证）

问题：开着设置页叫它说话，浮层（`s_sheet`）盖住底部退出把手，设置页又锁着翻页，AI 说完之前退不出去；顺带浮层本身也只能干等。

按用户意见做了三层，`nomi_ui.c` + `nomi_display.cc`：

- **半透明**：浮层底色 `LV_OPA_80`（原来不透明），顶上 1px 细边保留；要更透/更实就调 `create_sheet` 里那一行。
- **右上角 ×（随时收起打断）**：细线两笔画的 ×（不靠字体，44×44 手感区）。点了：说话中先走 `abort_speaking` 动作打断（`nomi_display.cc` 的 `Action` → `Application::AbortSpeaking`，`Schedule` 进主循环；只在 `kDeviceStateSpeaking` 时发），然后收起；在听 / 想的时候只收起不打断。收起后**本轮对话**不再升（`s_sheet_dismissed`，新一轮对话开始时清掉），不想用 × 的人浮层收起后一切照旧。
- **设置页解锁兜底**：浮层升着的时候设置页翻页锁放开（`sheet_slide` 里浮层起来时解锁、收起时照常锁回；`lock_outer_if_settings` 在 `s_sheet_up` 时不锁），上下推随时能退回主界面。设置页在第 0 行，解锁后只能滚回主界面；内层列表照常先滚、到边才带出翻页，"上下推翻内容"没动。
- 桌面、应用查过没有同类死锁：返回走全高左边缘（`nomi_app.c` 的 `add_edge`），浮层只盖底部 120px，上段照常能划。
- 上机验证：开着设置页说话、说话中退出设置、对话中下拉进设置再退出、翻到收听页/时钟页再说话，都不卡；说话中点 × 立刻闭嘴（一问一答回待机、连续对话回听）；× 收起后本轮不再升、下一轮对话照常升；浮层收起后设置页恢复锁住；半透明下对话文字读得清、下面内容隐约可见。

## 图片、文件、打开方式、媒体库（2026-11-04，已提交）

接口版本升到 3（`kApi`、`NOMI_API`、`build.py` 的 `API`）。三个预装应用都改成 api 3、升了版本（音乐 0.5.0、小说 0.3.0、图片 0.2.0），预装包已重打。固件剩 4%（0x2ca20）。

- **图片**（`nomi_image.cc` 解码、`nomi_photo.cc` 缩略图 / 看图 / 电子相框，应用 `apps/photo`）：JPEG 用 esp_jpeg 组件里的 TJpgDec R0.03（`CONFIG_JD_USE_ROM=n`、`CONFIG_JD_FASTDECODE_TABLE=y`），边读边解、缩放 1/2～1/8、按 EXIF 转正，先用照片自带的小图当预览；PNG 用 ROM 里的 miniz 边读边解（不认隔行）。缩略图、全屏图缓存在卡上 `nomi/cache/images/`。电子相框：设置里「睡着时显示」选相册，设备睡着时轮播（不调暗、到息屏时间照样关）。网页文件管理看大图走 `/sd/preview/<宽>x<高>/<路径>`（设备当场解成 BMP）。上机测过：4000×3000 6.1 秒、1280×960 1.8 秒、600×400 PNG 0.49 秒、EXIF 小图 125ms，方向对；用户看过说没问题。电子相框没上机测（用户设的从不睡着）。
- **文件服务**（`nomi_files.cc`）：卡上相对路径的规矩只有一份（不许 `..`、反斜杠，系统的 `nomi/` 不显示不让改），网页文件管理、应用的 `fs_*`、设备上的「文件」都走它；改了卡上的东西记一笔（`Changed`）。网页「设备 → SD 卡」改成文件管理：一层层点、新建文件夹、上传到当前文件夹、改名、删除、下载（`/sd/file/` 边读边发）、看大图、「设备上打开」。Playwright 连真机测过（除了「设备上打开」）。
- **写卡**：`fs_write`（参数改成 `path, buf, size, append`）`fs_remove` `fs_rename` `fs_mkdir`，`sd:/` 要新权限 `sd.write`。编过，没有应用用过。
- **打开方式**（`nomi_open.cc`）：manifest 的 `opens` 声明扩展名；点了文件：有记住的默认用默认，只有一个用它，好几个弹浮层选（能勾「以后都用它」，存 NVS `nomi_open`，写 NVS 排到主循环）。应用界面挂上后收 `NOMI_EV_OPEN`、`open_path` 取路径（`NomiWasm::OpenFile`：新起的进程排在 SHOW 后面，已经在前台的直接发）。应用管理每行显示「默认打开 .jpg」、有「清除默认」；应用删了默认也清掉。网页 `POST /sd/open {"path"}` 先叫醒设备。
- **浮层**（`nomi_sheet.cc`）：`lv_layer_top` 上的面板，选应用、长按菜单共用；要睡着时收起（`NomiDisplay::SetSleep`）。最上层不继承屏幕字体，要自己设（`NomiFont::Base()`）。
- **文件列表控件**（`nomi_filelist.cc`）和**系统应用「文件」**（`nomi_filesapp.cc`）：列文件夹、删东西在自己的后台线程（`nomi_files`，栈在 PSRAM，只碰卡）做，回来拿显示锁、核对是不是还是那一层；一次画 60 行，多的点「再列」。「文件」：点文件交给打开方式，长按出浮层（大小时间、打开方式…、删除点两下），左边缘右划先回上一层，排在桌面应用包后面、应用管理前面（`nomi_app_t.system`）。应用用 `ui_files` `files_picked`、`NOMI_EV_FILE`。
- **媒体库**（`nomi_media.cc`）：整张卡扫一遍，歌 / 书 / 图片各一个文件存在 `nomi/cache/media/`（`index.txt` 记卡序列号、总容量、剩余空间、各几个）。开机后第一次用：同一张卡、剩余空间没变就直接用；之后同一次开机里卡被拔过（`NomiSdCard::Epoch`）或系统改过卡上的东西（`NomiFiles::ChangeCount`）才重扫。缩略图缓存会改剩余空间，所以用过图片的话下次开机第一次会重扫一遍。应用 `library_list(kind, first, buf, size)`。
- **应用**：小说书架、图片相册改用媒体库（「重新找」= 强制重扫）；音乐语音点歌改查媒体库（原来一层层列文件夹、最多 48 个），按文件夹浏览、排队列照旧 `fs_list`。三个应用都接了 `NOMI_EV_OPEN`：音乐把那首所在的文件夹排成队列、从这首放；小说直接打开（不在书架上的小文件也能看）；图片打开所在相册看这张，不算照片的（小图、封面）单独看。
- **步骤 3～6 只编译过，没上机。** 上机验证清单：桌面上有「文件」、排在应用管理前；文件里一层层点、上一级、左边缘右划回上一层、多于 60 个的「再列」；点 .txt / .jpg / .mp3 分别打开小说 / 图片 / 音乐（新起的、后台跑着的、已经在前台的三种情况）；同一扩展名装两个应用时弹选择、勾「以后都用它」后不再弹、应用管理显示并能清除；长按删除文件和文件夹；网页「设备上打开」（设备睡着 / 息屏时也能叫醒）；浮层开着时睡着会收起；媒体库第一次扫多久、第二次秒回、网页上传或删除后重扫、拔卡换卡后重扫；小说书架、图片相册、音乐语音点歌结果和原来一致。

## 小说应用 v0.2（2026-11-04，上机打开过，细节待验证）

分层照旧：系统给能力，应用管业务。

- **系统**（`nomi/reader/`）：`nomi_book.cc`（认 UTF-8 / GBK、按文件位置读一段转 UTF-8、找章节，不依赖设备，电脑上 zig c++ 能编）、`nomi_reader.cc`（阅读器控件：LVGL 断行排版、分页、往回翻、手势）、`nomi_font.cc`（看书的字：系统字 + 卡上 `fonts/*.ttf` 补字，tiny_ttf）。`nomi_wasm.cc` 加了 `ui_reader` `reader_goto` `reader_set_flags` `reader_text` `text_chapters` `text_chapter_at` `fs_find`，接口版本升到 2（`nomi_pkg.cc` 的 `kApi`、`nomi_sdk.h` 的 `NOMI_API`、`build.py` 的 `API`）。
- **配置**：板子 `config.json` 开了 `LV_USE_TINY_TTF`（带文件读）和 LVGL 的 stdio 文件驱动（盘符 `S`，`S:/sd/...`），改了要用 `scripts/build.py` 重新生成 sdkconfig。SD 卡同时开的文件数 4 → 6。固件剩 6%。
- **资源分区**：`preinstall/gbk.tab`（GBK 对照表 48KB，`tools/gen_gbk_table.py` 生成）、`preinstall/nomi.novel.npk`（改了应用要重打）。桌面上原来的「小说」占位应用删了。
- **应用**（`apps/novel`）：书架（`fs_find` 整张卡找大于 16KB 的 .txt，上次的记在 kv、打开先显示再后台重找）、看书（点中间或长按出快速跳转：`ui_slider` 拖进度、上一章 / 下一章、目录、翻页方式五选一（滑 + 点上下 / 滑 + 点左右 / 只滑 / 只点上下 / 只点左右）、回书架；左边缘右划回书架；第一次打开弹一句「点屏幕中间」的提示）、书签（每本书一个 kv，翻页后 10 秒内存）、目录（一批 40 章，上一批 / 下一批）、语音 `open` `next_chapter` `prev_chapter` `chapter`、当前页给大模型当上下文。
- **字体文件**：卡上 `fonts/` 放静态 TrueType。用的 Noto Sans SC：Google Fonts 的是可变字体、默认字重 100（太细），用 fontTools 转成字重 400 的静态版、去掉排版表：`instancer.instantiateVariableFont(vf, {"wght": 400})`，再 subset 掉 GSUB/GPOS 等，10MB、3 万多字。
- 电脑上测过 `nomi_book.cc`：繁体《西遊記》UTF-8 / GBK 两份，任意位置切开一页页往后读和整本一次读完全一致，两份都找到 100 回，章名判断（含「第三回合他赢了。」这种不算）都对。
- **往卡上传文件**：`POST /sd/upload`，请求体第一行是卡上的路径（`fonts/x.ttf`），后面是文件内容；写到 `.part` 收完再改名，`nomi/` 下不让写。网页设置页「设备 → SD 卡」里能上传：填卡上的文件夹（有小说 / fonts / 根上三个快捷按钮），一次多选文件，一个个传、带进度，nomi 开头的文件夹网页上先拦下（浏览器实测过）。也能用 curl：`{ printf 'fonts/x.ttf\n'; cat x.ttf; } > body; curl --noproxy '*' --data-binary @body http://设备/sd/upload`（这台电脑有代理，要 `--noproxy`）。Wi-Fi 弱时很慢：10MB 字体传了 881 秒；收得快的时候 Wi-Fi 任务占满核 0，报过一次任务看门狗警告（没重启）。
- 2026-11-04 上机：书架整张卡找书 59ms；补字字体加载 20ms，行高和系统字一样（31）；每页 7 行 × 416 像素；第一次扫《西遊記》100 回用了 5.1 秒（之后用卡上缓存）。主人看过、说行。GBK 那本、拖进度、点左右还没在日志里看到。
- 上机验证：卡上放字体和书；书架找得到书；翻页（滑、点、长按菜单）；繁体字补得上、和系统字一样大、基线对齐；GBK 的书；往回翻；目录跳章；退出再进接着上次的页；语音「打开西游记」「下一章」「跳到第十回」；补字时翻页快不快（字形从卡上读）。

## 已修：m4a 目录按需读（2026-11-04，电脑上测过，待上机验证）

- **原来**：m4a 节目要把整个 moov 读进 PSRAM 一整块，1 个半小时的节目约 1MB，更长的、或者 PSRAM 碎了就放不了。
- **现在**（`nomi_mp4.cc` 重写，`nomi_radio.cc` 的 `ReadMp4`）：moov 只顺着读一遍，记下 stts / stsc / stsz / stco 在文件里的位置，每张表只在内存里留一段窗口（stsz、stco 各 8192 条 32KB，stts、stsc 各 2048 条；按表实际大小分配，AAC 常见的 stts、stsc 只有几十字节）。大表剩下的部分不到 128KB 就读掉扔了，多了重新开一段跳过去。放到窗口外面（44.1kHz 下约每 3 分钟一次）先断开正在下的音频、读下一段表、再从那一帧接着下——一条连接轮着用，不会同时开两条 TLS；中间靠 64KB 收流缓冲垫着（64kbps 约 8 秒）。
- 换了别的在放时，上一期的窗口放掉（原来那 1MB 一直留到下一期 m4a）。
- 电脑上测过（`nomi_mp4.cc` 能直接用 zig c++ 编）：ffmpeg 生成的 1.5 小时节目（moov 在开头 / 在末尾）、带视频轨的 mp4、故意改成每块 1～8 帧轮换 + co64 + 每帧一条 stts 的压力文件，从 10 个起点放到结尾，逐帧位置大小和原来整块读的解析、和 ffprobe 列的包都一致；窗口缩到 7 条再跑一遍也一致。1.5 小时的节目内存约 33KB，压力文件约 104KB。
- 上机验证：放一期小宇宙（m4a），日志看 `m4a index: … KB in memory`；放过 3 分钟、过窗口那一下听着不断；快进快退、对话后接着放都对；放长节目（2 小时以上）也能放。

## 已修：装在 SD 卡上的应用拔卡后灰着（2026-11-04，待上机验证）

- **原来**：拔卡后图标看着和平时一样，点了才弹「SD 卡没插」；不插卡开机，卡上的应用干脆不在桌面上。
- **现在**：底块淡到 40%、图标和名字变成灰色（`C_DIMMED`），点了提示「插上 SD 卡才能打开「X」」。机器内部分区里记着卡上每个应用的 manifest（`/int/nomi/sdapps/<id>.json`，`NomiPkg::CacheSd`），不插卡开机也照样灰着出现在桌面上。
- 没有插卡检测脚：桌面翻到眼前（`nomi_launcher_shown` → `launcher_shown` 钩子，有卡上的应用、3 秒内没看过才看）、点了灰着的应用（`dimmed_tap`）、卡上的应用启动时发现没卡，都去挂一下卡看（`NomiPkg::CheckSd`，排到应用任务里做，最多排一个）。卡回来了就恢复，点的那个顺手打开；卡上新装的也登记上；卡在但卡上没有它的也灰着，提示重新装。
- 卡不在时不让删卡上的应用（应用管理里点删除会提示先插卡）：卡上的文件删不掉，下次插卡又冒出来。
- 代价：翻到桌面那一下要挂一次卡（只在装了卡上的应用时，卡在约 0.1～0.2 秒，在应用任务里做，不卡界面）。另一种焊法（R30 焊上）的板子挂着卡时功放没声，那种板子翻桌面会让正在放的歌停十来秒——这块板不受影响。
- 上机验证：拔卡后翻到桌面，卡上的应用变灰、点了有提示；不插卡开机也灰着；插回去再翻一次桌面恢复，点灰着的那个能直接打开；卡不在时在应用管理里删它会提示插卡。

## 已修：放歌时永远不进睡眠（2026-11-04，待上机验证）

- **现象**：只有彻底待机才会按设置变时钟/调暗/息屏；放着歌就永远不睡。
- **原因**：收流解码的 PCM 走 `PushPcmToPlaybackQueue` 进了 AudioService 播放队列，而 `Application::CanEnterSleepMode`（application.cc）要求 `audio_service_.IsIdle()`——放歌时队列永远有货，睡眠计时永远清零。
- **修法**：`CanEnterSleepMode` 加了板子钩子 `SetPlaybackAllowsSleep(check)`——走到「队列没排空」这一步时设备状态已是待机（对话中进不来），队列里有货只可能是后台播放，板子说了算。SpotPear 板给的是 `NomiRadio::Wanted()`（放着/连着/让路中都算）。其他板子不设钩子，行为照旧。
- 效果：放着歌没人碰，到「多久后睡着」就切时钟（或表情）、30 秒后调暗、到「多久后息屏」灭屏，**歌继续放**；摸一下屏幕回睡前的页面。唤醒词在睡着时也能用（这块板睡眠不关唤醒词）。
- 顺带发现的边角（没动）：闹钟响铃不抑制睡眠计时，没人碰屏幕响久了也会变暗/灭屏。要修的话在铃声起止时挡一下计时。
- 上机验证：放歌等「睡着」时间到，屏幕切时钟、变暗、息屏，歌不断；摸屏幕回收听卡片；对话中不会中途睡着。
- **2026-11-04 上机发现并修了**：「多久后睡着」设成从不、「多久后息屏」还有时间时，到点只关背光、不进睡眠；`PowerSaveTimer::WakeUp` 只在睡眠里才回调 `OnExitSleepMode`，所以摸屏、唤醒词、按 BOOT 都亮不回来，只能按 power 重启（这个组合不放歌时本来也会这样，放歌不挡睡眠后更容易碰到）。板子加了 `screen_off_` 和 `WakeScreen()`，所有叫醒的地方都走它，息屏过就还原亮度。上机验证：睡着设从不、息屏设 2 分钟，等黑屏后摸一下能亮；黑屏时喊唤醒词也亮。

## 已修：断网不再卡在配网提示，离线照常能用（2026-11-04，待上机验证）

- **现象**：断网时（尤其是断着网开机）一直提示进配网模式，网络恢复了也出不来。
- **原因**：`WifiBoard::TryWifiConnect`（boards/common/wifi_board.cc）开机起 60 秒定时器，连不上就 `OnWifiConnectTimeout` → 停站点 + `StartWifiConfigMode()` 开热点；进了配网站点就是停着的，只有网页提交配置（`ConfigModeExit`）才会再连，没有任何自动恢复。机器在 -80 dBm 弱 Wi-Fi 下，路由器重启没就绪就必中。
- **修法**：超时不再切配网——站点留着按自己的退避间隔继续扫（10 秒起步、最长 5 分钟一轮，本来就是现成的），只在 60 秒那一刻弹一次提示「连不上 Wi-Fi，会自动重试；离线也能用，设置里可开配网热点」（新语言串 `WIFI_RETRY_OFFLINE`，zh-CN / en-US）。网络一恢复 `Connected` → `HandleNetworkConnectedEvent`（状态还是 starting）→ 照常激活。
- **离线模式就是现状**：应用系统、时钟闹钟、本地音乐 / 小说 / 图片都不依赖网络（自己查 `NetworkUp()`，联网接口返回 `E_NO_NET`），唤醒词模型开机就从本地资源分区加载。语音对话离线不可用（没有协议，按了没反应）。屏幕每轮扫描会弹「连接 / 扫描 Wi-Fi...」，状态栏图标显示离线。注意离线开不了 SNTP，**时钟页时间不对**（机器没有 RTC 硬件），联网后自动对时。
- **配网只剩手动入口**：启动没完成（=没网）时按一下 BOOT 键、设置页「开配网热点」、网页提交配置后重连。首开（没存过 Wi-Fi）仍然自动进配网，没变。
- 上机验证：**正常联网路径已验**（刷机后 9 秒同步网络时间、无超时提示、照常进待机）。断网分支待验：关路由器 Wi-Fi 重启设备，等 60 秒应只弹一次提示、界面照常；开回 Wi-Fi 最快 10 秒、最慢 5 分钟自动重连激活。串口里对应 `WiFi connection timeout, keep retrying in the background` 一条。
- **2026-11-04 刷机顺带看到的**：这台机器的口号从 COM11 变成了 COM3（插拔换了 USB 口）；开机 SRAM 最低水位降到 ~5.2KB（handoff 之前记 9.6KB，蓝牙开机就开 + 资源分区开机就应用，底部更紧了，目前没出事）。

## 离线热点：断网自动开个控制热点（2026-11-04，待上机验证）

用户要的：离线时网页控制也得有——设备自己开个热点，手机连上来用网页设置页控制。

- **`nomi_hotspot.cc/.h`**（`nomi/hotspot/`）：开机注册事件，断网满 60 秒开热点（开机没网、运行中断网都算；闪断不打扰），拿到 IP 自动关。**APSTA 模式**：`esp_wifi_set_mode(APSTA)` 只给跑着的站点加开一个 AP 面，重试照旧（绕开 WifiManager 的互斥，它只管自己那套）。SSID `Nomi-XXXX`（softAP MAC 后四位）、开放热点、captive portal（复用组件的 `DnsServer`，所有域名指到 192.168.4.1，手机连上自动弹设置页）。
- **网页服务不用动**：80 端口监听所有网卡，热点一开，全功能设置页自动从 `http://192.168.4.1/` 可达；`DeviceIp()`（config_server 和 nomi_display 两处）没 STA IP 时落到 AP。离线时没有协议（没 TLS ~30KB），内存够 AP + DNS 用。
- **界面**：不做新浮层——设置→网络页里那张二维码直接换（`hotspot` 文本在设置页签名里，热点开了下次下拉自动重建；nomi_display 的胶水加了 `hotspot_mode`：`config` / `offline`）。离线热点标题「离线模式」，提示「连上会自动打开设置页」；配网热点照旧「配网中」。60 秒超时提示改成「下拉设置可扫码连热点控制」。
- **换地方连新 Wi-Fi**（公司场景）：连着热点在网页上存公司 Wi-Fi（`/wifi/add`，本来就是这一份 SsidManager）→ 没联网时存完立刻触发一次扫描（不然退避最长 5 分钟才发现）→ 连上公司网后热点自动关，手机切到公司网照常访问设备。注意公司网开了 AP 隔离（客户端互相不通）时，手机访问不到设备，热点方案在**连不上任何网**时才好用。
- **和配网热点的冲突**：手动「开配网热点」前先 `NomiHotspot::StopForConfigMode()`（同一个 AP 接口，nomi_display 的 reconfigure_wifi 动作里调）。
- 上机验证：**正常联网路径已验**（刷入后完整启动：4 秒拿到 IP、激活进待机、时间同步、热点没误开；第一版在构造函数里注册事件崩了重启循环，挪到 `StartNetwork` 之后修好——见「踩过的坑」）。断网分支待验：关路由 Wi-Fi 重启 → 60 秒后热点广播、提示弹一次、网络页有二维码；手机扫码连上自动弹设置页；开回路由 Wi-Fi → 热点自动关、自动重连激活；连着热点时看内部 RAM 余量（日志 `Hotspot Nomi-XXXX up, internal free`）。

## 表情更活：球面视线、摸屏看手指、顺带眨眼、动作洗牌袋（2026-11-07，电脑上测过，已刷机，用户试后说观感不错）

起因：参考 grokbot.yin.wiki（对 Grok Bot 头像引擎的逆向笔记，只借思路，不拿它的动画数据）。改动都在 `core/face.c`、`face.h`，加 `nomi_ui.c` 的触摸入口。

- **视线是球面投影，不再整体平移**：两只眼睛在半径 `EYE_R` 的球面上（`EYE_*` 常量在 `face.c` 开头），视线 gx/gy 是头转的方向。看向一侧时，那侧的眼睛被"转到侧面"变窄、挪得少，另一侧变宽、挪得多，眼距略缩；上下看只带一点压扁。调转头幅度改 `EYE_YAW_MAX`（现 0.28，满视线时两只眼睛各挪约 14～16 逻辑像素，原来是 8）；改 `EYE_AZ0` 要同时改 `EYE_R`（= 30 / sin(EYE_AZ0)）保持静止眼距。
- **摸屏看手指**：`face_look_at(x, y)` / `face_look_release()`；`nomi_ui.c` 的 `on_face_touch` 在表情画布上收 PRESSING / RELEASED / PRESS_LOST，按"脸中心水平 150 像素、垂直 100 像素外算看到头"换算。松手后再盯 0.45 秒；没收到松手事件 1.2 秒超时也松开；睡着不跟；表情脚本明确写了 `look` 的步骤仍以脚本为准。跟的时候视线弹簧调硬（k 170 / d 16）。翻页滑动时表情本来就不画（`face_visible()`），所以只有"按住不动 / 小幅移动"看得到。
- **顺带眨眼**：定时眨眼留作兜底（2～5.5 秒，15% 概率连眨两下）；另外视线突然跳开（左右张望、新看点、手指刚按下）、换动作、换状态时 35～40% 概率顺带眨一下（`request_blink`，离上次眨眼不到 0.9 秒不眨，免得游移时眨个不停）。电脑上跑 10 分钟待机：约 22 次/分钟（改前约 16），连眨约占 7%。嫌多就调 `request_blink` 的三处概率或连眨概率。
- **动作池 + 洗牌袋**：`kVariants` 现为 待机 5 / 聆听 3 / 思考 4 / 说话 4 / 开心 3 / 疑惑 3（动作清单在 `face.c` 里 `kVariants` 上面的注释）。新增：待机「偷笑」、聆听「歪头听」、说话「点头 / 歪头晃着讲 / 往上看想词」、开心「摇摆 / 再蹦几下」、疑惑「摇头 / 眼神游移冒汗」。抽取改用每个状态一袋的洗牌袋（`bag_pick`）：一轮里每个动作出现一次，不连着重复；袋子在离开状态再回来时接着抽。待机的 0（平常张望）穿插在特殊动作之间，不进袋。
- **验证**：电脑上 `#include face.c` 的测试程序（没放仓库）——待机 10 分钟的眨眼频率 / 最小间隔、洗牌袋的排列和不重复、各状态进入先演 0、触摸看向 / 松开 / 超时 / 睡着不跟、所有动作渲染无 NaN 和越界脏区，全过；`tools/face-sim` 的 wasm 里手指在右时右眼 14px、左眼 19px 宽（静止各 18px）；`idf.py build` 通过，face.c / nomi_ui.c 无新警告，应用分区还剩 9%。**上机**：2026-11-07 刷机（`idf.py -p COM11 flash`），开机日志无 panic / 断言，Wi-Fi 和设置都在；用户试用后整体说「感觉不错」。**没单独逐项确认**：触摸换算的准确度、按住不动超过 1.2 秒眼睛会不会松开（PRESSING 事件不持续的话会）、眨眼频率的具体感受，有人反馈再调。
- **没做（评估过）**：动作数据化（clip + 贝塞尔）和本机关键帧编辑器（没遇到要解决的问题）、人设影响性格（做的话是设置里一个「活泼度」，别解析人设文字）、对话浮层里放迷你脸（和屏幕 DMA、CPU 抢资源，用户原话「浮层不一定用 Nomi 的脸」）、静止时降帧（`face_visible()` 已在不可见时停渲染，放歌时已隔帧）。
- 电脑上预览：`python tools/face-sim/build.py`，再在该目录 `python -m http.server 8800` 打开 `sim.html`，按住鼠标眼睛就跟着。

## 第 ④ 步：完成情况（2026-11-03）

目标达成：固件里不再有电台台单、播客、在线点歌的业务，全在音乐应用里；老 NVS 数据不迁移，设备上的已经用 `tools/erase_legacy_nvs.py` 清掉（备份在仓库外 `C:\Users\yuxin\code\ESP32\`）。

- **应用（0.4.0）**：台单进 kv `stations`（没动过就用 `assets/stations.txt` 的默认值，一改整个存进 kv），应用网页上增删、**排序**（↑↓）、试播；台单里没有的台名去 radio-browser 目录搜（语音里说台名 / 类型也走它，满意说「收藏这个台」进台单）；语音加了 `radio` 指令（空 = 接着放、「下一个 / 上一个」= 换台、台名 = 放 / 搜、「收藏」）。
- **老 NVS 数据不迁移**（用户定的）：固件里一行清理代码都没有——只跑一次的事不放固件。要腾 nvs 的地方就在电脑上跑一次 `tools/erase_legacy_nvs.py --port COM11`（esptool 读下 nvs 分区留备份 → 删 `nomi_radio` `nomi_pod` `nomi_music` 三个命名空间 → 别的原样写回，离线测过往返无损）。台单、订阅在音乐应用里重新加。
- **删掉的**：`nomi_podcast.*` 整个；`nomi_music.cc` 的在线点歌（Search/Play/网页接口/`self.music.play`）；`nomi_radio.cc` 的台单、搜台、网页接口和 `self.radio.play/save/remove`（留 `self.radio.stop`）；网页设置页「收听」三页和菜单项；系统收听页的标签、台单 / 订阅 / 节目单列表（只剩「正在放」+ 歌词，`NomiMusic::Hint` 提示装音乐应用）；`on_progress` 回调（进度归应用）。表情页左上角小条、放歌表情、闹钟让路都没动。
- 上机还没验：`tools/erase_legacy_nvs.py` 对真机跑一遍、收听卡片、搜台。

## 已知问题和风险

| 问题 | 说明 |
|---|---|
| PSRAM 紧 | 音乐应用常驻约占 750KB（内存 512KB + 运行时），装上后剩 1.3MB、最大块 1.3MB。m4a 的目录 2026-11-04 改成按需读了（最多约 100KB，和节目多长无关），不再是瓶颈 |
| CPU 满载 | 唤醒词 + 收音一直 ~38%；放 96k/24 位 FLAC 解码 + 读卡 ~22%；表情页放歌时的动画 ~30%。三样叠在一起会卡（用户接受「到上限就算了」）。已做：解码优先级降到界面之下、放歌时表情隔帧画 |
| 内部 RAM | 开蓝牙后平时空闲 ~60KB、开机最低 9.6KB（见「手机连接、地图」）。TLS 一条要 ~30KB，同时只能一条（`HoldForHttps`） |
| 音乐网站慢 | inmp3.com 搜一次 ~15 秒，取歌曲页 + 探格式又十几秒 |
| 没验证 | 第 ④ 步的迁移（台单顺序、播客进度接着放）、搜台、收听卡片；拔掉 SD 卡开机音乐应用能不能起来（远程做不了）；首页卡片顶上四个入口改宽后是否都显示全（没看屏幕）；真的手机导航和 GPS（见「手机连接、地图」） |
| 小毛病 | 随机播放时「上一首」也是随机；预装应用删不了也没有「恢复」；网页装应用不能自己选装在哪；装在 SD 卡上的应用在后台跑着（有首页卡片、在放歌）时拔卡，卡一直算挂着，图标要等它停了才灰 |

## 踩过的坑（速查）

- **栈在 PSRAM 的任务不能读写内部闪存（含 NVS）**：读写闪存时 cache 关掉、访问不了 PSRAM，`esp_task_stack_isane_cache_disabled` 断言重启。应用进程、`nomi_jobs`、播客 / 音乐后台任务的栈都在 PSRAM：碰 `/int` 走 `NomiFlashIo::Run` / `ForPath`，写 NVS 排到主循环（`Application::Schedule`）。
- **Windows 上全量重编会撞 CreateProcess 的 32767 字符上限**：`project_elf_src` 那条编译命令带全部组件的 include（WAMR 又加了几个目录），超了就报 `CreateProcess failed / The parameter is incorrect`。修法（已写进 build 缓存）：`idf.py reconfigure -DCMAKE_NINJA_FORCE_RESPONSE_FILE=1`，include 进 @rsp 文件。`scripts/build.py` 会重建编译缓存，跑完它要再来一次。
- **分区挪了位置，第一次刷机前删 `firmware/build/*_flashed.bin`**：IDF 6.1 只刷和上次不一样的部分，按文件名拿上次刷进去的副本比，不管分区挪没挪。
- **Gadgetbridge 把每行 `\n` 前面那个字符当 `\r` 去掉**（Espruino 的习惯）：板子发回去只带 `\n`，会吃掉 JSON 最后的 `}`，手机上弹「Malformed JSON」。行尾要 `\r\n`。
- **蓝牙要开机就开**：控制器要一大块连续的内部 RAM，开机一阵过去就碎了，推迟开申请不到。
- **`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0` 之后普通 `malloc` 都给 PSRAM**：要内部 RAM 的（DMA、闪存操作时要用的、要快的）自己 `heap_caps_malloc(…, MALLOC_CAP_INTERNAL)`。
- **拉了新代码别只 `idf.py reconfigure`**：sdkconfig 是旧的，新组件（比如 WAMR）会按自己的默认值补进去（WASI 默认开，链接就报 `os_openat` 未定义）。要跑 `python scripts/build.py spotpear/spotpear-esp32s3-max35 --name spotpear-esp32s3-max35` 按 config.json 重新生成 sdkconfig。
- WAMR 必须在 pthread 里跑（加载时调 `pthread_self`），普通 FreeRTOS 任务里断言重启。
- 板子构造函数里别调 `Board::GetInstance()`（单例没建完，自己等自己）。显示对象由板子传给 `NomiWasm::Init`。
- **板子构造函数里默认事件循环还没建**（`WifiManager::Initialize` 才建）：注册 Wi-Fi 事件（`esp_event_handler_instance_register`）直接 abort、重启循环。要挂 Wi-Fi 事件的模块在板子里重载 `StartNetwork()`，`WifiBoard::StartNetwork()` 之后挂（离线热点 `NomiHotspot::Init` 就这么干的）。
- LVGL 字体声明（`LV_FONT_DECLARE`）别写进匿名命名空间，链接找不到。
- zig 默认带调试信息，app.wasm 大三倍：`build.py` 已加 `-g0 -Wl,--strip-all`。
- 应用内存别开太大：开到 1MB 时 PSRAM 不够读 m4a 的 moov，连固件自己的播客都放不了（2026-11-04 起 moov 不整块读了，但 PSRAM 还是紧）。
- 首页卡片顶上那排入口宽度写死 244 像素时，第四个按钮被截掉；现在跟着按钮走。
- 网页设置页的接口最多 7 + 32 个（`max_uri_handlers`），再加接口注意别超（前缀接口 GET、POST 各占一个；2026-11-04 共用 23 个）。
- 浮在 `lv_layer_top` 上的东西不继承屏幕的字体（中文变方块），自己设 `NomiFont::Base()`；睡着时记得收起，不然压在时钟上。
- LVGL 9 长按之后松手还会来 `LV_EVENT_CLICKED`：同一行既能点又能长按时，点用 `LV_EVENT_SHORT_CLICKED`。
- 在一行自己的点击回调里重建整个列表：别直接 `lv_obj_clean`，把旧的藏起来再 `lv_obj_delete_async`。
- 更多（唤醒词、SD 卡引脚、屏幕 SPI 别上 80MHz 等）见各 docs 和提交说明。

## 约定和用户偏好

- **回复用中文**，汇报、表格也是。
- 用户一个人的仓库：要提交推送时**直接推 main**，不开分支；没让提交就不提交。提交说明中文，末尾带 `Co-Authored-By`。
- 只改用户点名的东西；要改界面布局、尺寸先分清楚改哪。
- 交互参考 Wear OS：表情页是表盘，左右是卡片，应用里只上下滚、左边缘右划返回，不放返回按钮。对话不翻页、不关应用。
- 分层：**系统只给能力，业务归应用**（用户明确定的方向；别把队列、台单这类业务再写回固件）。
- 设计要通用：SD 卡、应用平台都按任意卡、任意应用设计，测试卡里的歌只是测试数据。
- 用户审美：细线条、像蔚来 NOMI；嫌偷懒（比如卡片直接塞一个应用页里的小控件，被批评过）。
- 上机测试先打招呼（见上）。
