# 应用开发规范（草案 v0.1）

Pocket Companion 的应用像手机 APK 一样能装、能删、能更新，不用重刷固件。这份文档说明应用包长什么样、应用怎么写、能调哪些系统接口、装在哪、怎么管理。

> 状态：v0.1 部分实现（2026-11-03 上机跑通计数器示例）。已实现的接口见第 13 节，其余是计划；标「待验证」的地方上机测过才定。

## 1. 分工

| 部分 | 在哪 | 能不能删 |
|---|---|---|
| 系统：表盘、左右卡片（历史、收听、时钟）、下拉设置、对话浮层、桌面、「应用管理」 | 固件 | 不能，跟着固件升级 |
| 预装应用：音乐、小说、图片、地图 | 固件的资源分区（只读） | 能删（实际是藏起来），能在应用管理里恢复 |
| 自己装的应用 | 机器内部的应用分区，或 SD 卡（装的时候选） | 随便装、随便删 |

**应用只管界面和逻辑**：显示什么、点了怎么办、语音指令怎么响应。**重活都调系统接口**：读 SD 卡、解码图片和音乐、播放、联网、大模型。这些在固件里用原生代码跑，应用这边只是一行调用。

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
  "memory_kb": 512
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

## 3. 生命周期

同一时间只运行一个应用。应用导出两个函数，系统只通过它们调应用：

```c
void    nomi_main(void);                                        // 打开时调一次：建第一页
int32_t nomi_event(int32_t type, int32_t id, int32_t value);   // 其他所有事情；处理了返回 1
```

| 事件 `type` | 什么时候 | `id` / `value` |
|---|---|---|
| `NOMI_EV_CLICK` | 点了按钮、列表项 | 控件 id / 0 |
| `NOMI_EV_CHANGE` | 开关、滑杆变了 | 控件 id / 新值 |
| `NOMI_EV_BACK` | 左边缘右划退了一页 | 0 / 剩几页。系统先关掉当前页再通知应用；已经是第一页时右划直接关掉应用（收到 `NOMI_EV_CLOSE`）。应用拦不住返回：那样界面得等应用回话，应用卡住时界面也跟着卡 |
| `NOMI_EV_PAUSE` / `NOMI_EV_RESUME` | 对话浮层升起 / 收起、睡着 / 醒来 | — |
| `NOMI_EV_CLOSE` | 要关了（用户退出、装新版本、内存不够） | — 。趁这时候把进度存好 |
| `NOMI_EV_TIMER` | 定时器到点 | 定时器 id |
| `NOMI_EV_VOICE` | 用户说了这个应用注册的语音指令 | 指令 id / 0 |
| `NOMI_EV_READER` | 阅读器翻页 | 阅读器 id / 当前读到的字节位置 |
| `NOMI_EV_MEDIA` | 播放状态变了 | 0 / 新状态 |

规矩：

