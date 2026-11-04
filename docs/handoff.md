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
| ④ 删固件里的旧业务 | 删固件的电台台单 / 播客 / 在线，老 NVS 数据不迁移（用 `tools/erase_legacy_nvs.py` 清一次） | ✓（应用 0.4.0：台单进 kv 可在网页增删排序、radio-browser 搜台、电台语音） |

## 仓库地图

| 位置 | 是什么 |
|---|---|
| `firmware/main/boards/spotpear/spotpear-esp32s3-max35/` | 这块板子的全部自研代码（下面几行都在这里） |
| `nomi_wasm.cc/.h` | 应用运行时：进程、所有系统接口（`ui_*` `media_*` `net_*` `kv_*` `voice_*` `web_*`）、语音 / 网页请求转给应用 |
| `nomi_pkg.cc/.h` | 包管理：安装（tar 流）、扫描、预装、删除、应用网页 `/app/<id>/`、内部分区挂载 |
| `nomi_flash_io.cc/.h` | 替栈在 PSRAM 的任务读写内部闪存（见「踩过的坑」） |
| `nomi_radio.cc/.h` | 系统播放器：收流、本地文件（FLAC / WAV / MP3）、m4a、解码、对话时让路；放什么归应用（`Station.owner`），大模型工具只留 `self.radio.stop` |
| `nomi_music.cc` / `nomi_music_local.cc` | 「正在放」的封面、歌词；本地文件读标签。放哪首、音乐网站都归应用 |
| `nomi_ui.c/.h` | 主界面（表情、卡片、设置、对话浮层）；`ui_now_playing` 组件（系统收听页和应用卡片共用的「正在放」）；系统收听页只剩「正在放」（没装音乐应用时给提示） |
| `nomi_app.c/.h` | 桌面和应用层（打开、右划返回、应用管理） |
| `nomi_display.cc` | 界面和系统的胶水：界面要的文字、图片、按钮动作 |
| `spotpear_esp32s3_max35.cc` | 板子初始化、大模型工具注册（含 `self.app.command`）、网页接口挂载（应用、SD 卡） |
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
- 串口 COM11。用 IDF venv 的 Python（有 pyserial）：`C:\Espressif\tools\python_env\idf6.1_py3.12_env\Scripts\python.exe`。打开串口前先把 DTR、RTS 设成 False，不然会复位芯片；要从开机收日志就主动拉一下 RTS。
- 设备在局域网 192.168.1.40（会变，看屏幕下拉设置或串口日志 `Config page:`）。网页设置页就是这个地址。
- 常用接口：`POST /apps/install`（请求体是 .npk）、`GET /apps.json`、`POST /app/<id>/call`（应用自己的网页请求，比如 `{"cmd":"pod_play","feed":"…"}`、`{"cmd":"station_list"}`）、`GET /app/<id>/`。固件的 `/radio/*` `/podcast/*` `/music.json` 第 ④ 步删掉了。
- 清老固件的 NVS 数据（只做一次）：IDF 环境里 `python tools/erase_legacy_nvs.py --port COM11`（先加 `--dry-run` 看会删什么；会自动留备份，恢复 `esptool write-flash 0x9000 备份.bin`）。
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

## 已修：对话浮层的交互（2026-11-04，待上机验证）

问题：开着设置页叫它说话，浮层（`s_sheet`）盖住底部退出把手，设置页又锁着翻页，AI 说完之前退不出去；顺带浮层本身也只能干等。

按用户意见做了三层，`nomi_ui.c` + `nomi_display.cc`：

