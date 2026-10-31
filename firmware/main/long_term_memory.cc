#include "long_term_memory.h"

#include "settings.h"

#include <esp_log.h>

namespace {

constexpr const char* kNs = "directapi";
constexpr const char* kKey = "memory";

// 事实要按行存，把模型给的换行压成空格再去掉首尾空白
std::string Sanitize(const std::string& fact) {
    std::string out;
    out.reserve(fact.size());
    for (char c : fact) {
        out += (c == '\n' || c == '\r') ? ' ' : c;
    }
    size_t begin = out.find_first_not_of(" \t");
    if (begin == std::string::npos) {
        return "";
    }
    size_t end = out.find_last_not_of(" \t");
    return out.substr(begin, end - begin + 1);
}

bool ContainsLine(const std::string& text, const std::string& line) {
    std::string padded = "\n" + text + "\n";
    return padded.find("\n" + line + "\n") != std::string::npos;
}

}  // namespace

namespace LongTermMemory {

std::string Get() {
    Settings s(kNs, false);
    return s.GetString(kKey);
}

bool Set(const std::string& text) {
    if (text.size() > kMaxBytes) {
        return false;
    }
    Settings s(kNs, true);
    s.SetString(kKey, text);
    return true;
}

bool Append(const std::string& fact) {
    std::string line = Sanitize(fact);
    if (line.empty()) {
        return false;
    }
    Settings s(kNs, true);
    std::string cur = s.GetString(kKey);
    if (ContainsLine(cur, line)) {
        return true;  // 已经记过，不算错
    }
    std::string next = cur.empty() ? line : cur + "\n" + line;
    if (next.size() > kMaxBytes) {
        ESP_LOGW("LongTermMemory", "Memory full (%u bytes), dropping: %s", (unsigned)next.size(),
                 line.substr(0, 60).c_str());
        return false;
    }
    s.SetString(kKey, next);
    return true;
}

size_t UsedBytes() {
    return Get().size();
}

}  // namespace LongTermMemory
