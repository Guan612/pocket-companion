#include "direct_api_config_server.h"

#include "board.h"
#include "direct_api_config_page.h"
#include "direct_api_settings.h"
#include "long_term_memory.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>

#include <array>
#include <cstring>
#include <set>

#define TAG "DirectApiCfg"

// 网页设置页的后端。页面本身在 direct_api_config_page.h（静态 HTML + JS），这里只提供 JSON 接口：
//   GET  /config.json  供应商列表（不含 Key，只报有没有存）、各用途选的模型、人设、长期记忆
//   POST /save         整份设置一次提交（JSON），校验通过才写 NVS
//   POST /models       由设备去调供应商的 GET {地址}/models，把模型 id 列表交给页面（浏览器直连会被跨域拦）
// 存储格式见 direct_api_settings.h。浏览器模拟器（tools/device-sim/serve.py）实现了同样的接口。

namespace {

using namespace DirectApiSettings;

constexpr size_t kMaxBody = 32768;  // 8 个供应商 × 30 个模型 + 2KB 记忆，JSON 转义后留足

httpd_handle_t s_server = nullptr;
std::string s_url;

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '/')) s.pop_back();
    size_t b = s.find_first_not_of(' ');
    return b == std::string::npos ? "" : s.substr(b);
}

std::string JsonString(cJSON* obj, const char* key) {
    cJSON* v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}

esp_err_t SendJson(httpd_req_t* req, cJSON* root, const char* status = "200 OK") {
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == nullptr) return httpd_resp_send_500(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return err;
}

esp_err_t SendError(httpd_req_t* req, const std::string& message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", message.c_str());
    return SendJson(req, root);
}

bool ReadBody(httpd_req_t* req, std::string& body) {
    if (req->content_len > kMaxBody) return false;
    body.assign(req->content_len, '\0');
    size_t got = 0;
    while (got < body.size()) {
        int n = httpd_req_recv(req, body.data() + got, body.size() - got);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

esp_err_t HandleIndex(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kDirectApiConfigPage, sizeof(kDirectApiConfigPage) - 1);
}

esp_err_t HandleConfig(httpd_req_t* req) {
    Settings s(kNs, false);
    cJSON* root = cJSON_CreateObject();
    cJSON* providers = cJSON_AddArrayToObject(root, "providers");
    for (const auto& p : LoadProviders(s)) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", p.id.c_str());
        cJSON_AddStringToObject(item, "name", p.name.c_str());
        cJSON_AddStringToObject(item, "preset", p.preset.c_str());
        cJSON_AddStringToObject(item, "url", p.url.c_str());
        cJSON* models = cJSON_AddArrayToObject(item, "models");
        for (const auto& m : p.models) cJSON_AddItemToArray(models, cJSON_CreateString(m.c_str()));
        cJSON_AddBoolToObject(item, "has_key", !s.GetString(KeyOf(p.id)).empty());  // Key 不回传
        cJSON_AddItemToArray(providers, item);
    }
    cJSON* roles = cJSON_AddObjectToObject(root, "roles");
    for (const auto& r : kRoles) {
        cJSON* role = cJSON_AddObjectToObject(roles, r.name);
        cJSON_AddStringToObject(role, "p", s.GetString(r.provider_key).c_str());
        cJSON_AddStringToObject(role, "m", s.GetString(r.model_key).c_str());
    }
    cJSON_AddStringToObject(cJSON_GetObjectItem(roles, "asr"), "lang", s.GetString(kAsrLang).c_str());
    cJSON_AddStringToObject(cJSON_GetObjectItem(roles, "tts"), "voice", s.GetString(kTtsVoice).c_str());
    cJSON_AddStringToObject(root, "prompt", s.GetString(kPrompt).c_str());
    cJSON_AddStringToObject(root, "memory_md", LongTermMemory::Get().c_str());
    cJSON* limits = cJSON_AddObjectToObject(root, "limits");
    cJSON_AddNumberToObject(limits, "providers", kMaxProviders);
    cJSON_AddNumberToObject(limits, "models", kMaxModels);
    cJSON_AddNumberToObject(limits, "memory", LongTermMemory::kMaxBytes);
    return SendJson(req, root);
}

