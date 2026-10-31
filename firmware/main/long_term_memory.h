// 设备端长期记忆：一串"一行一条"的事实，存 NVS（directapi 命名空间的 memory 键）。
// 每轮对话拼进 system prompt；写入靠 self.memory.remember 工具，网页设置页可以查看和手改。
// 体量上限 2KB：只是几十条事实，NVS 单条字符串限制之内，不需要文件系统。
#ifndef LONG_TERM_MEMORY_H
#define LONG_TERM_MEMORY_H

#include <string>

namespace LongTermMemory {

constexpr size_t kMaxBytes = 2000;

std::string Get();
bool Set(const std::string& text);  // 网页编辑用；超长返回 false
bool Append(const std::string& fact);  // 换行折成空格、完全相同的条目不重复；满了返回 false
size_t UsedBytes();

}  // namespace LongTermMemory

#endif  // LONG_TERM_MEMORY_H
