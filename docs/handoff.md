# 交接文档（2026-11-03）

接手的人先看这份，再按需看 [app-sdk.md](app-sdk.md)（应用规范和实现进度）、[flash-partitions.md](flash-partitions.md)（分区）、[hardware.md](hardware.md)（引脚）、[online-music.md](online-music.md)（音乐网站格式）。

## 现状

Pocket Companion 是一个桌面语音伙伴：底座是小智 xiaozhi-esp32 v2.5.2（`firmware/`，git subtree），硬件是 SpotPear ESP32-S3 3.5 寸板，界面是自己画的 Nomi，设备直连 OpenAI 兼容接口，不要任何服务端。

最近在做的是**应用化**：固件只做系统能力（解码播放、联网、存储、界面控件、大模型），业务做成可安装的应用包（WebAssembly，`.npk`）。音乐（本地、电台、播客、在线点歌）已经搬进了音乐应用，并且预装在机器里，不插 SD 卡也能用。

迁移分五步，进度：

| 步骤 | 内容 | 状态 |
|---|---|---|
| ① 平台 | 每个应用一个进程、后台常驻、首页卡片、语音指令交给应用、联网接口、播放会话（放完 / 上一首下一首通知所属应用） | ✓ `1b9cdde` |
| ② 音乐应用 v1 | 本地 + 电台，收听卡片换成应用的 | ✓ `1b9cdde` |
| ③ 播客、在线 | 搬进音乐应用；应用自带网页设置页 | ✓ `0bf21ee` |
| 预装 | 机器内部的应用分区（2MB FAT）、预装应用、从 SD 卡搬数据 | ✓ `3edbf6e` |
| ④ 删固件里的旧业务 | 删固件的电台台单 / 播客 / 在线，迁 NVS 数据进音乐应用 | **待做**，见下 |

## 仓库地图

| 位置 | 是什么 |
|---|---|
| `firmware/main/boards/spotpear/spotpear-esp32s3-max35/` | 这块板子的全部自研代码（下面几行都在这里） |
| `nomi_wasm.cc/.h` | 应用运行时：进程、所有系统接口（`ui_*` `media_*` `net_*` `kv_*` `voice_*` `web_*`）、语音 / 网页请求转给应用 |
| `nomi_pkg.cc/.h` | 包管理：安装（tar 流）、扫描、预装、删除、应用网页 `/app/<id>/`、内部分区挂载 |
| `nomi_flash_io.cc/.h` | 替栈在 PSRAM 的任务读写内部闪存（见「踩过的坑」） |
| `nomi_radio.cc/.h` | 播放器：收流、本地文件（FLAC / WAV / MP3）、m4a、解码、对话时让路。**还带着电台台单和搜台（第 ④ 步要删）** |
| `nomi_music.cc` / `nomi_music_local.cc` | 「正在放」的封面、歌词；本地文件读标签。**nomi_music.cc 里还有固件的在线点歌（第 ④ 步要删）** |
| `nomi_podcast.cc/.h` | **固件的播客（第 ④ 步整个删）** |
| `nomi_ui.c/.h` | 主界面（表情、卡片、设置、对话浮层）；`ui_now_playing` 组件（系统收听页和应用卡片共用的「正在放」） |
| `nomi_app.c/.h` | 桌面和应用层（打开、右划返回、应用管理） |
| `nomi_display.cc` | 界面和系统的胶水：界面要的文字、图片、按钮动作 |
| `spotpear_esp32s3_max35.cc` | 板子初始化、大模型工具注册（含 `self.app.command`）、网页接口挂载 |
| `preinstall/nomi.music.npk` | 预装的音乐应用包（**手动打的，改了应用要重打**） |
| `firmware/main/protocols/direct_api_config_*` | 网页设置页（页面 HTML 在 `direct_api_config_page.h`） |
| `firmware/partitions/v2/16m_apps.csv` | 这块板子的分区表 |
| `apps/music/` | 音乐应用源码：`main.c`（界面、分发、本地、电台）`podcast.c` `online.c` `assets/`（台单、网页） |
| `tools/app-sdk/` | SDK：`nomi_sdk.h`（接口）`nomi_text.h`（文字工具）`build.py`（编译打包）`examples/counter` |
| `tools/device-sim/` | 浏览器整机模拟器（应用平台还没接进去） |

## 环境和常用命令

Windows，ESP-IDF v6.1。每个 PowerShell 先 `. C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1`。

```powershell
cd firmware
idf.py build                      # 平时
idf.py reconfigure build          # 板子目录里新加了 .cc（源文件是 glob 的，配置时才扫）
python scripts/build.py spotpear/spotpear-esp32s3-max35 --name spotpear-esp32s3-max35
                                  # 改了板子 config.json（分区表、sdkconfig 项）：重新生成 sdkconfig，全量编译
idf.py -p COM11 flash             # 刷机（会写程序、资源、分区表；不写 nvs 和应用分区）
```

应用：

```bash
python tools/app-sdk/build.py apps/music        # 打到 apps/music/build/，网页「设备 → 应用」上传安装
python tools/app-sdk/build.py apps/music -o firmware/main/boards/spotpear/spotpear-esp32s3-max35/preinstall/nomi.music.npk
                                                # 改了音乐应用：重打预装包，再编固件（版本号要升，机器里才会换）
python tools/app-sdk/wasm_info.py apps/music/build/app.wasm   # 看导入导出（查有没有意外的 memset 之类）
```

`build.py` 要 zig（`pip install ziglang`，系统 Python 里有）。IDF 的 Python 环境里没有，所以固件编译时不自动打应用包。

