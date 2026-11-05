#include "nomi_system.h"

#include "application.h"
#include "mcp_server.h"
#include "nomi_clock.h"
#include "nomi_display.h"
#if CONFIG_DIRECT_API
#include "direct_api_config_server.h"
#include "direct_api_protocol.h"
#endif
#if CONFIG_NOMI_SDCARD
#include "nomi_files.h"
#include "nomi_filesapp.h"
#include "nomi_sdcard.h"
#endif
#if CONFIG_NOMI_APPS
#include "nomi_open.h"
#include "nomi_pkg.h"
#include "nomi_wasm.h"
#endif
#if CONFIG_NOMI_PLAYER
#include "nomi_music.h"
#include "nomi_radio.h"
#endif
#if CONFIG_NOMI_PHOTO
#include "nomi_photo.h"
#endif
#if CONFIG_NOMI_CAMERA
#include "nomi_camera.h"
#endif
#if CONFIG_NOMI_PHONE
#include "nomi_phone.h"
#endif
#if CONFIG_NOMI_HOTSPOT
#include "nomi_hotspot.h"
#endif

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string>

namespace {

NomiDisplay* s_display = nullptr;
NomiSystem::Hooks s_hooks;

void WakeScreen() {
    if (s_hooks.wake_screen) s_hooks.wake_screen();
}

void RegisterTools() {
    auto& mcp_server = McpServer::GetInstance();
    // 表情脚本：主人明确要表演时大模型调这个工具；平时它在回复开头写 <face>…</face>（见 NomiDisplay::FacePrompt）
    mcp_server.AddTool("self.face.play",
                       "让你的脸表演一段动作。主人明确要你做表情、做鬼脸、跳舞、弹吉他之类时用；"
                       "script 是表情脚本 JSON（步骤数组，格式和回复里的 <face> 一样）。",
                       PropertyList({Property("script", kPropertyTypeString)}),
                       [](const PropertyList& properties) -> ToolResult {
                           std::string error;
                           if (!s_display->PlayFace(properties["script"].value<std::string>(), error)) {
                               return std::unexpected(error);
                           }
                           return std::string("正在表演");
                       });
    // 时钟页：闹钟、倒计时提醒、天气城市
    auto& clock = NomiClock::GetInstance();
    clock.RegisterTools();
    clock.on_ring_start = [](const std::string& time, const std::string& label) {
#if CONFIG_NOMI_PLAYER
        NomiRadio::GetInstance().SetAlarmHold(true);  // 在放的先停，别和铃声混在一起
#endif
        WakeScreen();  // 睡着 / 息屏也要亮起来
        s_display->ShowAlarm(time, label);
    };
    clock.on_ring_stop = []() {
        s_display->HideAlarm();
#if CONFIG_NOMI_PLAYER
        NomiRadio::GetInstance().SetAlarmHold(false);
#endif
    };
#if CONFIG_NOMI_PLAYER
    // 播放器：系统只留「停 / 暂停」这一个工具，放什么、换哪个都归应用（self.app.command）
    NomiRadio::GetInstance().RegisterTools();
#endif
#if CONFIG_NOMI_APPS
    // 应用包的指令：哪些应用有哪些指令写在每轮的系统提示里（NomiPkg::PromptContext），这里只一个通用入口，
    // 装、删应用不用改工具列表
    mcp_server.AddTool(
        "self.app.command",
        "让装好的应用做事（比如音乐应用放歌）。能用的 app、command 见系统提示里「装好的应用能做的事」；"
        "arg 按指令说明填，没有就空字符串。",
        PropertyList({Property("app", kPropertyTypeString), Property("command", kPropertyTypeString),
                      Property("arg", kPropertyTypeString, std::string("")).SetMaxLength(200)}),
        [](const PropertyList& p) -> ToolResult {
            bool end = false;
            std::string r = NomiWasm::VoiceCommand(p["app"].value<std::string>(), p["command"].value<std::string>(),
                                                   p["arg"].value<std::string>(), &end);
            if (!r.empty() && r[0] == '!') return std::unexpected(r.substr(1));
#if CONFIG_DIRECT_API
            if (end) DirectApiProtocol::ExitAfterReply();  // 开始放了：这句说完就回待机，声音才出得来
#endif
            return r;
        });
#endif
#if CONFIG_NOMI_CAMERA && CONFIG_DIRECT_API
    // 让大模型看一眼摄像头：拍照和编码要一两秒，放到独立任务里做，别卡主循环；
    // 拍好的 JPEG 经 AttachImage 交给直连协议，由它随下一轮请求发给视觉模型（见 Chat()）
    mcp_server.AddTool("self.camera.look",
                       "看一眼摄像头拍到的画面并说出看到了什么。主人让你看东西、认东西、读画面上的文字、"
                       "描述眼前的场景时调用；question 写上你想从画面里知道的问题。",
                       PropertyList({Property("question", kPropertyTypeString)}),
                       [](const PropertyList& properties) -> ToolResult {
                           (void)properties;
                           if (xTaskCreate(
                                   [](void* arg) {
                                       (void)arg;
                                       std::string jpeg = NomiCamera::Capture();
                                       if (!jpeg.empty()) {
                                           DirectApiProtocol::AttachImage(std::move(jpeg));
                                       }
                                       vTaskDelete(nullptr);
                                   },
                                   "cam_look", 8192, nullptr, 3, nullptr) != pdPASS) {
                               return std::unexpected("相机忙，稍后再试");
                           }
                           return std::string("已拍下画面，照片随后作为用户消息发给你");
                       });
#endif
}

#if CONFIG_DIRECT_API
void ConnectDirectApi() {
    // 每轮对话的系统提示词里带上现在几点、天气、已设的闹钟（设"明早 7 点"要知道今天是哪天）、
    // 装好的应用能做什么（放歌、电台、播客都在里面）
    DirectApiProtocol::context_provider = []() {
        std::string context = NomiClock::GetInstance().PromptContext();
#if CONFIG_NOMI_APPS
        context += "\n" + NomiPkg::GetInstance().PromptContext() + "\n" + NomiWasm::PromptContext();
#endif
        return context;
    };
    DirectApiConfigServer::on_city_changed = []() { NomiClock::GetInstance().RefreshWeather(); };
    DirectApiProtocol::face_prompt = NomiDisplay::FacePrompt();
    DirectApiProtocol::face_handler = [](const std::string& json, std::string& error) {
        return s_display->PlayFace(json, error);
    };
#if CONFIG_NOMI_APPS
    // 网页设置页的「应用」卡片：上传 .npk 安装、列出、删除
    DirectApiConfigServer::AddUploadHandler("/apps/install",
                                            [](size_t length, const std::function<int(char*, size_t)>& read) {
                                                return NomiPkg::GetInstance().InstallJson(length, read);
                                            });
    DirectApiConfigServer::AddJsonHandler("/apps.json", false,
                                          [](const std::string&) { return NomiPkg::GetInstance().ListJson(); });
    DirectApiConfigServer::AddJsonHandler("/apps/remove", true, [](const std::string& body) {
        return NomiPkg::GetInstance().RemoveJson(body);
    });
    DirectApiConfigServer::AddJsonHandler("/apps/open", true, [](const std::string& body) {
        return NomiPkg::GetInstance().OpenJson(body);
    });
    // 应用自带的网页设置页：/app/<id>/ 是包里的 assets/web/，POST /app/<id>/call 交给应用
    DirectApiConfigServer::AddPrefixHandler(
        "/app/", [](const std::string& uri, bool post, const std::string& body, std::string& type, std::string& out) {
            return NomiPkg::GetInstance().WebRequest(uri, post, body, type, out);
        });
    DirectApiConfigServer::AddJsonHandler("/sd/open", true, NomiOpen::OpenJson);  // 在设备上打开
#endif
#if CONFIG_NOMI_SDCARD
    // 网页设置页的「SD 卡」卡片：每次读都当场检测
    DirectApiConfigServer::AddJsonHandler("/sd.json", false,
                                          [](const std::string&) { return NomiSdCard::GetInstance().StatusJson(); });
    // 网页的文件管理：列、新建文件夹、改名、删除、下载（边读边发）
    DirectApiConfigServer::AddJsonHandler("/sd/list", true, NomiFiles::ListJson);
    DirectApiConfigServer::AddJsonHandler("/sd/mkdir", true, NomiFiles::MkdirJson);
    DirectApiConfigServer::AddJsonHandler("/sd/rename", true, NomiFiles::RenameJson);
    DirectApiConfigServer::AddJsonHandler("/sd/delete", true, NomiFiles::DeleteJson);
    DirectApiConfigServer::AddStreamHandler(
        "/sd/file/", [](const std::string& uri, const DirectApiConfigServer::StreamBegin& begin,
                        const DirectApiConfigServer::StreamWrite& write) { return NomiFiles::Download(uri, begin, write); });
    // 往卡上传文件（字体、书）：请求体第一行是卡上的路径
    DirectApiConfigServer::AddUploadHandler("/sd/upload",
                                            [](size_t length, const std::function<int(char*, size_t)>& read) {
                                                return NomiSdCard::GetInstance().UploadJson(length, read);
                                            });
#endif
#if CONFIG_NOMI_PHOTO
    // 卡上的图片当场解成 BMP（网页预览用）：/sd/preview/<宽>x<高>/<路径>
    DirectApiConfigServer::AddPrefixHandler(
        "/sd/preview/", [](const std::string& uri, bool, const std::string&, std::string& type, std::string& out) {
            return NomiPhoto::PreviewBmp(uri, type, out);
        });
#endif
#if CONFIG_NOMI_CAMERA
    // 应用的「问它」（camera_ask）：照片挂上，待机就开始对话、在说话就打断转去听、已经在听就跟着这一轮发
    NomiCamera::ask_handler = [](std::string jpeg, const std::string& question) {
        DirectApiProtocol::AttachImage(std::move(jpeg), true, question);
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state == kDeviceStateIdle) app.ToggleChatState();
            else if (state == kDeviceStateSpeaking) app.AbortSpeaking(kAbortReasonNone);
        });
        return true;
    };
