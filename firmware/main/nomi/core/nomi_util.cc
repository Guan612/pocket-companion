#include "nomi_util.h"

#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <jpeg_decoder.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define TAG "NomiUtil"

namespace nomi {

bool NetworkUp() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    return netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

bool ValidUtf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0) return false;
        for (int k = 1; k <= n; k++) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += n + 1;
    }
    return true;
}

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string UrlEncode(const std::string& s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

static void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string JsonField(const std::string& obj, const char* key) {
    std::string pat = std::string("\"") + key + "\":";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    while (p < obj.size() && obj[p] == ' ') p++;
    if (p >= obj.size()) return "";
    if (obj[p] != '"') {
        size_t e = obj.find_first_of(",}", p);
        return obj.substr(p, e == std::string::npos ? std::string::npos : e - p);
    }
    std::string out;
    for (size_t i = p + 1; i < obj.size(); i++) {
        char c = obj[i];
        if (c == '"') break;
        if (c != '\\' || i + 1 >= obj.size()) {
            out += c;
            continue;
        }
        char e = obj[++i];
        if (e == 'u' && i + 4 < obj.size()) {
            AppendUtf8(out, strtoul(obj.substr(i + 1, 4).c_str(), nullptr, 16));
            i += 4;
        } else {
            out += e == 'n' ? '\n' : e == 't' ? '\t' : e;  // \" \\ \/ 原样
        }
    }
    return out;
}

std::string XmlUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += s[i];
            continue;
        }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, nullptr, 16)
                                                                             : strtoul(ent.c_str() + 1, nullptr, 10);
            AppendUtf8(out, cp);
        } else {
            out += s.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

std::string XmlField(const std::string& xml, const char* tag, const char* attr) {
    std::string open = std::string("<") + tag;
    size_t p = 0;
    for (;;) {
        p = xml.find(open, p);
        if (p == std::string::npos) return "";
        char next = p + open.size() < xml.size() ? xml[p + open.size()] : '\0';
        if (next == '>' || next == ' ' || next == '/' || next == '\t' || next == '\n') break;  // 别把 <title 匹配到 <titles
        p += open.size();
    }
    size_t gt = xml.find('>', p);
    if (gt == std::string::npos) return "";
    if (attr != nullptr) {
        std::string head = xml.substr(p, gt - p);
        std::string pat = std::string(" ") + attr + "=";
        size_t a = head.find(pat);
        if (a == std::string::npos) return "";
        a += pat.size();
        char q = a < head.size() ? head[a] : '"';
        size_t e = head.find(q, a + 1);
        return e == std::string::npos ? "" : XmlUnescape(head.substr(a + 1, e - a - 1));
    }
    if (xml[gt - 1] == '/') return "";  // <tag/>
    size_t close = xml.find(std::string("</") + tag + ">", gt);
    if (close == std::string::npos) return "";
    std::string text = xml.substr(gt + 1, close - gt - 1);
    size_t cd = text.find("<![CDATA[");
    if (cd != std::string::npos) {
        size_t ce = text.find("]]>", cd);
        return text.substr(cd + 9, ce == std::string::npos ? std::string::npos : ce - cd - 9);
    }
    return XmlUnescape(text);
}

// 用 ESP-IDF 的 esp_http_client，不用 Board 的网络接口：后者连 https 时没设超时，服务器卡在握手
// 就一直挂着（拉一个国外播客的 RSS 卡死过 100 多秒）；而且它每条连接都另开一个 4KB 内部 RAM 栈的
// 接收任务。esp_http_client 连接、握手、读都有超时，数据在调用它的任务里同步读
bool HttpGet(const std::string& url, std::string& body, size_t max_bytes, std::string* final_url,
             const std::function<bool(const std::string&)>& enough) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 4096;  // 大于 1KB 的缓冲走 PSRAM
    cfg.buffer_size_tx = 2048;
    cfg.user_agent = "PocketCompanion/1.0";
    cfg.keep_alive_enable = false;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) return false;
    bool ok = false;
    for (int hop = 0; hop < 5; hop++) {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGW(TAG, "GET failed: %s", url.c_str());
            break;
        }
        esp_http_client_fetch_headers(client);
        int code = esp_http_client_get_status_code(client);
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            esp_http_client_flush_response(client, nullptr);
            esp_http_client_close(client);
            if (esp_http_client_set_redirection(client) != ESP_OK) break;
            continue;
        }
        if (code != 200) {
            ESP_LOGW(TAG, "GET %s -> %d", url.c_str(), code);
            break;
        }
        body.clear();
        char* buf = static_cast<char*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
        while (buf != nullptr && body.size() < max_bytes) {
            int n = esp_http_client_read(client, buf, 4096);
            if (n <= 0) break;  // 读完了，或超时 / 断开
            body.append(buf, n);
            if (enough && enough(body)) break;
        }
        heap_caps_free(buf);
        if (final_url) {
            char u[512] = {};
            esp_http_client_get_url(client, u, sizeof(u));
            *final_url = u;
        }
        ok = true;
        break;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

bool DecodeCover(const std::string& jpg, uint16_t* out, int size) {
    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata = reinterpret_cast<uint8_t*>(const_cast<char*>(jpg.data()));
    cfg.indata_size = jpg.size();
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    esp_jpeg_image_output_t info = {};
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || info.width == 0 || info.width > 640 || info.height > 640) {
        return false;
    }
    size_t out_size = info.width * info.height * 2;
    auto* pixels = static_cast<uint8_t*>(heap_caps_malloc(out_size, MALLOC_CAP_SPIRAM));
    if (pixels == nullptr) return false;
    cfg.outbuf = pixels;
    cfg.outbuf_size = out_size;
    bool ok = esp_jpeg_decode(&cfg, &info) == ESP_OK;
    if (ok) {
        auto* src = reinterpret_cast<const uint16_t*>(pixels);
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                out[y * size + x] = src[(y * info.height / size) * info.width + x * info.width / size];
            }
        }
    }
    heap_caps_free(pixels);
    return ok;
}

}  // namespace nomi
