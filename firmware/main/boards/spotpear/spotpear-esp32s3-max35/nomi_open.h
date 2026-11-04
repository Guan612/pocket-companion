// 打开方式（系统能力）：卡上的一个文件交给哪个应用打开。
//
// 应用在 manifest 里声明能打开哪些扩展名（"opens": ["txt"]）。打开一个文件时：记住了默认的用默认的；
// 只有一个应用能开就用它；好几个就在屏幕上弹出来选（能勾「以后都用它」）；一个都没有就弹提示。
// 选好的应用界面挂上之后收到 NOMI_EV_OPEN，用 open_path 取路径（sd:/ 开头）。
// 记住的默认存在 NVS（nomi_open），应用管理里能清掉。设备上的「文件」、网页文件管理的「在设备上打开」都走这里。
#pragma once

#include <string>
#include <vector>

struct NomiPackage;

namespace NomiOpen {

void Start();  // 读记住的默认；板子初始化时调一次

// rel 是卡上的相对路径。显示锁里调
void Open(const std::string& rel);
// 不管有没有默认都弹出来选（「打开方式…」）
void Choose(const std::string& rel);

// 应用管理用：这个应用是哪些类型的默认（"默认打开 .jpg .png"，不是任何类型的默认返回空），清掉
std::string DefaultsOf(const std::string& app_id);
void ClearDefaults(const std::string& app_id);

// 网页 POST /sd/open {"path"}：在设备上打开
std::string OpenJson(const std::string& body);

}  // namespace NomiOpen