#endif
}
#endif  // CONFIG_DIRECT_API

}  // namespace

namespace NomiSystem {

void Start(NomiDisplay* display, const Hooks& hooks) {
    s_display = display;
    s_hooks = hooks;
    RegisterTools();
#if CONFIG_DIRECT_API
    ConnectDirectApi();
#endif

    NomiClock::GetInstance().Start();  // 时区、闹钟；联网后自己对时、拉天气
#if CONFIG_NOMI_PLAYER
    NomiRadio::GetInstance().Start();  // 收流 / 解码任务（平时睡着）；放什么由应用决定
    NomiMusic::GetInstance().Start();  // 下封面的后台任务
    // 放歌不算忙：待机时后台还在放（电台、歌、节目）不挡睡着——屏幕按设置变时钟/表情、
    // 调暗、息屏，歌继续放；摸一下屏幕照常回原来的页面
    Application::GetInstance().SetPlaybackAllowsSleep([]() { return NomiRadio::GetInstance().Wanted(); });
#endif
#if CONFIG_NOMI_APPS
    NomiWasm::Init(display);  // 应用包的运行时（docs/app-sdk.md）
#endif
#if CONFIG_NOMI_PHOTO
    NomiPhoto::Start(display);  // 后台解图（缩略图、看图、电子相框）
#endif
#if CONFIG_NOMI_SDCARD
    {
        DisplayLockGuard lock(display);
        NomiFilesApp::Register();  // 系统应用「文件」
    }
#endif
#if CONFIG_NOMI_APPS
    NomiOpen::Start();               // 记住的「用什么打开」
    NomiPkg::GetInstance().Start();  // 预装应用、迁老数据、扫装好的应用包，登记到桌面
#endif
#if CONFIG_NOMI_PHONE
    NomiPhone::Start();  // 蓝牙连手机（Gadgetbridge）
#endif
}

void OnNetworkStarted() {
#if CONFIG_NOMI_HOTSPOT
    NomiHotspot::Init();
#endif
}

bool KeepBright() {
#if CONFIG_NOMI_PHOTO
    return NomiPhoto::FrameActive();
#else
    return false;
#endif
}

}  // namespace NomiSystem
