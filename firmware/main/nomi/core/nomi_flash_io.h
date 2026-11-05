// 读写机器内部闪存（应用分区 /int）的小帮手。
//
// 读写闪存时 cache 会关掉，那一会儿 PSRAM 访问不了：栈在 PSRAM 的任务（应用进程、系统任务、播客 / 音乐的
// 后台任务）一读写闪存就断言重启（esp_task_stack_is_sane_cache_disabled）。这里一个常驻小任务（栈 6KB，
// 在内部 RAM）替它们做。调用方自己的栈就在内部 RAM（网页服务器、界面任务）时直接做，不绕这一道。
// 数据缓冲放 PSRAM 没关系：闪存驱动会用内部 RAM 的小缓冲倒一下。
#pragma once

#include <functional>
#include <string>

namespace NomiFlashIo {

void Start();  // 建那个小任务；板子构造时调一次

// 在栈在内部 RAM 的任务里跑 fn，等它跑完。fn 里别等要这个任务才能完成的事（会自己等自己）
void Run(const std::function<void()>& fn);

// 路径在内部应用分区（/int/…）时经 Run 做，别的路径（SD 卡等）直接做
void ForPath(const std::string& path, const std::function<void()>& fn);

}  // namespace NomiFlashIo
