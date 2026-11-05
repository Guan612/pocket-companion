// 文件（系统能力）：SD 卡上的浏览、新建文件夹、改名、删除、下载。设备上的「文件」、网页设置页的文件管理、
// 应用的 fs_* 接口都走这里，规矩只有一份：
// - 路径一律是卡上的相对路径（"小说/西游记.txt"，空 = 卡的根），不许 ".."、反斜杠。
// - 系统自己的 nomi/（应用、缓存）不显示、不让改；. 开头的、System Volume Information 这类也不显示。
// - 改了卡上的东西（上传、删、改名、新建）记一笔（Changed），媒体库据此知道要重新扫。
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace NomiFiles {

struct Entry {
    std::string name;
    bool dir = false;
    uint64_t size = 0;
    int64_t mtime = 0;  // Unix 秒
};

// rel 换成设备上的路径（/sd/…）。write 为 true 时 nomi/ 下面、卡的根本身都不行。不合规返回原因
std::string Resolve(const std::string& rel, bool write, std::string& full);
// 隐藏的名字（. 开头、系统的）；at_root 时 nomi 也算
bool Hidden(const std::string& name, bool at_root);

// 下面这些要挂着卡（调用方 NomiSdCard::Use）。成功返回空字符串，不然是给人看的原因
std::string List(const std::string& rel, std::vector<Entry>& out, bool& truncated);  // 文件夹在前，按名字排
std::string Mkdir(const std::string& rel);
std::string Rename(const std::string& from, const std::string& to);  // 也能挪到别的文件夹
std::string Remove(const std::string& rel, int* removed = nullptr);  // 文件或整个文件夹

// 卡上的东西变了（系统自己改的）：媒体库要重扫
void Changed();
uint32_t ChangeCount();

// 网页设置页：POST /sd/list {"path"}、/sd/mkdir {"path"}、/sd/rename {"from","to"}、/sd/delete {"path"}
std::string ListJson(const std::string& body);
std::string MkdirJson(const std::string& body);
std::string RenameJson(const std::string& body);
std::string DeleteJson(const std::string& body);
// GET /sd/file/<百分号编码的路径>：下载（边读边发）
using Begin = std::function<void(const std::string& type, const std::string& filename)>;
using Write = std::function<bool(const char* data, size_t len)>;
bool Download(const std::string& uri, const Begin& begin, const Write& write);

std::string UrlDecode(const std::string& s);

}  // namespace NomiFiles
