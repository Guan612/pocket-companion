# 应用开发规范（草案 v0.2）

Pocket Companion 的应用像手机 APK 一样能装、能删、能更新，不用重刷固件。这份文档说明应用包长什么样、应用怎么写、能调哪些系统接口、装在哪、怎么管理。

> 状态：v0.2 部分实现（2026-11-03 上机跑通计数器示例和音乐应用）。已实现的接口见第 13 节，其余是计划；标「待验证」的地方上机测过才定。

## 1. 分工

| 部分 | 在哪 | 能不能删 |
|---|---|---|
| 系统：表盘、左右卡片（历史、收听、时钟）、下拉设置、对话浮层、桌面、「应用管理」 | 固件 | 不能，跟着固件升级 |
| 预装应用：音乐、小说、图片、地图 | 固件的资源分区（只读） | 能删（实际是藏起来），能在应用管理里恢复 |
| 自己装的应用 | 机器内部的应用分区，或 SD 卡（装的时候选） | 随便装、随便删 |

**应用只管界面和逻辑**：显示什么、点了怎么办、语音指令怎么响应。**重活都调系统接口**：读 SD 卡、解码图片和音乐、播放、联网、大模型。这些在固件里用原生代码跑，应用这边只是一行调用。

**系统给能力，业务归应用**。拿音乐举例：
- 系统的：解码、放"这一首"、封面和歌词、进度条控件。
- 音乐应用的：放哪首、播放队列、列表循环 / 随机、电台台单，以及本地 / 在线 / 播客几个页面。
- 首页的收听卡片也由音乐应用来画（像 Wear OS 的 tile）。

