#include "nomi_display.h"

#include "application.h"
#include "board.h"
#include "display/lvgl_display/lvgl_font.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "face_script.h"
#include "nomi_ui.h"
#include "settings.h"
#include "wifi_board.h"
#if CONFIG_DIRECT_API
#include "direct_api_config_server.h"
#include "direct_api_protocol.h"
#endif

#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <nvs_flash.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define TAG "NomiDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {

constexpr const char* kNomiNs = "nomi";        // 设置页自己的设置项
constexpr const char* kDirectApiNs = "directapi";  // 人设和音色写到直连 API 的配置里

const char* const kEmotionRule =
    "回答开头必须带一个情绪标签，只能是 [happy]、[neutral]、[confused] 之一，"
    "例如：[happy]好呀，我来帮你看看。";

// 第一个人设对应直连 API 的默认提示词（NVS 里不存 prompt）
struct Persona {
    const char* name;
    const char* prompt;
};
const Persona kPersonas[] = {
    {"桌面小伙伴", nullptr},
    {"毒舌损友", "你是用户的毒舌损友，说话直接、爱吐槽但心地善良，用口语化的中文，每次不超过三句话，不要用 markdown。"},
    {"学习助手", "你是耐心的学习助手，讲解清楚、举例具体，用口语化的中文，每次不超过四句话，不要用 markdown。"},
    {"英语陪练", "你是英语口语陪练，用简单的英语和用户对话，用户说错时用一句中文温和地纠正，每次不超过三句话，不要用 markdown。"},
};
const char* const kVoices[] = {"alloy", "echo", "fable", "onyx", "nova", "shimmer"};

std::string PersonaPrompt(int index) { return std::string(kPersonas[index].prompt) + kEmotionRule; }

// ---------- 聊天历史（第二页显示用） ----------

struct ChatEntry {
    std::string role, text;
};
std::vector<ChatEntry> g_chat_log;  // 只在 LVGL 任务里访问（所有调用方都在同一线程）
constexpr size_t kChatLogMax = 100;

bool IsAsciiAlnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// 逐句下发字幕（小智一句话一条消息），同一条回答的句子接着记，别拆成好几条
void AddChatMessage(const char* role, const char* content) {
    if (content == nullptr || *content == '\0') return;
    std::string text = content;
    if (text.size() > 2000) text.resize(2000);
    if (strcmp(role, "assistant") == 0 && !g_chat_log.empty() && g_chat_log.back().role == "assistant") {
        std::string& last = g_chat_log.back().text;
        // 英文单词之间补个空格，中文直接连上
        if (!last.empty() && IsAsciiAlnum(last.back()) && IsAsciiAlnum(text.front())) last += ' ';
        last += text;
    } else {
        g_chat_log.push_back({role, std::move(text)});
    }
    if (g_chat_log.size() > kChatLogMax) {
        g_chat_log.erase(g_chat_log.begin(), g_chat_log.end() - kChatLogMax);
    }
}

int HistoryCount() { return static_cast<int>(g_chat_log.size()); }

const char* HistoryGet(int index, const char** role) {
    static const char* kEmpty = "";
    if (index < 0 || index >= static_cast<int>(g_chat_log.size())) {
        *role = "";
        return kEmpty;
    }
    *role = g_chat_log[index].role.c_str();
    return g_chat_log[index].text.c_str();
}

// 音色跟着语音合成选的供应商走：网页设置页保存时把那家的音色列表写进 tts_voices（一行一个），
// 没有就用 OpenAI 的六个
std::vector<std::string> VoiceList() {
    std::string text = Settings(kDirectApiNs).GetString("tts_voices");
    std::vector<std::string> list;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string v = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!v.empty()) list.push_back(v);
        pos = nl == std::string::npos ? text.size() : nl + 1;
    }
    if (list.empty()) list.assign(std::begin(kVoices), std::end(kVoices));
    return list;
}

// 屏幕上只显示短名：FunAudioLLM/CosyVoice2-0.5B:anna → anna
std::string VoiceLabel(const std::string& voice) {
    size_t cut = voice.find_last_of(":/");
    return cut == std::string::npos ? voice : voice.substr(cut + 1);
}

void ApplyAec(bool on) {
    Application::GetInstance().SetAecMode(on && NomiDisplay::aec_supported ? kAecOnDeviceSide : kAecOff);
}

// 以下回调在 LVGL 任务里被 nomi_ui 调用

