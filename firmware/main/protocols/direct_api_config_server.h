// 直连 API 的网页设置页：设备连上 Wi-Fi 后在 http://<设备 IP>/ 提供表单，
// 填 ASR / LLM / TTS 的地址、key、模型，保存到 NVS 的 "directapi" 命名空间。
#ifndef DIRECT_API_CONFIG_SERVER_H
#define DIRECT_API_CONFIG_SERVER_H

#include <string>

namespace DirectApiConfigServer {

// 启动设置页，返回访问地址（拿不到 IP 时为空）
std::string Start();

// 最近一次 Start() 得到的访问地址
const std::string& Url();

}  // namespace DirectApiConfigServer

#endif  // DIRECT_API_CONFIG_SERVER_H