- **半透明**：浮层底色 `LV_OPA_80`（原来不透明），顶上 1px 细边保留；要更透/更实就调 `create_sheet` 里那一行。
- **右上角 ×（随时收起打断）**：细线两笔画的 ×（不靠字体，44×44 手感区）。点了：说话中先走 `abort_speaking` 动作打断（`nomi_display.cc` 的 `Action` → `Application::AbortSpeaking`，`Schedule` 进主循环；只在 `kDeviceStateSpeaking` 时发），然后收起；在听 / 想的时候只收起不打断。收起后**本轮对话**不再升（`s_sheet_dismissed`，新一轮对话开始时清掉），不想用 × 的人浮层收起后一切照旧。
- **设置页解锁兜底**：浮层升着的时候设置页翻页锁放开（`sheet_slide` 里浮层起来时解锁、收起时照常锁回；`lock_outer_if_settings` 在 `s_sheet_up` 时不锁），上下推随时能退回主界面。设置页在第 0 行，解锁后只能滚回主界面；内层列表照常先滚、到边才带出翻页，"上下推翻内容"没动。
- 桌面、应用查过没有同类死锁：返回走全高左边缘（`nomi_app.c` 的 `add_edge`），浮层只盖底部 120px，上段照常能划。
- 上机验证：开着设置页说话、说话中退出设置、对话中下拉进设置再退出、翻到收听页/时钟页再说话，都不卡；说话中点 × 立刻闭嘴（一问一答回待机、连续对话回听）；× 收起后本轮不再升、下一轮对话照常升；浮层收起后设置页恢复锁住；半透明下对话文字读得清、下面内容隐约可见。

## 小说应用 v0.2（2026-11-04，上机打开过，细节待验证）

分层照旧：系统给能力，应用管业务。

- **系统**（板子目录）：`nomi_book.cc`（认 UTF-8 / GBK、按文件位置读一段转 UTF-8、找章节，不依赖设备，电脑上 zig c++ 能编）、`nomi_reader.cc`（阅读器控件：LVGL 断行排版、分页、往回翻、手势）、`nomi_font.cc`（看书的字：系统字 + 卡上 `fonts/*.ttf` 补字，tiny_ttf）。`nomi_wasm.cc` 加了 `ui_reader` `reader_goto` `reader_set_flags` `reader_text` `text_chapters` `text_chapter_at` `fs_find`，接口版本升到 2（`nomi_pkg.cc` 的 `kApi`、`nomi_sdk.h` 的 `NOMI_API`、`build.py` 的 `API`）。
- **配置**：板子 `config.json` 开了 `LV_USE_TINY_TTF`（带文件读）和 LVGL 的 stdio 文件驱动（盘符 `S`，`S:/sd/...`），改了要用 `scripts/build.py` 重新生成 sdkconfig。SD 卡同时开的文件数 4 → 6。固件剩 6%。
- **资源分区**：`preinstall/gbk.tab`（GBK 对照表 48KB，`tools/gen_gbk_table.py` 生成）、`preinstall/nomi.novel.npk`（改了应用要重打）。桌面上原来的「小说」占位应用删了。
- **应用**（`apps/novel`）：书架（`fs_find` 整张卡找大于 16KB 的 .txt，上次的记在 kv、打开先显示再后台重找）、看书（点中间或长按出快速跳转：`ui_slider` 拖进度、上一章 / 下一章、目录、翻页方式五选一（滑 + 点上下 / 滑 + 点左右 / 只滑 / 只点上下 / 只点左右）、回书架；左边缘右划回书架；第一次打开弹一句「点屏幕中间」的提示）、书签（每本书一个 kv，翻页后 10 秒内存）、目录（一批 40 章，上一批 / 下一批）、语音 `open` `next_chapter` `prev_chapter` `chapter`、当前页给大模型当上下文。
- **字体文件**：卡上 `fonts/` 放静态 TrueType。用的 Noto Sans SC：Google Fonts 的是可变字体、默认字重 100（太细），用 fontTools 转成字重 400 的静态版、去掉排版表：`instancer.instantiateVariableFont(vf, {"wght": 400})`，再 subset 掉 GSUB/GPOS 等，10MB、3 万多字。
- 电脑上测过 `nomi_book.cc`：繁体《西遊記》UTF-8 / GBK 两份，任意位置切开一页页往后读和整本一次读完全一致，两份都找到 100 回，章名判断（含「第三回合他赢了。」这种不算）都对。
- **往卡上传文件**：`POST /sd/upload`，请求体第一行是卡上的路径（`fonts/x.ttf`），后面是文件内容；写到 `.part` 收完再改名，`nomi/` 下不让写。现在只有接口，网页上没做按钮，用 curl：`{ printf 'fonts/x.ttf\n'; cat x.ttf; } > body; curl --noproxy '*' --data-binary @body http://设备/sd/upload`（这台电脑有代理，要 `--noproxy`）。Wi-Fi 弱时很慢：10MB 字体传了 881 秒；收得快的时候 Wi-Fi 任务占满核 0，报过一次任务看门狗警告（没重启）。
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
| 内部 RAM | 平时空闲 ~50KB，开机联网那阵最低到过 4.8KB。TLS 一条要 ~30KB，同时只能一条（`HoldForHttps`） |
| 音乐网站慢 | inmp3.com 搜一次 ~15 秒，取歌曲页 + 探格式又十几秒 |
| 没验证 | 第 ④ 步的迁移（台单顺序、播客进度接着放）、搜台、收听卡片；拔掉 SD 卡开机音乐应用能不能起来（远程做不了）；首页卡片顶上四个入口改宽后是否都显示全（没看屏幕） |
| 小毛病 | 随机播放时「上一首」也是随机；预装应用删不了也没有「恢复」；网页装应用不能自己选装在哪；装在 SD 卡上的应用在后台跑着（有首页卡片、在放歌）时拔卡，卡一直算挂着，图标要等它停了才灰 |