- **一次事件处理不能超过 2 秒**，超过就算「应用无响应」，系统关掉它。耗时的事交给系统接口做。
- 应用跑在自己的任务里，不占界面任务，应用卡住时界面照常能滑、能返回。
- **退出就是关掉**：回到桌面时应用被关掉、内存收回，进度要自己用 `kv_set` 存，下次 `nomi_main` 读回来接着显示。只有音乐这种交给系统播放器的，退出后还能接着放。

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
```

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

交给系统播放器放，收听卡片、表情页小条、下拉设置里都能看到、能控制，退出应用也接着放。

```c
int32_t media_play(const char *path, const char *title, const char *artist);  // sd:/ 文件或 http(s) 地址
int32_t media_queue(const char *path, const char *title, const char *artist);  // 加到播放队列末尾
void    media_control(int32_t cmd);   // NOMI_MEDIA_PAUSE / RESUME / NEXT / PREV / STOP
void    media_seek(int32_t second);
int32_t media_state(void);            // 0 没在放、1 连接中、2 在放、3 暂停
```

格式：现在能放 MP3、AAC、m4a；FLAC、WAV 要给系统加解码器（待做，音乐应用会用到）。

### 大模型和语音（权限 `assistant`）

```c
void    assistant_context(const char *text);  // 每轮对话带给大模型：比如小说当前这一页、正在看的照片说明
int32_t voice_command(const char *name, const char *description);  // 注册语音指令，触发时收到 NOMI_EV_VOICE
void    assistant_say(const char *text);      // 用语音读出来（走语音合成）
```

比如小说应用注册「下一章」「从头读」，把当前页文字设成上下文。用户在看书时问「这段讲的什么」，大模型就知道在说哪一段。

### 其他

```c
int64_t time_now(void);                                     // 当前时间（Unix 毫秒），没对上时间时为 0
void    timer_start(int32_t id, int32_t ms, int32_t repeat);
void    timer_stop(int32_t id);
void    keep_awake(int32_t on);                             // 要权限 keep_awake：看书时不息屏
void    nomi_log(const char *text);                         // 打到串口日志，调试用
```

联网（`net.get`，权限 `net`）放到下一版：先看几个应用实际需要什么再定。

## 6. 权限

| 权限 | 能做什么 |
|---|---|
| `sd.read` | 读 SD 卡上的文件（只读） |
| `media` | 让系统播放器放歌、控制播放 |
| `assistant` | 给大模型加上下文、注册语音指令、让它念出来 |
| `keep_awake` | 打开时不息屏 |
| `net` | 联网（下一版） |

没写在 `permissions` 里的接口，调了返回 `NOMI_E_DENIED`。

## 7. 安装、更新、卸载

在「应用管理」里操作，以后网页设置页也能做：

1. **从哪装**：SD 卡上的 `.npk` 文件（放在卡里的「安装包」文件夹），或者在网页设置页上传。
2. **装之前给用户看**：名字、版本、大小、要的权限。固件版本不够时提示先升级固件。
3. **选装在哪**：机器内部 / SD 卡（见第 8 节），默认机器内部。
4. 解包、检查 `app.wasm` 能不能加载，然后在桌面上出现。

- **更新**：装同一个 `id` 的新版本，覆盖程序，**保留数据**。
- **卸载**：删程序和数据，删之前确认一次。
- **预装应用**：在资源分区里只读。「卸载」只是从桌面藏起来，数据照删，应用管理里能恢复。也能装更新的版本盖过预装的那个。

## 8. 安装位置和存储管理

| | 机器内部 | SD 卡 |
|---|---|---|
| 放在 | 新加的应用分区（从资源分区没用的那部分匀出来，暂定 3MB，文件系统待验证） | `sd:/nomi/apps/<id>/` |
| 拔卡后 | 照常能用 | 桌面图标变灰，点开提示「SD 卡没插」 |
| 适合 | 常用的小应用 | 大应用、不常用的 |

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
| 运行内存 | `memory_kb`，最大 1MB（PSRAM） |
| 一次事件处理 | 2 秒 |
| 同时运行 | 1 个应用 |
| 一次读文件 | 32KB |

## 10. 开发工具

先只支持 C 写，用和模拟器一样的 zig 工具链编译。仓库里会放：

```
tools/app-sdk/
  nomi_sdk.h      接口声明、常量、几个小工具函数（字符串、格式化，不带完整的 C 库）
  build.py        编译 + 打成 .npk：python build.py 应用目录
  examples/       示例应用
```

编译大致是这样（`build.py` 替你调）：

```
zig cc -target wasm32-freestanding -O2 -nostdlib \
  -Wl,--no-entry -Wl,--export=nomi_main -Wl,--export=nomi_event -Wl,--allow-undefined \
  -o app.wasm app.c
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
  - 应用跑在自己的任务里，事件经队列送进去。
  - `ui_*` 调用拿显示锁去改界面，不在界面任务里跑应用代码。
  - 2 秒超时靠这个任务的看门狗来实现。
