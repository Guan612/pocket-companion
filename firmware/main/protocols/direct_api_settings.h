// 直连 API 的设置在 NVS 里怎么存（directapi 命名空间）。
//
// 结构照 Cherry Studio：先有供应商（地址 + Key + 自己的模型列表），各用途再从里面挑一个模型。
//   prov_list        供应商顺序，逗号分隔的编号：p1,p2,…
//   prov_<编号>      {"name","preset","url","models":[…]}，一个供应商一条，避开 NVS 单条 4000 字节的上限
//   pkey_<编号>      这个供应商的 Key，单独存，永远不回传给网页
//   chat_p / chat_m  对话用哪个供应商的哪个模型；vis_*（视觉，空着就用对话模型）、asr_*、tts_* 同理
//   asr_lang、tts_voice、tts_voices（这家的音色列表，屏幕上选音色用）、tts_speak（语音播报开关，"0" 才是关）、prompt、memory（长期记忆，见 long_term_memory.h）
// 网页设置页（direct_api_config_server.cc）负责写，DirectApiConfig::Load 负责读。
#pragma once

#include "settings.h"

#include <string>
#include <vector>

namespace DirectApiSettings {

constexpr const char* kNs = "directapi";
constexpr size_t kMaxProviders = 8;
constexpr size_t kMaxModels = 30;
constexpr size_t kMaxProviderBytes = 3800;  // NVS 单条字符串最多 4000 字节
constexpr const char* kProviderList = "prov_list";
constexpr const char* kAsrLang = "asr_lang";
constexpr const char* kTtsVoice = "tts_voice";
constexpr const char* kTtsVoices = "tts_voices";  // 合成供应商的音色列表（一行一个），给屏幕上的音色选择用
constexpr const char* kTtsSpeak = "tts_speak";    // 语音播报开关，"0" 关；没存过按开算
constexpr const char* kPrompt = "prompt";

struct Provider {
    std::string id, name, preset, url;
    std::vector<std::string> models;
};

struct Role {
    const char* name;  // 网页里的名字
    const char* provider_key;
    const char* model_key;
};

inline constexpr Role kRoles[] = {
    {"chat", "chat_p", "chat_m"},
    {"vision", "vis_p", "vis_m"},
    {"asr", "asr_p", "asr_m"},
    {"tts", "tts_p", "tts_m"},
};
inline constexpr const Role& kChat = kRoles[0];
inline constexpr const Role& kVision = kRoles[1];
inline constexpr const Role& kAsr = kRoles[2];
inline constexpr const Role& kTts = kRoles[3];

struct Endpoint {
    std::string url, key, model;
    bool ok() const { return !url.empty() && !model.empty(); }
};

inline std::string KeyOf(const std::string& id) { return "pkey_" + id; }
inline std::string EntryOf(const std::string& id) { return "prov_" + id; }

bool ValidId(const std::string& id);  // p 加 1～3 位数字
std::vector<Provider> LoadProviders(Settings& s);
std::string SerializeProvider(const Provider& p);
// 写入新的供应商列表；列表里没有了的供应商连同 Key 一起删掉
void SaveProviders(Settings& s, const std::vector<Provider>& old_list, const std::vector<Provider>& list);
// 某个用途解析成 地址 / Key / 模型；没选、供应商不在了都返回空
Endpoint ResolveRole(Settings& s, const Role& role);

}  // namespace DirectApiSettings
