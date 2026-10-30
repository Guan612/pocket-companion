#include "direct_api_config_server.h"

#include "settings.h"

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>

#include <cstring>
#include <iterator>

#define TAG "DirectApiCfg"

namespace {

constexpr const char* kSettingsNs = "directapi";
constexpr size_t kMaxBody = 8192;

struct Field {
    const char* key;
    const char* label;
    const char* hint;
    bool secret;
};

// 分组展示；ASR / TTS 的地址和 key 留空时沿用 LLM 的
const Field kLlm[] = {
    {"llm_url", "地址", "https://api.deepseek.com/v1", false},
    {"llm_key", "Key", "", true},
    {"llm_model", "模型", "deepseek-chat", false},
};
const Field kAsr[] = {
    {"asr_url", "地址", "留空沿用大模型的地址", false},
    {"asr_key", "Key", "留空沿用大模型的 key", true},
    {"asr_model", "模型", "whisper-1", false},
    {"asr_lang", "语言", "zh", false},
};
const Field kTts[] = {
    {"tts_url", "地址", "留空沿用大模型的地址", false},
    {"tts_key", "Key", "留空沿用大模型的 key", true},
    {"tts_model", "模型", "tts-1", false},
    {"tts_voice", "音色", "alloy", false},
};

httpd_handle_t s_server = nullptr;
std::string s_url;

std::string HtmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string UrlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            out += static_cast<char>(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

void AppendGroup(std::string& html, Settings& s, const char* title, const Field* fields, size_t n) {
    html += "<fieldset><legend>";
    html += title;
    html += "</legend>";
    for (size_t i = 0; i < n; i++) {
        const Field& f = fields[i];
        std::string value = s.GetString(f.key);
        html += "<label>";
        html += f.label;
        html += "<input name=\"";
        html += f.key;
        if (f.secret) {
            // key 不回显，留空表示不修改
            html += "\" type=\"password\" autocomplete=\"off\" placeholder=\"";
            html += value.empty() ? HtmlEscape(f.hint) : "已保存，留空不修改";
            html += "\">";
        } else {
            html += "\" value=\"" + HtmlEscape(value) + "\" placeholder=\"" + HtmlEscape(f.hint) + "\">";
        }
        html += "</label>";
    }
    html += "</fieldset>";
}

esp_err_t HandleIndex(httpd_req_t* req) {
    Settings s(kSettingsNs, false);
    std::string html =
        "<!doctype html><html lang=zh><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Pocket Companion 设置</title><style>"
        "body{margin:0;padding:16px;background:#111318;color:#e8eaf0;font:15px/1.5 system-ui,sans-serif}"
        "main{max-width:560px;margin:auto}h1{font-size:20px}p{color:#8b90a0}"
        "fieldset{border:1px solid #2c3040;border-radius:12px;margin:0 0 16px;padding:12px}"
        "legend{padding:0 6px;color:#6aa6ff}label{display:block;margin:8px 0;color:#8b90a0}"
        "input,textarea{display:block;width:100%;box-sizing:border-box;margin-top:4px;padding:8px;"
        "border-radius:8px;border:1px solid #2c3040;background:#1b1e26;color:#e8eaf0;font:inherit}"
        "button{width:100%;padding:12px;border:0;border-radius:10px;background:#6aa6ff;color:#0b1220;"
        "font:inherit;font-weight:600}.ok{color:#5ad19a}</style><main>"
        "<h1>Pocket Companion 设置</h1>"
        "<p>三项都走 OpenAI 兼容接口：/chat/completions、/audio/transcriptions、/audio/speech。"
        "语音合成需要支持 opus 输出。</p>";
    char query[16] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK && strstr(query, "saved")) {
        html += "<p class=ok>已保存，下一轮对话生效。</p>";
    }
    html += "<form method=post action=/save>";
    AppendGroup(html, s, "大模型 LLM", kLlm, std::size(kLlm));
    html += "<fieldset><legend>人设提示词</legend><textarea name=prompt rows=4 "
            "placeholder='留空用默认提示词'>" +
            HtmlEscape(s.GetString("prompt")) + "</textarea></fieldset>";
    AppendGroup(html, s, "语音识别 ASR", kAsr, std::size(kAsr));
    AppendGroup(html, s, "语音合成 TTS", kTts, std::size(kTts));
    html += "<button>保存</button></form></main></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html.data(), html.size());
}

void SaveField(Settings& s, const Field* fields, size_t n, const std::string& key,
               const std::string& value) {
    for (size_t i = 0; i < n; i++) {
        if (key != fields[i].key) continue;
        if (fields[i].secret && value.empty()) return;  // 密码框留空表示不改
        s.SetString(key, value);
        return;
    }
}

esp_err_t HandleSave(httpd_req_t* req) {
    if (req->content_len > kMaxBody) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too large");
    }
    std::string body(req->content_len, '\0');
    size_t got = 0;
    while (got < body.size()) {
        int n = httpd_req_recv(req, body.data() + got, body.size() - got);
        if (n <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read failed");
        got += n;
    }

    Settings s(kSettingsNs, true);
    size_t pos = 0;
    while (pos < body.size()) {
        size_t amp = body.find('&', pos);
        std::string pair = body.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        pos = amp == std::string::npos ? body.size() : amp + 1;
        size_t eq = pair.find('=');
        if (eq == std::string::npos) continue;
        std::string key = UrlDecode(pair.substr(0, eq));
        std::string value = UrlDecode(pair.substr(eq + 1));
        if (key == "prompt") {
            s.SetString(key, value);
            continue;
        }
        SaveField(s, kLlm, std::size(kLlm), key, value);
        SaveField(s, kAsr, std::size(kAsr), key, value);
        SaveField(s, kTts, std::size(kTts), key, value);
    }
    ESP_LOGI(TAG, "Settings saved");

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/?saved=1");
    return httpd_resp_send(req, nullptr, 0);
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
        config.stack_size = 8192;
        if (httpd_start(&s_server, &config) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start config server");
            return "";
        }
        httpd_uri_t index = {.uri = "/", .method = HTTP_GET, .handler = HandleIndex, .user_ctx = nullptr};
        httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = HandleSave, .user_ctx = nullptr};
        httpd_register_uri_handler(s_server, &index);
        httpd_register_uri_handler(s_server, &save);
    }
    std::string ip = DeviceIp();
    s_url = ip.empty() ? "" : "http://" + ip + "/";
    ESP_LOGI(TAG, "Config page: %s", s_url.c_str());
    return s_url;
}

const std::string& Url() { return s_url; }

}  // namespace DirectApiConfigServer