nomi_state_t GetState() {
    switch (Application::GetInstance().GetDeviceState()) {
        case kDeviceStateListening: return NOMI_STATE_LISTENING;
        case kDeviceStateSpeaking: return NOMI_STATE_SPEAKING;
        case kDeviceStateConnecting:
        case kDeviceStateActivating:
        case kDeviceStateUpgrading: return NOMI_STATE_CONNECTING;
        case kDeviceStateFatalError: return NOMI_STATE_ERROR;
        default: return NOMI_STATE_IDLE;
    }
}

void UserActivity() {
    if (NomiDisplay::on_user_activity) NomiDisplay::on_user_activity();
}

int GetInt(const char* key, int def) {
    auto& board = Board::GetInstance();
    if (!strcmp(key, NOMI_KEY_VOLUME)) return board.GetAudioCodec()->output_volume();
    if (!strcmp(key, NOMI_KEY_BRIGHTNESS)) return board.GetBacklight()->brightness();
    if (!strcmp(key, NOMI_KEY_AEC_SUPPORTED)) return NomiDisplay::aec_supported;
    if (!strcmp(key, NOMI_KEY_PERSONA)) {
        // 按当前提示词反查是哪个预设；网页里改过就算自定义
        std::string prompt = Settings(kDirectApiNs).GetString("prompt");
        if (prompt.empty()) return 0;
        for (int i = 1; i < static_cast<int>(std::size(kPersonas)); i++) {
            if (prompt == PersonaPrompt(i)) return i;
        }
        return -1;
    }
    if (!strcmp(key, NOMI_KEY_VOICE)) {
        std::string voice = Settings(kDirectApiNs).GetString("tts_voice", "alloy");
        auto list = VoiceList();
        for (int i = 0; i < static_cast<int>(list.size()); i++) {
            if (voice == list[i]) return i;
        }
        return -1;
    }
    return Settings(kNomiNs).GetInt(key, def);
}

void SetInt(const char* key, int value, bool final) {
    auto& board = Board::GetInstance();
    if (!strcmp(key, NOMI_KEY_VOLUME)) {
        // SetOutputVolume 每次都写 NVS，拖动中不调，松手再设
        if (final) board.GetAudioCodec()->SetOutputVolume(value);
        return;
    }
    if (!strcmp(key, NOMI_KEY_BRIGHTNESS)) {
        board.GetBacklight()->SetBrightness(value, false);
        if (final) {
            // Backlight::SetBrightness 遇到相同的值会直接返回不存盘，所以松手时自己写
            Settings("display", true).SetInt("brightness", value);
        }
        return;
    }
    if (!strcmp(key, NOMI_KEY_PERSONA)) {
        Settings s(kDirectApiNs, true);
        if (value == 0) {
            s.EraseKey("prompt");
        } else if (value > 0 && value < static_cast<int>(std::size(kPersonas))) {
            s.SetString("prompt", PersonaPrompt(value));
        }
        return;
    }
    if (!strcmp(key, NOMI_KEY_VOICE)) {
        auto list = VoiceList();
        if (value >= 0 && value < static_cast<int>(list.size())) {
            Settings(kDirectApiNs, true).SetString("tts_voice", list[value]);
        }
        return;
    }

    Settings(kNomiNs, true).SetInt(key, value);
    if (!strcmp(key, NOMI_KEY_AEC)) {
        ApplyAec(value);
    } else if (!strcmp(key, NOMI_KEY_DIM) || !strcmp(key, NOMI_KEY_OFF)) {
        if (NomiDisplay::on_power_timing_changed) NomiDisplay::on_power_timing_changed();
    }
}

std::string DeviceIp() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    if (netif == nullptr || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0) {
        return "";
    }
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
    return buf;
}

const char* GetText(const char* key) {
    static std::string text;  // nomi_ui 拿到后马上用，下次调用前一直有效
    text.clear();
    if (!strcmp(key, "personas")) {
        for (const auto& p : kPersonas) text += std::string(text.empty() ? "" : "\n") + p.name;
    } else if (!strcmp(key, "voices")) {
        for (const auto& v : VoiceList()) text += std::string(text.empty() ? "" : "\n") + VoiceLabel(v);
    } else if (!strcmp(key, "wifi")) {
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            text = reinterpret_cast<const char*>(ap.ssid);
            text += "（信号 " + std::to_string(ap.rssi) + " dBm）";
        }
    } else if (!strcmp(key, "ip")) {
        text = DeviceIp();
    } else if (!strcmp(key, "config_url")) {
#if CONFIG_DIRECT_API
        text = DirectApiConfigServer::Url();
#endif
    } else if (!strcmp(key, "version")) {
        text = std::string("Pocket Companion ") + esp_app_get_description()->version;
    }
    return text.c_str();
}

