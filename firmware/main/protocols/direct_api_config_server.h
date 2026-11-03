// 直连 API 的网页设置页：设备连上 Wi-Fi 后在 http://<设备 IP>/ 提供表单，
// 填 ASR / LLM / TTS 的地址、key、模型，保存到 NVS 的 "directapi" 命名空间。
#ifndef DIRECT_API_CONFIG_SERVER_H
#define DIRECT_API_CONFIG_SERVER_H

#include <functional>
#include <string>

namespace DirectApiConfigServer {

// 启动设置页，返回访问地址（拿不到 IP 时为空）
std::string Start();

// 最近一次 Start() 得到的访问地址
const std::string& Url();

// 停掉设置页（进配网模式前：配网热点的网页也要用 80 端口）。之后可以再 Start()
void Stop();

// 保存时天气城市变了（板子用它马上重新查天气）
extern std::function<void()> on_city_changed;

// 板子挂自己的接口（比如电台台单）：handler 收请求体（GET 时为空），返回 JSON；
// 返回 "!原因" 时回 {"ok":false,"error":原因}。要在 Start() 之前加
using JsonHandler = std::function<std::string(const std::string& body)>;
void AddJsonHandler(const char* uri, bool post, JsonHandler handler);

// 上传大文件（比如应用包）：请求体不先读进内存，handler 拿到总长度和一个读函数自己边收边处理。
// read(buf, size) 返回这次读到的字节数，<= 0 表示出错或连接断了。返回值规则同 JsonHandler
using UploadHandler = std::function<std::string(size_t length, const std::function<int(char*, size_t)>& read)>;
void AddUploadHandler(const char* uri, UploadHandler handler);

}  // namespace DirectApiConfigServer

#endif  // DIRECT_API_CONFIG_SERVER_H