// 整份设置一次提交；任何一项不合格都不写，返回给人看的原因
esp_err_t HandleSave(httpd_req_t* req) {
    std::string body;
    if (!ReadBody(req, body)) return SendError(req, "提交的内容太大或没收全");
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return SendError(req, "提交的内容不是 JSON");
    struct Guard {
        cJSON* p;
        ~Guard() { cJSON_Delete(p); }
    } guard{root};

    Settings s(kNs, true);
    auto old_list = LoadProviders(s);
    std::vector<Provider> list;
    std::vector<std::pair<std::string, std::string>> new_keys;  // 这次填了新 Key 的供应商
    std::set<std::string> ids;
    cJSON* arr = cJSON_GetObjectItem(root, "providers");
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) > kMaxProviders) {
        return SendError(req, "供应商最多 " + std::to_string(kMaxProviders) + " 个");
    }
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, arr) {
        Provider p;
        p.id = JsonString(item, "id");
        p.name = JsonString(item, "name");
        p.preset = JsonString(item, "preset");
        p.url = Trim(JsonString(item, "url"));
        std::string key = JsonString(item, "key");
        if (!ValidId(p.id) || !ids.insert(p.id).second) return SendError(req, "供应商编号不对：" + p.id);
        if (p.name.empty() || p.name.size() > 48) return SendError(req, "供应商名称要填，且不超过 16 个字");
        if (p.url.rfind("http://", 0) != 0 && p.url.rfind("https://", 0) != 0) {
            return SendError(req, "「" + p.name + "」的地址要以 http:// 或 https:// 开头");
        }
        cJSON* models = cJSON_GetObjectItem(item, "models");
        cJSON* m = nullptr;
        cJSON_ArrayForEach(m, models) {
            if (cJSON_IsString(m) && *m->valuestring && strlen(m->valuestring) <= 120) p.models.push_back(m->valuestring);
        }
        if (p.models.size() > kMaxModels) {
            return SendError(req, "「" + p.name + "」的模型最多 " + std::to_string(kMaxModels) + " 个");
        }
        // 防止 Key 被发到别处：已存过 Key 的供应商改了地址，必须同时重填 Key
        for (const auto& old : old_list) {
            if (old.id == p.id && old.url != p.url && key.empty() && !s.GetString(KeyOf(p.id)).empty()) {
                return SendError(req, "「" + p.name + "」的地址改了，请重新填写它的 Key（防止原来的 Key 被发到新地址）");
            }
        }
        if (SerializeProvider(p).size() > kMaxProviderBytes) {
            return SendError(req, "「" + p.name + "」的模型太多了，删掉几个再保存");
        }
        if (!key.empty()) new_keys.emplace_back(p.id, key);
        list.push_back(std::move(p));
    }

    std::string memory = JsonString(root, "memory_md");
    if (memory.size() > LongTermMemory::kMaxBytes) {
        return SendError(req, "长期记忆超过 " + std::to_string(LongTermMemory::kMaxBytes) + " 字节，删掉一些再保存");
    }

    // 校验都过了才写
    SaveProviders(s, old_list, list);
    for (const auto& [id, key] : new_keys) s.SetString(KeyOf(id), key);
    cJSON* roles = cJSON_GetObjectItem(root, "roles");
    for (const auto& r : kRoles) {
        cJSON* role = cJSON_GetObjectItem(roles, r.name);
        std::string pid = JsonString(role, "p"), model = JsonString(role, "m");
        if (!ids.count(pid)) pid.clear(), model.clear();  // 供应商被删了，这个用途就空着
        s.SetString(r.provider_key, pid);
        s.SetString(r.model_key, model);
    }
    s.SetString(kAsrLang, JsonString(cJSON_GetObjectItem(roles, "asr"), "lang"));
    s.SetString(kTtsVoice, JsonString(cJSON_GetObjectItem(roles, "tts"), "voice"));
    std::string voices;
    cJSON* v = nullptr;
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(cJSON_GetObjectItem(roles, "tts"), "voices")) {
        if (!cJSON_IsString(v) || !*v->valuestring || voices.size() + strlen(v->valuestring) > 1000) continue;
        if (!voices.empty()) voices += '\n';
        voices += v->valuestring;
    }
    s.SetString(kTtsVoices, voices);
    s.SetString(kPrompt, JsonString(root, "prompt"));
    LongTermMemory::Set(memory);
    ESP_LOGI(TAG, "Settings saved (%u providers)", (unsigned)list.size());

    cJSON* ok = cJSON_CreateObject();
    cJSON_AddBoolToObject(ok, "ok", true);
    return SendJson(req, ok);
}