void Action(const char* name) {
    auto& app = Application::GetInstance();
    if (!strcmp(name, "reconfigure_wifi")) {
        app.Schedule([]() { static_cast<WifiBoard&>(Board::GetInstance()).EnterWifiConfigMode(); });
    } else if (!strcmp(name, "factory_reset")) {
        app.Schedule([]() {
            ESP_LOGW(TAG, "Factory reset");
            nvs_flash_erase();
            esp_restart();
        });
    } else if (!strcmp(name, "test_api")) {
#if CONFIG_DIRECT_API
        nomi_ui_notify("正在测试 API…", 15000);
        // 网络请求要几秒，单开一个任务，测完用通知条显示结果
        xTaskCreate(
            [](void*) {
                std::string result = DirectApiProtocol::SelfTest();
                Board::GetInstance().GetDisplay()->ShowNotification(result, 8000);
                vTaskDelete(nullptr);
            },
            "api_test", 8192, nullptr, 3, nullptr);
#else
        nomi_ui_notify("当前固件没有开启直连 API", 4000);
#endif
    }
}

}  // namespace

bool NomiDisplay::aec_supported = false;
std::function<void()> NomiDisplay::on_user_activity;
std::function<void()> NomiDisplay::on_power_timing_changed;

NomiDisplay::NomiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                         int width, int height, int offset_x, int offset_y, bool mirror_x,
                         bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y,
                    swap_xy) {}

void NomiDisplay::GetIdleSeconds(int& dim_seconds, int& off_seconds) {
    static const int dim[] = NOMI_DIM_SECONDS;
    static const int off[] = NOMI_OFF_SECONDS;
    Settings s(kNomiNs);
    int d = s.GetInt(NOMI_KEY_DIM, 1), o = s.GetInt(NOMI_KEY_OFF, 1);
    dim_seconds = dim[d >= 0 && d < static_cast<int>(std::size(dim)) ? d : 1];
    off_seconds = off[o >= 0 && o < static_cast<int>(std::size(off)) ? o : 1];
}

void NomiDisplay::SetupUI() {
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    SpiLcdDisplay::SetupUI();

    DisplayLockGuard lock(this);
    face_buf_ = static_cast<uint16_t*>(heap_caps_malloc(width_ * height_ * sizeof(uint16_t),
                                                        MALLOC_CAP_SPIRAM));
    if (face_buf_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate face buffer, keep XiaoZhi UI");
        return;
    }

    // 小智的界面对象照常存在（它的其他代码还会更新它们），只是全部隐藏
    lv_obj_t* screen = lv_screen_active();
    for (uint32_t i = 0; i < lv_obj_get_child_count(screen); i++) {
        lv_obj_add_flag(lv_obj_get_child(screen, i), LV_OBJ_FLAG_HIDDEN);
    }

    static const nomi_ui_callbacks_t callbacks = {
        .get_state = GetState,
        .user_activity = UserActivity,
        .get_int = GetInt,
        .set_int = SetInt,
        .get_text = GetText,
        .action = Action,
        .history_count = HistoryCount,
        .history_get = HistoryGet,
    };
    // 先用内置字体，资源分区里的完整中文字体加载后会经 SetTextFont 换上
    nomi_ui_create(screen, face_buf_, &BUILTIN_TEXT_FONT, &callbacks);
    ui_ready_ = true;

    // 恢复"随时插话打断"；放到主循环里，等音频编解码器确定有没有回采之后再设
    if (Settings(kNomiNs).GetInt(NOMI_KEY_AEC, 0)) {
        Application::GetInstance().Schedule([]() { ApplyAec(true); });
    }
}

bool NomiDisplay::SetTextFont(std::shared_ptr<LvglFont> text_font) {
    bool ok = SpiLcdDisplay::SetTextFont(text_font);
    if (ok && ui_ready_ && text_font && text_font->font()) {
        DisplayLockGuard lock(this);
        nomi_ui_set_font(text_font->font());
        ESP_LOGI(TAG, "Nomi UI switched to assets text font");
    }
    return ok;
}

void NomiDisplay::SetStatus(const char* status) {
    SpiLcdDisplay::SetStatus(status);
    if (!ui_ready_) return;
    DisplayLockGuard lock(this);
    nomi_ui_set_status(status);
}

void NomiDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!ui_ready_) {
        SpiLcdDisplay::ShowNotification(notification, duration_ms);
        return;
    }
    DisplayLockGuard lock(this);
    nomi_ui_notify(notification, duration_ms);
}

void NomiDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void NomiDisplay::SetEmotion(const char* emotion) {
    if (!ui_ready_) {
        SpiLcdDisplay::SetEmotion(emotion);
        return;
    }
    DisplayLockGuard lock(this);
    nomi_ui_set_emotion(emotion);
}

void NomiDisplay::SetChatMessage(const char* role, const char* content) {
    SpiLcdDisplay::SetChatMessage(role, content);
    if (!ui_ready_) return;
    DisplayLockGuard lock(this);
    // 系统消息（请稍候、错误提示等）用通知条，字幕只留给对话
    if (role != nullptr && strcmp(role, "system") == 0) {
        if (content != nullptr && *content) nomi_ui_notify(content, 4000);
        return;
    }
    // 推送通知的字幕不进聊天历史
    if (Application::GetInstance().GetDeviceState() != kDeviceStateNotifying) {
        AddChatMessage(role ? role : "", content);
    }
    nomi_ui_set_subtitle(role, content);
}

void NomiDisplay::UpdateStatusBar(bool update_all) {
    SpiLcdDisplay::UpdateStatusBar(update_all);
    if (!ui_ready_) return;
    int level = -1;
    bool charging = false, discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        level = -1;
    }
    DisplayLockGuard lock(this);
    nomi_ui_set_battery(level, charging);
}

void NomiDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    SpiLcdDisplay::SetPreviewImage(std::move(image));
    if (!ui_ready_ || preview_image_ == nullptr) return;
    // 小智的预览图在我们的界面下面，提到最上层；它自己的定时器到时会再隐藏
    DisplayLockGuard lock(this);
    lv_obj_move_foreground(preview_image_);
}

int NomiDisplay::GetSavedInt(const char* key, int def) { return Settings(kNomiNs).GetInt(key, def); }

bool NomiDisplay::PlayFace(const std::string& json, std::string& error) {
    if (!ui_ready_) {
        error = "屏幕还没准备好";
        return false;
    }
    DisplayLockGuard lock(this);
    char err[160];
    if (!nomi_ui_play_face(json.c_str(), err, sizeof(err))) {
        error = err;
        return false;
    }
    return true;
}

std::string NomiDisplay::FacePrompt() {
    return std::string(
               "你有一张会动的脸：黑底白色线条，两只眼睛加一双小手。可以在情绪标签后面紧跟一段表情脚本 "
               "<face>JSON</face> 配合你说的话，不需要就不写，别每句都写。\n"
               "JSON 是 1～6 步的数组，每步字段都可选：dur 秒（0.3-4）；eyes：capsule 平常｜arc 笑眯眯｜ring 睁大的空心眼｜"
               "dash 平静｜dot 小眼｜closed 闭眼｜heart｜star｜x｜shades 墨镜｜spiral 转圈的晕；look [x,y] 视线（-1~1）；"
               "tilt 歪头（-1~1）；brow：up 挑眉｜down 皱眉；sweat：true 汗滴；blush：0-1 腮红；"
               "hands：none｜chin 托腮｜ear 手放耳边｜wave 挥手｜up 举双手｜cover 捂眼｜mic 拿麦克风｜cheeks 双手托腮；"
               "motion：nod 点头｜bounce 蹦｜shake 摇头｜sway 摇摆；"
               "prop 道具：{\"icon\":图标,\"anim\":\"bob|swing|spin|float|pop|orbit 绕着转\",\"at\":\"mouth|left|right|top\"}，图标有 ") +
           face_script_icon_names() +
           "；没有合适的图标就用 \"strokes\" 画白色线条：坐标 0-100，[x1,y1,x2,y2,…] 是折线，[x,y,r] 是圆，"
           "r 为负是实心点，最多 12 笔。\n"
           "例：[happy]<face>[{\"eyes\":\"ring\",\"brow\":\"up\",\"dur\":0.8},{\"eyes\":\"arc\",\"hands\":\"up\","
           "\"motion\":\"bounce\",\"prop\":{\"icon\":\"star\",\"anim\":\"pop\",\"at\":\"top\"},\"dur\":2}]</face>太好啦！";
}

void NomiDisplay::SetSleep(bool sleeping) {
    if (!ui_ready_) return;
    DisplayLockGuard lock(this);
    nomi_ui_set_sleep(sleeping);
}
