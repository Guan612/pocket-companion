// 电台、播客共用的小工具：联网判断、http(s) 取文本、JSON / XML 里抠字段。
//
// 目录接口（radio-browser、Apple 播客）和 RSS 动辄几十 KB 到几 MB：用 cJSON 解析会在内部 RAM 里
// 建几百个小节点，所以这里只按字符串扫需要的几个字段。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace nomi {

bool NetworkUp();
bool ValidUtf8(const std::string& s);
std::string Lower(std::string s);
std::string UrlEncode(const std::string& s);

// JSON 对象文本里 "key": 后面的值；字符串值处理常见转义（含 \uXXXX），其他值原样返回
std::string JsonField(const std::string& obj, const char* key);

// XML：<tag>…</tag> 里的文字（去掉 CDATA、解开常见实体）；attr 不空时取 <tag … attr="…"> 的属性
std::string XmlField(const std::string& xml, const char* tag, const char* attr = nullptr);
std::string XmlUnescape(const std::string& s);

// GET 一个地址，跟着 301/302/307 走（最多 3 次），正文最多读 max_bytes（够了就断开）。
// enough 不空时每收一块问一次，返回 true 就不往下读了（RSS 只要前几期）。
// 成功（HTTP 200）返回 true；final_url 是跳转后的最终地址
bool HttpGet(const std::string& url, std::string& body, size_t max_bytes = 64 * 1024,
             std::string* final_url = nullptr, const std::function<bool(const std::string&)>& enough = nullptr);

// 封面：JPEG 解码后最近邻缩放成 size 见方的 RGB565，写进 out（size × size 个像素）；不是能用的 JPEG 返回 false
bool DecodeCover(const std::string& jpg, uint16_t* out, int size);

}  // namespace nomi