// 拉某个供应商的模型列表。Key 用这次填的；没填就用存过的，但地址必须和存的一致（同上，防 Key 外泄）
esp_err_t HandleModels(httpd_req_t* req) {
    std::string body;
    if (!ReadBody(req, body)) return SendError(req, "请求太大");
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return SendError(req, "请求不是 JSON");
    std::string id = JsonString(root, "id"), url = Trim(JsonString(root, "url")), key = JsonString(root, "key");
    cJSON_Delete(root);
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return SendError(req, "先填好地址");
    if (key.empty()) {
        Settings s(kNs, false);
        for (const auto& p : LoadProviders(s)) {
            if (p.id == id && p.url == url) key = s.GetString(KeyOf(id));
        }
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    http->SetTimeout(15000);
    if (!key.empty()) http->SetHeader("Authorization", "Bearer " + key);
    if (!http->Open("GET", url + "/models")) return SendError(req, "连不上 " + url);
    auto status = http->GetStatusCode();
    if (!status || *status >= 400) {
        int code = status ? *status : 0;
        if (code == 401 || code == 403) return SendError(req, "Key 不对（HTTP " + std::to_string(code) + "）");
        if (code == 404) return SendError(req, "这家没有提供模型列表，请手动添加模型名");
        return SendError(req, "获取失败（HTTP " + std::to_string(code) + "）");
    }

    // 列表可能很大（几百个模型、每个带一堆属性），边读边挑出 "id":"…"，不整份存下来
    std::vector<std::string> ids;
    std::set<std::string> seen;
    std::string window;
    std::array<char, 1024> buf;
    while (ids.size() < 400) {
        auto n = http->Read(buf.data(), buf.size());
        if (!n || *n <= 0) break;
        window.append(buf.data(), *n);
        size_t pos = 0;
        while (true) {
            size_t k = window.find("\"id\"", pos);
            if (k == std::string::npos) break;
            size_t q1 = window.find('"', window.find(':', k + 4) + 1);
            size_t q2 = q1 == std::string::npos ? q1 : window.find('"', q1 + 1);
            if (q2 == std::string::npos) {
                pos = k;  // 值还没读全，留到下一块
                break;
            }
            std::string v = window.substr(q1 + 1, q2 - q1 - 1);
            if (!v.empty() && v.size() <= 120 && seen.insert(v).second) ids.push_back(v);
            pos = q2 + 1;
        }
        window.erase(0, pos);
        if (window.size() > 4096) window.erase(0, window.size() - 256);
    }
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON* models = cJSON_AddArrayToObject(resp, "models");
    for (const auto& v : ids) cJSON_AddItemToArray(models, cJSON_CreateString(v.c_str()));
    return SendJson(req, resp);
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

}  // namespace

namespace DirectApiConfigServer {

std::string Start() {
    if (s_server == nullptr) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.stack_size = 12288;  // /models 里有 HTTPS 请求
        if (httpd_start(&s_server, &config) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start config server");
            return "";
        }
        const httpd_uri_t uris[] = {
            {.uri = "/", .method = HTTP_GET, .handler = HandleIndex, .user_ctx = nullptr},
            {.uri = "/config.json", .method = HTTP_GET, .handler = HandleConfig, .user_ctx = nullptr},
            {.uri = "/save", .method = HTTP_POST, .handler = HandleSave, .user_ctx = nullptr},
            {.uri = "/models", .method = HTTP_POST, .handler = HandleModels, .user_ctx = nullptr},
        };
        for (const auto& u : uris) httpd_register_uri_handler(s_server, &u);
    }
    std::string ip = DeviceIp();
    s_url = ip.empty() ? "" : "http://" + ip + "/";
    ESP_LOGI(TAG, "Config page: %s", s_url.c_str());
    return s_url;
}

const std::string& Url() { return s_url; }

}  // namespace DirectApiConfigServer