应用编译成 WebAssembly（`.wasm`），在设备上由乐鑫的 [wasm-micro-runtime](https://components.espressif.com/components/espressif/wasm-micro-runtime/) 解释执行。应用在沙箱里：读写越界、死循环只会让这个应用被关掉，不会把整机弄崩。

## 2. 应用包

一个应用就是一个 `.npk` 文件：不压缩的 tar 包（设备上解包简单），里面：

```
manifest.json   应用说明（必需）
app.wasm        程序（必需）
assets/         应用自带的只读文件（可选），程序里用 pkg:/ 开头的路径读
```

### manifest.json

```json
{
  "id": "nomi.novel",
  "name": "小说",
  "version": "1.0.0",
  "api": 1,
  "description": "看 SD 卡里的 TXT 小说，记住读到哪",
  "icon": {
    "lines": [[[17, 10], [4, 7], [4, 28], [17, 31], [17, 10]],
              [[19, 10], [32, 7], [32, 28], [19, 31], [19, 10]]],
    "circles": []
  },
  "permissions": ["sd.read", "assistant"],
  "memory_kb": 512,
  "voice": [
    {"name": "next_chapter", "description": "翻到下一章"}
  ]
}
```

| 字段 | 说明 |
|---|---|
| `id` | 全局唯一，反向域名风格（`作者.应用`），装新版本时按它认 |
| `name` | 桌面上的名字，最多 4 个汉字放得下 |
| `version` | `主.次.修`，装的时候比大小，只能升级（降级要先删） |
| `api` | 用到的系统接口版本。固件支持的版本低于它就拒绝安装，提示升级固件 |
| `icon` | **线条图标**：在 36×36 的格子里写折线（`lines`，每条是一串点）和描边圆（`circles`，每个是 `[x, y, 直径]`）。系统统一画成白色 2 像素细线、放进深色圆角方块，桌面风格一致，也不用解码图片 |
| `permissions` | 要用的权限，见第 6 节。装的时候列给用户看 |
| `memory_kb` | 运行时最多用多少内存（放 PSRAM），默认 256，最大 1024 |
| `tile` | `true`：这个应用占首页的收听卡片（现在只有这一个卡片位，给音乐应用用）。开机、装好后系统在后台把它跑起来，它在 `nomi_main` 里用 `ui_tile` 画卡片 |
| `voice` | 大模型能调的指令，最多 8 个。每个指令有 `name`（小写字母、数字、下划线）和 `description`（写给大模型看：什么时候用、`arg` 怎么填）。要 `assistant` 权限，见第 5 节 |

## 3. 生命周期

每个跑着的应用是一个"进程"：有自己的线程、内存和事件队列。所以音乐应用能在后台放歌、画首页卡片，同时前台开着别的应用。应用导出两个函数，系统只通过它们调应用：

```c
void    nomi_main(void);                                        // 进程起来时调一次：读设置、画卡片
int32_t nomi_event(int32_t type, int32_t id, int32_t value);   // 其他所有事情；处理了返回 1
```

**界面能挂上、拿下**：

- **从桌面打开**：
  - 没在跑：先起进程，`nomi_main` 跑完后收到 `NOMI_EV_SHOW`，在这时建第一页。
  - 已经在后台跑着：直接收到 `SHOW`。
  - 只在前台用的简单应用（比如计数器）可以在 `nomi_main` 里直接建页面，不理 `SHOW` 就行。
- **右划退出**：界面拿下，收到 `NOMI_EV_HIDE`。
  - 有理由的进程留在后台：manifest 里写了 `"tile": true`，或者它放的歌还在放（暂停着、对话时让出来的也算）。
  - 没理由的马上收到 `NOMI_EV_CLOSE`，然后结束。
  - 留在后台、后来理由没了的，30 秒后结束。
- **在后台时**：`ui_page` 这类界面调用返回 `NOMI_E_NO_UI`；卡片上的控件照常能改。
- **没在跑时有事找它**：比如它放的歌放完了、卡片上点了下一首、大模型调了它的指令。系统先在后台把它拉起来（跑 `nomi_main`），再送事件。所以之后还要用的状态（队列、放到第几首）要存进 `kv`。

| 事件 `type` | 什么时候 | `id` / `value` |
|---|---|---|
| `NOMI_EV_CLICK` | 点了按钮、列表项 | 控件 id / 0 |
| `NOMI_EV_CHANGE` | 开关、滑杆变了 | 控件 id / 新值 |
| `NOMI_EV_BACK` | 左边缘右划退了一页 | 0 / 剩几页。系统先关掉当前页再通知应用；已经是第一页时右划直接关掉应用（收到 `NOMI_EV_CLOSE`）。应用拦不住返回：那样界面得等应用回话，应用卡住时界面也跟着卡 |
| `NOMI_EV_PAUSE` / `NOMI_EV_RESUME` | 对话浮层升起 / 收起、睡着 / 醒来 | — |
| `NOMI_EV_CLOSE` | 进程要结束了：退出后没理由留着、在后台闲太久、装了新版本、被删 | — 。趁这时候把进度存好，界面已经没了 |
| `NOMI_EV_TIMER` | 定时器到点 | 定时器 id |
| `NOMI_EV_VOICE` | 大模型调了 manifest 里的语音指令 | 第几个指令（从 1 起）/ 0。用 `voice_arg` 取参数，用 `voice_reply` 回话 |
| `NOMI_EV_READER` | 阅读器翻页 | 阅读器 id / 当前读到的字节位置 |
| `NOMI_EV_MEDIA` | 你放的歌放完了，或主人在系统的播放控件上点了上一首 / 下一首 / 放 | 0 / `NOMI_MEDIA_FINISHED`、`NEXT`、`PREV`、`PLAY`，见第 5 节 |
| `NOMI_EV_SHOW` / `NOMI_EV_HIDE` | 界面挂上 / 拿下 | — |

规矩：

- **一次事件处理不能超过 2 秒**，超过就算「应用无响应」，系统关掉它。耗时的事交给系统接口做。
- 应用跑在自己的任务里，不占界面任务，应用卡住时界面照常能滑、能返回。
- **大多数应用退出就是关掉**：回到桌面时进程结束、内存收回。进度要自己用 `kv_set` 存，下次在 `nomi_main` 里读回来接着显示。会留在后台的只有上面说的两种。
- 同时跑着的应用最多 4 个。

## 4. 界面

照 Wear OS 的路子：**页面一层层往里压，应用里只上下滚，从左边缘右划退一层**。应用不自己画按钮样式，用系统给的控件，风格自动跟系统一致（黑底、白色细线、深色圆角卡片）。

```c
int32_t ui_page(const char *title);           // 压一页新页面，后面建的控件都放进去；返回页面 id
void    ui_page_close(void);                   // 关掉当前页（相当于替用户右划一下）

int32_t ui_text(const char *text, int32_t style);   // 一段字：NOMI_TEXT_TITLE / BODY / MUTED
int32_t ui_item(const char *title, const char *sub, const char *right);  // 可点的一行（列表项）
int32_t ui_button(const char *text, int32_t primary);
int32_t ui_switch(const char *label, int32_t on);
int32_t ui_slider(const char *label, int32_t min, int32_t max, int32_t value);
int32_t ui_progress(int32_t value, int32_t max);
int32_t ui_image(const char *path, int32_t w, int32_t h);  // 系统解码 JPEG / PNG，按大小缩放
int32_t ui_reader(const char *path, int32_t offset);       // 阅读器：系统排版、分页，上下翻页
int32_t ui_canvas(int32_t w, int32_t h);                   // 自己画（地图之类），见下

void ui_set_text(int32_t id, const char *text);
void ui_set_value(int32_t id, int32_t value);
void ui_set_hidden(int32_t id, int32_t hidden);
void ui_remove(int32_t id);
void ui_toast(const char *text);                           // 屏幕上方弹一句提示
int32_t ui_player(void);   // 系统画的「正在放」：封面、歌名、进度（能拖）、上一首 / 放停 / 下一首，自己刷新

int32_t ui_tile(void);           // 首页卡片（要 "tile": true）：清空重画，接下来建的控件放进卡片
int32_t ui_target(int32_t id);   // 接下来建的控件放到哪：一页（ui_page 的返回值）或卡片（ui_tile 的返回值）
void    ui_open(void);           // 打开自己的界面（卡片上点了"打开"时用），之后收到 SHOW
```

- **首页卡片**：首页往右划的收听卡片交给 `"tile": true` 的应用画（现在是音乐应用）。
  - 应用在 `nomi_main` 里调 `ui_tile()`，再放 `ui_player()` 和几个 `ui_item`。卡片上的控件点了，照常收到 `NOMI_EV_CLICK`。
  - 没装这样的应用、或者它的进程结束了，卡片位置显示系统自己的收听页。

- 控件按建的先后从上往下排，超出一屏自动可以上下滚。
- **阅读器**（`ui_reader`）是给小说准备的：一大段文字的排版、分页、翻页都由系统做（原生代码，快）。应用只给文件和起始位置，翻页时收到 `NOMI_EV_READER` 和当前位置，存下来就是书签。
- **画布**（`ui_canvas`）给地图这种要自己画的：

```c
void canvas_clear(int32_t id, int32_t color);
void canvas_line(int32_t id, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t width, int32_t color);
void canvas_rect(int32_t id, int32_t x, int32_t y, int32_t w, int32_t h, int32_t color, int32_t filled);
void canvas_text(int32_t id, int32_t x, int32_t y, const char *text, int32_t color);
void canvas_image(int32_t id, const char *path, int32_t x, int32_t y);  // 贴一张图（地图瓦片）
```

  画布能拖动时，右划返回只认从左边缘开始的（和 Wear OS 一样），其他方向的拖动交给应用（`NOMI_EV_CHANGE` 带拖动距离，待细化）。

## 5. 系统接口

### 存储

路径前缀决定读哪：

| 前缀 | 是什么 | 读 | 写 |
|---|---|---|---|
| `data:/` | 应用自己的数据目录（和应用装在一起） | ✓ | ✓ |
| `pkg:/` | 应用包里的 `assets/` | ✓ | ✗ |
| `sd:/` | SD 卡（要 `sd.read` 权限） | ✓ | ✗ |

```c
int32_t fs_list(const char *dir, char *buf, int32_t size);   // 名字一行一个，目录后面带 '/'；返回字节数，出错 <0
int32_t fs_size(const char *path);
int32_t fs_read(const char *path, int32_t offset, void *buf, int32_t size);         // 一次最多 32KB
int32_t fs_write(const char *path, int32_t offset, const void *buf, int32_t size);  // 只能写 data:/
int32_t fs_remove(const char *path);                                                 // 只能删 data:/ 里的

// 小设置和进度：存成 data:/ 里的一个小文件（不占设备那 16KB 的 NVS）
int32_t kv_get(const char *key, char *buf, int32_t size);
int32_t kv_set(const char *key, const char *value);
int32_t kv_get_int(const char *key, int32_t def);   // nomi_sdk.h 里基于上面两个写的小工具
int32_t kv_set_int(const char *key, int32_t value);
```

SD 卡由系统按需挂载（见 `nomi_sdcard.h`）：没插卡时 `sd:/` 的调用返回错误码 `NOMI_E_NO_SD`，应用要给出「没插 SD 卡」的提示。

### 播放（权限 `media`）

**系统只管放"这一首"**：解码、缓冲、对话时暂停让出来、封面歌词、进度。**放哪首、下一首放什么（队列、循环、随机）是应用的事**。

放的东西记在应用名下。收听卡片、表情页小条、下拉设置里都能看到、能控制，退出应用也接着放。

```c
// src：sd:/ 的文件（还要 sd.read）或 http(s) 地址。flags：NOMI_PLAY_LIVE 直播（电台：没有进度，断了自动重连）、
// NOMI_PLAY_EPISODE 节目（播客：左右键是倒回 15 秒 / 快进 30 秒），都不写是一首歌。
// start 从第几秒放；duration、bytes 是总长（秒）和文件大小（字节，网上的 MP3 按时间跳转要用），不知道填 0
int32_t media_play(const char *src, const char *title, const char *artist, int32_t flags,
                   int32_t start, int32_t duration, int32_t bytes);
void    media_meta(const char *cover_url, const char *lrc);  // 网上的歌、节目：配封面（JPEG 地址）和 LRC 歌词
void    media_control(int32_t cmd);   // NOMI_MEDIA_PAUSE / RESUME / STOP / TOGGLE
void    media_seek(int32_t second);
int32_t media_state(void);            // 0 没在放、1 连接中、2 在放、3 暂停
int32_t media_position(void);         // 放到第几秒
int32_t media_duration(void);         // 总长（秒），不知道为 0
int32_t media_mine(void);             // 现在放的（或停着的）是不是自己放的
int32_t media_info(const char *path, char *buf, int32_t size);  // 读文件里的标签："标题\n歌手\n时长秒"
```

`NOMI_EV_MEDIA` 只发给放这一首的应用：

| `value` | 什么时候 | 应用该做什么 |
|---|---|---|
| `NOMI_MEDIA_FINISHED` | 这一首放完了 | 按自己的队列和模式接着放（不放就停着） |
| `NOMI_MEDIA_NEXT` / `PREV` | 主人在 `ui_player` 或收听卡片上点了下一首 / 上一首 | 在自己的队列里换 |
| `NOMI_MEDIA_PLAY` | 主人点了你的 `ui_player` 上的"放"，但现在放的不是你的 | 决定放什么，比如接着放上次的 |

SD 卡上的歌，系统自己读文件里的标签（FLAC 的 Vorbis 注释、MP3 的 ID3），用读到的歌名歌手替换显示的，还会读内嵌封面和同名的 `.lrc` 歌词。

网上的歌、节目，系统放之前先探一下：
- https 的地址先试 http，能用就走 http（不占 TLS 要的内部 RAM）。
- 按文件头认格式（扩展名、响应头常对不上，m4a 写成 .aac 之类）。
- 歌如果不是音频（下架后跳到网页之类），`media_play` 返回 `NOMI_E_NOT_FOUND`，应用好换下一首。节目探不出来也照样试着放。
- 应用放的节目，进度由应用自己记（`media_position`），系统不记。

格式：MP3、AAC、m4a、FLAC（最高 24 位 / 96kHz，只限 SD 卡）、WAV（只限 SD 卡）。

### 大模型和语音（权限 `assistant`）

**语音指令写在 manifest 的 `voice` 里**。每轮对话的系统提示会列出装好的应用各有哪些指令，大模型统一用 `self.app.command(app, command, arg)` 这一个工具去调。所以装、删应用都不用改工具列表。

```c
void    assistant_context(const char *text);        // 界面开在前台时，每轮对话带给大模型（最多 600 字节）
int32_t voice_arg(char *buf, int32_t size);         // 收到 NOMI_EV_VOICE 时：取参数
void    voice_reply(const char *text, int32_t end); // 回话；end = 1 时这句说完就回待机
```

- **回话时机**：要在处理这个 `NOMI_EV_VOICE` 的那次 `nomi_event` 里回，系统最多等 25 秒（在线点歌要搜、取歌曲页）。不回话的话，大模型收到的是"已交给「应用名」"。
- **开始放歌这类事**：回话时带 `end = 1`。不然对话还开着，声音出不来。
- **没找到、做不了**：如实回话（比如"SD 卡里没找到"），大模型会换别的办法。

例子：
- 音乐应用的 `play` 指令：`arg` 是歌名、歌手或文件夹名。在 SD 卡里搜到就放，回"好的，放「…」"。
- 小说应用可以注册「下一章」，并把当前页的文字设成上下文。用户看书时问「这段讲的什么」，大模型就知道说的是哪一段。

### 其他

```c
int64_t time_now(void);                                     // 当前时间（Unix 毫秒），没对上时间时为 0
void    timer_start(int32_t id, int32_t ms, int32_t repeat);
void    timer_stop(int32_t id);
void    keep_awake(int32_t on);                             // 要权限 keep_awake：看书时不息屏
void    nomi_log(const char *text);                         // 打到串口日志，调试用
```

### 联网（权限 `net`）

```c
// GET 一个 http(s) 地址（跟着跳转），正文最多 size 字节（够了就断开，一次最多 512KB）；返回字节数，
// 没联网 NOMI_E_NO_NET，别的失败（连不上、不是 200）NOMI_E_FAIL。最多等 20 秒，收到多少算多少
int32_t net_get(const char *url, char *buf, int32_t size);
// 同上，收到 until 出现 count 次就不往下收：RSS 动辄几 MB，只要最新 10 期就 until = "</item>"、count = 10
int32_t net_get_until(const char *url, char *buf, int32_t size, const char *until, int32_t count);
```

- 会卡住几秒，但跑在应用自己的线程里，不影响界面。
- 设备内部 RAM 只够同时开一条 https 连接：如果正在放的是 https 的流，系统先暂停它，取完再接着放。
- 拿回来的 JSON、XML、网页用 `tools/app-sdk/nomi_text.h` 里的小工具拆，见第 10 节。

### 网页（应用自带的设置页）

有些设置在设备上不好弄：要打字（搜播客、填网址），或者要列一长串。应用可以自带一个网页：

- 包里放 `assets/web/index.html`（还可以有 js、css、图片）。设备网页设置页的「设备 → 应用」里，这个应用旁边就会出现「设置」，打开的是 `http://<设备>/app/<id>/`。
- 网页里 POST 到相对地址 `call`，请求体交给应用（`NOMI_EV_WEB`）。应用回的内容（一般是 JSON，最多 32KB）原样回给网页。
- 回话要在这次 `nomi_event` 里给，系统最多等 60 秒，可以先联网搜一下再回。应用没在跑时，系统先在后台把它拉起来。

```c
int32_t web_body(char *buf, int32_t size);  // 收到 NOMI_EV_WEB 时：取请求体
void    web_reply(const char *text);         // 回给网页
```

例子：音乐应用的网页管播客订阅（搜索、订阅、取消、放最新一期）和在线音乐的网站地址。

## 6. 权限

| 权限 | 能做什么 |
|---|---|
| `sd.read` | 读 SD 卡上的文件（只读） |
| `media` | 让系统播放器放歌、控制播放 |
| `assistant` | 给大模型加上下文、注册语音指令、让它念出来 |
| `keep_awake` | 打开时不息屏 |
| `net` | 联网（`net_get` `net_get_until`） |

没写在 `permissions` 里的接口，调了返回 `NOMI_E_DENIED`。

## 7. 安装、更新、卸载

在「应用管理」里操作，以后网页设置页也能做：

1. **从哪装**：SD 卡上的 `.npk` 文件（放在卡里的「安装包」文件夹），或者在网页设置页上传。
2. **装之前给用户看**：名字、版本、大小、要的权限。固件版本不够时提示先升级固件。
3. **选装在哪**：机器内部 / SD 卡（见第 8 节），默认机器内部。
4. 解包、检查 `app.wasm` 能不能加载，然后在桌面上出现。

- **更新**：装同一个 `id` 的新版本，覆盖程序，**保留数据**。
- **卸载**：删程序和数据，删之前确认一次。
- **预装应用**：跟着固件走，包（`.npk`）打在资源分区里（板子目录的 `preinstall/`）。开机时，资源分区里的包比机器里装着的新、或者还没装，就装进机器内部的应用分区；SD 卡上装过同一个应用的，把它的 `data/` 搬过来，再删掉卡上那份。现在不能删（应用管理和网页上都没有删除）；从网页装同一个 id 的新版本能盖过它，下次固件里的版本更新时又换回来。
  - 改了预装应用要重新打包放进去：`python tools/app-sdk/build.py apps/music -o firmware/main/boards/spotpear/spotpear-esp32s3-max35/preinstall/nomi.music.npk`（固件编译时不打包：IDF 的 Python 环境里没有 zig），在 `nomi_pkg.cc` 的 `kPreinstalled` 里登记。

## 8. 安装位置和存储管理

| | 机器内部 | SD 卡 |
|---|---|---|
| 放在 | 应用分区 `apps`（闪存里 2MB 的 FAT，从资源分区匀出来；挂在 `/int`，`nomi/apps/<id>/`） | `/sd/nomi/apps/<id>/` |
| 拔卡后 | 照常能用 | 灰着留在桌面上（开机就没插卡也是），点了提示插卡；插回来后桌面一翻到就恢复，点灰着的那个会顺手打开它 |
| 适合 | 预装应用、常用的小应用 | 大应用、不常用的 |

装到哪（现在还不能自己选）：装过的新版本装回原处；新的应用插着 SD 卡就装卡上，没插装机器里。

每个应用目录里是 `manifest.json`、`app.wasm`、`assets/`、`data/`。数据和程序放在一起，挪位置、卸载都一起走。

**存储管理**是应用管理里的「存储」页：

- 机器内部应用区、SD 卡的总量、已用、剩余。
- 按应用列出：装在哪、程序多大、数据多大。
- 能做的：清除数据、挪到 SD 卡或机器内部、卸载。

改分区表不会动设置和用户数据（NVS 的位置、大小不变），见 [flash-partitions.md](flash-partitions.md)。

## 9. 限制

| 项 | 限制 |
|---|---|
| `app.wasm` 大小 | 1MB |
| 运行内存 | `memory_kb`，最大 1MB（PSRAM）。常驻的应用要省着用：PSRAM 总共就剩 2MB 左右，播放器放长 m4a 节目时还要 1MB 上下 |
| 一次事件处理 | 2 秒 |
| 同时运行 | 4 个应用（前台一个，其余在后台） |
| 语音指令回话 | 25 秒 |
| 网页请求回话 | 60 秒 |
| 一次读文件 | 32KB |

## 10. 开发工具

先只支持 C 写，用和模拟器一样的 zig 工具链编译。仓库里会放：

```
tools/app-sdk/
  nomi_sdk.h      接口声明、常量、几个小工具函数（字符串、格式化，不带完整的 C 库）
  nomi_text.h     处理文字的小工具（纯 C）：查找、拷贝、XML / HTML 字段、JSON 字段、URL 编码、base64、哈希
  build.py        编译 + 打成 .npk：python build.py 应用目录（目录里所有 .c 一起编）
  examples/       示例应用
```

编译大致是这样（`build.py` 替你调；不带调试信息，不然 app.wasm 要大两三倍）：

```
zig cc -target wasm32-freestanding -O2 -g0 -nostdlib \
  -Wl,--no-entry -Wl,--strip-all -Wl,-z,stack-size=16384 \
  -Wl,--initial-memory=<memory_kb> -Wl,--max-memory=<memory_kb> \
  -o app.wasm *.c
```

**调试**：浏览器模拟器也能装 `.npk`。它用浏览器自带的 WebAssembly 跑应用，接口和设备上是同一套，改完不用刷机就能看效果。

### 最小示例

```c
#include "nomi_sdk.h"

static int32_t g_count, g_label, g_button;

void nomi_main(void)
{
    ui_page("计数器");
    g_count = kv_get_int("count", 0);  // 上次点到几
    g_label = ui_text("", NOMI_TEXT_TITLE);
    g_button = ui_button("点我", 1);
    nomi_event(NOMI_EV_CHANGE, 0, 0);  // 先把数字显示出来
}

int32_t nomi_event(int32_t type, int32_t id, int32_t value)
{
    if (type == NOMI_EV_CLICK && id == g_button) {
        g_count++;
        kv_set_int("count", g_count);
    } else if (type != NOMI_EV_CHANGE) {
        return 0;
    }
    char text[32];
    nomi_fmt(text, sizeof(text), "点了 %d 次", g_count);
    ui_set_text(g_label, text);
    return 1;
}
```

## 11. 设备上的实现（给固件开发看）

- 运行时：乐鑫组件 `espressif/wasm-micro-runtime`，先用解释执行。
- 系统接口：用 `wasm_runtime_register_natives("env", …)` 注册。字符串参数签名写 `$`，缓冲区写 `*~`，运行时会自动把应用里的地址换成设备地址并检查越界。
- 调应用：用 `wasm_runtime_lookup_function` 找到 `nomi_main` / `nomi_event`，用 `wasm_runtime_call_wasm` 调。不让应用传函数指针当回调，全部走 `nomi_event`。
- 线程：
  - 每个进程一个 pthread（栈 32KB，放 PSRAM），有自己的模块实例和事件队列。系统另有一个任务，做开机扫描之类的活。
  - `ui_*` 调用拿显示锁去改界面。拿到锁先看界面还在不在，不在就什么都不做，所以关界面不用等应用。
  - 进程对象用 `shared_ptr` 管理；界面回调持有的是 `weak_ptr`，进程没了再点也不会出事。
  - 每 5 秒检查一遍：在后台、又没理由留着的进程，30 秒后结束。
  - 2 秒超时（待做）靠这个任务的看门狗来实现。
- 内部闪存：读写闪存时 cache 关掉、PSRAM 访问不了，栈在 PSRAM 的任务（应用进程、系统任务）一碰 `/int` 就断言重启。它们的读写（读程序、读写 kv、`data:/` `pkg:/` 的文件）都交给 `NomiFlashIo`：一个栈在内部 RAM 的小任务（6KB）替它们做；调用方自己的栈在内部 RAM（网页服务器）就直接做。
- 播放：
  - `NomiRadio::Station.owner` 记着是哪个应用放的。
  - 放完时，`on_finished` 把 `NOMI_MEDIA_FINISHED` 送给这个应用。
  - 收听卡片和 `ui_player` 上的上一首 / 下一首，经 `NomiWasm::MediaCommand` 送给它。
  - 系统自己放的电台、播客（`owner` 为空），这两个按钮还是换台、快进快退。
- 大模型：
  - `self.app.command` 在主循环里调 `NomiWasm::VoiceCommand`，送出 `NOMI_EV_VOICE` 后等应用处理完。
  - 指令列表由 `NomiPkg::PromptContext` 每轮放进系统提示。
- 内存：运行时的堆和应用的线性内存都放 PSRAM。内部 RAM 很紧，空闲只有 55～60KB。
- 界面：`ui_*` 建的控件放进应用层（`nomi_app.c`）给的全屏容器里。页面栈、右划返回由框架管。

## 12. 待定 / 待验证

- 运行时占多少固件空间：固件现在还剩 9%，不够的话就把两个 OTA 程序槽合成一个。
- 机器内部应用区的大小和文件系统（LittleFS 还是 FAT），以及资源分区能不能安全缩小。
- 解释执行的速度够不够：主要看列表很长时的滚动、阅读器翻页。不够的话试 fast-interp 或 AOT。
- 小说要完整的中文字体：系统字体只是常用字子集，生僻字会缺，打算从 SD 卡加载完整字体文件。
- 应用包要不要签名：现在默认信任用户自己装的包。
- 网页设置页的「应用」页：列表、上传安装、卸载。（v0.1 已有：设备 → 应用，还能「打开」）
- ~~没插 SD 卡时，装在卡上的应用不出现在桌面上~~：已做（2026-11-04）。机器内部分区里记着卡上每个应用的 manifest（`/int/nomi/sdapps/<id>.json`），卡不在时照样登记、灰着；没有插卡检测脚，桌面翻到眼前（3 秒内最多一次、有卡上的应用才看）和点了灰着的应用时去挂一下卡看看。卡在但卡上没有它的也灰着（提示重新装）；卡不在时不让删卡上的应用（卡上的文件删不掉，下次插卡又冒出来）。

## 13. 实现进度（v0.2）

| 部分 | 状态 |
|---|---|
| 运行时 | ✓ wasm-micro-runtime 2.4 解释执行（fast-interp），关了 AOT、WASI、pthread 库、它自带的应用框架；固件增加约 90KB |
| 进程 | ✓ 每个应用一个进程、界面挂上 / 拿下、后台常驻（有卡片、在放歌）、闲置后结束、有事时在后台拉起；✗ 2 秒无响应自动关 |
| 界面 | ✓ `ui_page` `ui_page_close` `ui_text` `ui_item` `ui_button` `ui_set_text` `ui_set_hidden` `ui_remove` `ui_toast` `ui_player` `ui_tile` `ui_target` `ui_open`；✗ 开关、滑杆、进度条、图片、阅读器、画布 |
| 存储 | ✓ `fs_list` `fs_size` `fs_read` `kv_get` `kv_set`（`data/kv.json`）；✗ `fs_write` `fs_remove` |
| 播放 | ✓ `media_play`（本地 / 网上的歌 / 节目 / 直播，能指定从第几秒放；网上的先探格式）`media_meta` `media_control` `media_seek` `media_state` `media_position` `media_duration` `media_mine` `media_info`；放完、上一首、下一首、放都会通知应用 |
| 联网 | ✓ `net_get` `net_get_until` |
| 网页 | ✓ 应用自带设置页（`assets/web/`、`/app/<id>/`、`call` → `NOMI_EV_WEB`、`web_body` `web_reply`） |
| 大模型 | ✓ manifest 的 `voice` + `self.app.command`、`voice_arg` `voice_reply` `assistant_context`；✗ `assistant_say` |
| 其他 | ✓ `time_now` `timer_start` `timer_stop` `nomi_log`；✗ `keep_awake` |
| 事件 | ✓ 点击、返回、关闭、定时器、语音、网页、播放、SHOW / HIDE；✗ 暂停 / 恢复（浮层、睡眠） |
| 安装 | ✓ 网页上传 `.npk`、开机扫描、更新保留数据、应用管理和网页删除、机器内部的应用分区、预装应用（开机装、从 SD 卡搬数据）；✗ 自己选装在哪、挪位置、预装应用删了能恢复、从 SD 卡上的安装包装、权限确认界面 |
| 工具 | ✓ `tools/app-sdk/`：`nomi_sdk.h`、`nomi_text.h`、`build.py`（编译 + 打包，去掉调试信息）、`wasm_info.py`（看导入导出）、`examples/counter` |

| 自带应用 | ✓ `apps/music` v0.4：本地（按文件夹排队列、三种播放模式）、电台（台单在 kv 里、自带网页上增删排序、radio-browser 搜台）、播客（订阅、最新 10 期、每个播客记听到哪）、在线点歌（搜、歌词、封面）、首页卡片、自带网页（台单、订阅、音乐网站）、`play` `skip` `mode` `podcast` `subscribe` `online` `radio` 七个指令。固件里的台单 / 播客 / 在线业务已删（2026-11-03 第 ④ 步），老固件留在 NVS 里的数据不迁移（要腾地方用 `tools/erase_legacy_nvs.py` 清一次） |

源码和安装包放在哪：

- 自带应用的源码在 `apps/<名字>/`；SDK 示例在 `tools/app-sdk/examples/`。
- `build.py` 把 `.npk` 打到各自目录的 `build/` 下。`build/` 不进仓库，要用时现打。

实测（计数器示例，`app.wasm` 6.6KB）：
- 上传安装约 1～1.6 秒。
- 第一次打开 75ms，之后 8ms。
- 关掉后内存全部收回。

实测（音乐应用，`app.wasm` 47KB）：
- 开机后在后台起来，约 40～300ms。
- 按名字在 SD 卡里搜歌，搜到后开始放约 1.8 秒。
- 一首放完自动接下一首。
- 重装同一版本：旧进程先结束，新进程接着用原来的数据。

实现上踩过的坑：

- 运行时的 esp-idf 平台层假定跑在 **pthread** 里，加载应用时会调 `pthread_self()`，放在普通 FreeRTOS 任务里会断言失败、整机重启。应用任务要用 `pthread_create` 建（`esp_pthread_cfg_t.stack_alloc_caps` 让栈放 PSRAM）。
- wasm-micro-runtime 2.4 在 ESP-IDF 6.1 上编不过：平台层缺 `<sys/stat.h>`，WASI 用的 `espidf_file.c` 还用了 6.1 里没有的 `renameat`。在 `main/CMakeLists.txt` 里给它的编译目标强制带上头文件、去掉那个文件，下载来的组件不改。
- 板子构造函数里不能调 `Board::GetInstance()`（单例还没建完，再进去会自己等自己，开机卡死），显示对象由板子直接传给 `NomiWasm::Init`。
- zig 的链接器不认 `--allow-undefined`；系统接口都用 `import_module("env")` 标成导入，本来也不需要它。
