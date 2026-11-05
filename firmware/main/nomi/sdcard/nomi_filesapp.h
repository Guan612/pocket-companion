// 系统应用「文件」：看 SD 卡上有什么，点文件交给能打开它的应用（nomi_open），长按删除、换打开方式、看大小时间。
// 改名、新建文件夹、传文件在网页设置页的文件管理里做（设备上打字不方便）。左边缘右划先回上一层。
#pragma once

namespace NomiFilesApp {

void Register();  // 登记到桌面（排在应用包后面、应用管理前面）。显示锁里调

}  // namespace NomiFilesApp