- 内存：运行时的堆和应用的线性内存都放 PSRAM。内部 RAM 很紧，空闲只有 55～60KB。
- 界面：`ui_*` 建的控件放进应用层（`nomi_app.c`）给的全屏容器里。页面栈、右划返回由框架管。

## 12. 待定 / 待验证

- 运行时占多少固件空间：固件现在还剩 9%，不够的话就把两个 OTA 程序槽合成一个。
- 机器内部应用区的大小和文件系统（LittleFS 还是 FAT），以及资源分区能不能安全缩小。
- 解释执行的速度够不够：主要看列表很长时的滚动、阅读器翻页。不够的话试 fast-interp 或 AOT。
- 小说要完整的中文字体：系统字体只是常用字子集，生僻字会缺，打算从 SD 卡加载完整字体文件。
- 应用包要不要签名：现在默认信任用户自己装的包。
- 网页设置页的「应用」页：列表、上传安装、卸载。（v0.1 已有：设备 → 应用，还能「打开」）
- 没插 SD 卡时，装在卡上的应用现在不出现在桌面上；规范要的是图标变灰，要在机器内部存一份已装应用的索引。

## 13. 实现进度（v0.1）

| 部分 | 状态 |
|---|---|
| 运行时 | ✓ wasm-micro-runtime 2.4 解释执行（fast-interp），关了 AOT、WASI、pthread 库、它自带的应用框架；固件增加约 90KB |
| 界面 | ✓ `ui_page` `ui_page_close` `ui_text` `ui_item` `ui_button` `ui_set_text` `ui_set_hidden` `ui_remove` `ui_toast`；✗ 开关、滑杆、进度条、图片、阅读器、画布 |
| 存储 | ✓ `fs_list` `fs_size` `fs_read` `kv_get` `kv_set`（`data/kv.json`）；✗ `fs_write` `fs_remove` |
| 其他 | ✓ `time_now` `timer_start` `timer_stop` `nomi_log`；✗ 播放、大模型和语音、`keep_awake` |
| 事件 | ✓ 点击、返回、关闭、定时器；✗ 暂停 / 恢复（浮层、睡眠）、2 秒无响应自动关 |
| 安装 | ✓ 网页上传 `.npk`、开机扫描、更新保留数据、应用管理和网页删除；✗ 装到机器内部、从 SD 卡上的安装包装、权限确认界面 |
| 工具 | ✓ `tools/app-sdk/`：`nomi_sdk.h`、`build.py`（编译 + 打包）、`wasm_info.py`（看导入导出）、`examples/counter` |

实测（计数器示例，`app.wasm` 6.6KB）：上传安装约 1～1.6 秒；第一次打开 75ms，之后 8ms；关掉后内存全部收回。

实现上踩过的坑：

- 运行时的 esp-idf 平台层假定跑在 **pthread** 里，加载应用时会调 `pthread_self()`，放在普通 FreeRTOS 任务里会断言失败、整机重启。应用任务要用 `pthread_create` 建（`esp_pthread_cfg_t.stack_alloc_caps` 让栈放 PSRAM）。
- wasm-micro-runtime 2.4 在 ESP-IDF 6.1 上编不过：平台层缺 `<sys/stat.h>`，WASI 用的 `espidf_file.c` 还用了 6.1 里没有的 `renameat`。在 `main/CMakeLists.txt` 里给它的编译目标强制带上头文件、去掉那个文件，下载来的组件不改。
- 板子构造函数里不能调 `Board::GetInstance()`（单例还没建完，再进去会自己等自己，开机卡死），显示对象由板子直接传给 `NomiWasm::Init`。
- zig 的链接器不认 `--allow-undefined`；系统接口都用 `import_module("env")` 标成导入，本来也不需要它。