上机测试（这台电脑的环境）：
- 串口 COM11。用 IDF venv 的 Python（有 pyserial）：`C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe`。打开串口前先把 DTR、RTS 设成 False，不然会复位芯片；要从开机收日志就主动拉一下 RTS。
- 设备在局域网 192.168.1.40（会变，看屏幕下拉设置或串口日志 `Config page:`）。网页设置页就是这个地址。
- 常用接口：`POST /apps/install`（请求体是 .npk）、`GET /apps.json`、`POST /app/<id>/call`（应用自己的网页请求，比如 `{"cmd":"pod_play","feed":"…"}`）、`GET /app/<id>/`、`POST /radio/play {"stop":true}`、`GET /podcast.json`、`GET /music.json`。
- 测 CPU：板子上临时加 `SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(2000))`（`system_info.h`），测完删。
- **用户经常同时在用这台机器。上机测试会出声、切歌、重启的，先跟用户说一声。**

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

## 第 ④ 步：要做的事

目标：固件里不再有电台台单、播客、在线点歌的业务，全在音乐应用里；用户的数据一次迁过去。

1. **迁数据**（开机一次，迁完清掉旧的）：固件 NVS 里的
   - 电台台单（`nomi_radio` 命名空间 `list`，JSON 数组 `[{"n":台名,"u":地址}]`，顺序就是用户调好的）和上次放的台（`last`，下标）
   - 播客订阅（`nomi_pod` 的 `list`）、每个播客的进度（`p<哈希>`）、`last`
   - 音乐网站（`nomi_music` 的 `site`，应用里已经手动填过了）

   写进 `/int/nomi/apps/nomi.music/data/kv.json`（格式见 `apps/music/podcast.c` `online.c` 开头的注释）。应用在跑时要让它重新读（结束进程再起、或者加个事件）。
2. **应用补功能**：
   - 台单进 kv、可编辑：网页上增删、**排序**（用户一开始就要的功能，现在在固件网页「收听 → 电台」）；`assets/stations.txt` 只当默认值。
   - 按台名去网上电台目录搜台（固件 `NomiRadio::Search`，radio-browser）。
   - 电台的「接着放」「换台」语音（现在是固件的 `self.radio.play`）。
3. **删固件**：`nomi_podcast.*` 整个、`nomi_music.cc` 里的在线点歌、`nomi_radio.cc` 的台单 / 搜台 / 网页接口 / 工具，网页设置页「收听」菜单下的三页，对应的大模型工具（`self.radio.*` `self.podcast.*` `self.music.play`），系统收听页的标签和列表（只留「正在放」，没装音乐应用时给提示）。「停 / 暂停」留成系统工具。表情页左上角的小条、放歌时的表情跟着系统播放状态走，不动。
4. 更新 `docs/app-sdk.md`、`docs/flash-partitions.md`（nvs 里不再有台单、播客）、README。

## 已知问题和风险

| 问题 | 说明 |
|---|---|
| PSRAM 紧 | 音乐应用常驻约占 750KB（内存 512KB + 运行时），装上后剩 1.3MB、最大块 1.3MB。m4a 节目要把整个 moov 读进 PSRAM（1.5 小时的节目约 1MB），更长的可能放不了。根治：m4a 不整块缓存 moov，按需读 |
| CPU 满载 | 唤醒词 + 收音一直 ~38%；放 96k/24 位 FLAC 解码 + 读卡 ~22%；表情页放歌时的动画 ~30%。三样叠在一起会卡（用户接受「到上限就算了」）。已做：解码优先级降到界面之下、放歌时表情隔帧画 |
| 内部 RAM | 平时空闲 ~50KB，开机联网那阵最低到过 4.8KB。TLS 一条要 ~30KB，同时只能一条（`HoldForHttps`） |
| 程序槽 | 余量 ~240KB（6%）。第 ④ 步删代码会腾出一些 |
| 音乐网站慢 | inmp3.com 搜一次 ~15 秒，取歌曲页 + 探格式又十几秒 |
| 两套工具并存 | 第 ④ 步前，大模型同时看到固件的 `self.radio/podcast/music.*` 和应用的 `self.app.command`，都能用 |
| 没验证 | 拔掉 SD 卡开机音乐应用能不能起来（远程做不了）；首页卡片顶上四个入口改宽后是否都显示全（没看屏幕） |
| 小毛病 | 随机播放时「上一首」也是随机；预装应用删不了也没有「恢复」；网页装应用不能自己选装在哪；SD 卡上的应用拔卡后不在桌面（规范要的是变灰） |

## 踩过的坑（速查）

- **栈在 PSRAM 的任务不能读写内部闪存（含 NVS）**：读写闪存时 cache 关掉、访问不了 PSRAM，`esp_task_stack_is_sane_cache_disabled` 断言重启。应用进程、`nomi_jobs`、播客 / 音乐后台任务的栈都在 PSRAM：碰 `/int` 走 `NomiFlashIo::Run` / `ForPath`，写 NVS 排到主循环（`Application::Schedule`）。
- WAMR 必须在 pthread 里跑（加载时调 `pthread_self`），普通 FreeRTOS 任务里断言重启。
- 板子构造函数里别调 `Board::GetInstance()`（单例没建完，自己等自己）。显示对象由板子传给 `NomiWasm::Init`。
- LVGL 字体声明（`LV_FONT_DECLARE`）别写进匿名命名空间，链接找不到。
- zig 默认带调试信息，app.wasm 大三倍：`build.py` 已加 `-g0 -Wl,--strip-all`。
- 应用内存别开太大：开到 1MB 时 PSRAM 不够读 m4a 的 moov，连固件自己的播客都放不了（当时 moov 分配失败没有日志，已补）。
- 首页卡片顶上那排入口宽度写死 244 像素时，第四个按钮被截掉；现在跟着按钮走。
- 网页设置页的接口最多 7 + 32 个（`max_uri_handlers`），再加接口注意别超。
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