## 踩过的坑（速查）

- **栈在 PSRAM 的任务不能读写内部闪存（含 NVS）**：读写闪存时 cache 关掉、访问不了 PSRAM，`esp_task_stack_isane_cache_disabled` 断言重启。应用进程、`nomi_jobs`、播客 / 音乐后台任务的栈都在 PSRAM：碰 `/int` 走 `NomiFlashIo::Run` / `ForPath`，写 NVS 排到主循环（`Application::Schedule`）。
- **Windows 上全量重编会撞 CreateProcess 的 32767 字符上限**：`project_elf_src` 那条编译命令带全部组件的 include（WAMR 又加了几个目录），超了就报 `CreateProcess failed / The parameter is incorrect`。修法（已写进 build 缓存）：`idf.py reconfigure -DCMAKE_NINJA_FORCE_RESPONSE_FILE=1`，include 进 @rsp 文件。
- **拉了新代码别只 `idf.py reconfigure`**：sdkconfig 是旧的，新组件（比如 WAMR）会按自己的默认值补进去（WASI 默认开，链接就报 `os_openat` 未定义）。要跑 `python scripts/build.py spotpear/spotpear-esp32s3-max35 --name spotpear-esp32s3-max35` 按 config.json 重新生成 sdkconfig。
- WAMR 必须在 pthread 里跑（加载时调 `pthread_self`），普通 FreeRTOS 任务里断言重启。
- 板子构造函数里别调 `Board::GetInstance()`（单例没建完，自己等自己）。显示对象由板子传给 `NomiWasm::Init`。
- LVGL 字体声明（`LV_FONT_DECLARE`）别写进匿名命名空间，链接找不到。
- zig 默认带调试信息，app.wasm 大三倍：`build.py` 已加 `-g0 -Wl,--strip-all`。
- 应用内存别开太大：开到 1MB 时 PSRAM 不够读 m4a 的 moov，连固件自己的播客都放不了（2026-11-04 起 moov 不整块读了，但 PSRAM 还是紧）。
- 首页卡片顶上那排入口宽度写死 244 像素时，第四个按钮被截掉；现在跟着按钮走。
- 网页设置页的接口最多 7 + 32 个（`max_uri_handlers`），再加接口注意别超（第 ④ 步删掉 14 个固件的，现在余量多了）。
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
