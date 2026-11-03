// 应用包（.npk）的安装、删除和开机登记。包的格式、manifest 字段见 docs/app-sdk.md。
//
// 现在只装在 SD 卡上（sd:/nomi/apps/<id>/，里面 manifest.json、app.wasm、assets/、data/）；
// 机器内部的应用分区还没做。开机后在应用任务里扫一遍卡上装好的包，登记到桌面（nomi_apps_add）。
// 装同一个 id 的新版本：换掉程序和 assets，保留 data/（进度）。删除连 data/ 一起删。
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nomi_app.h"

struct NomiPackage {
    std::string id, name, version, description;
    std::string dir;  // 设备上的目录
    std::vector<std::string> permissions;
    int memory_kb = 256;
    // 线条图标（manifest 里的 icon），nomi_app_t 指着这里
    std::vector<std::vector<lv_point_precise_t>> icon_points;
    std::vector<nomi_icon_line_t> icon_lines;
    std::vector<nomi_icon_circle_t> icon_circles;
    nomi_icon_t icon = {};
    nomi_app_t app = {};  // 登记到桌面的那一项，user 指回这个包

    bool Has(const char* permission) const;
};

class NomiPkg {
public:
    static constexpr const char* kAppsDir = "/sd/nomi/apps";

    static NomiPkg& GetInstance();

    // 开机时调一次：在应用任务里扫 SD 卡上装好的包、登记到桌面，挂上应用管理的删除和说明
    void Start();

    // 网页设置页：POST /apps/install（请求体就是 .npk）、GET /apps.json、POST /apps/remove {"id":…}
    std::string InstallJson(size_t length, const std::function<int(char*, size_t)>& read);
    std::string ListJson();
    std::string RemoveJson(const std::string& body);
    std::string OpenJson(const std::string& body);  // POST /apps/open {"id":…}：在设备上打开

    bool Remove(const std::string& id);  // 删程序和数据、从桌面去掉
    int Count();

private:
    std::mutex mutex_;
    std::vector<std::unique_ptr<NomiPackage>> packages_;
    // 换下来的旧包对象不删：应用任务可能还在收尾、引用着它（一个几百字节，留着无所谓）
    std::vector<std::unique_ptr<NomiPackage>> retired_;

    NomiPkg() = default;
    void Scan();
    void Register(std::unique_ptr<NomiPackage> pkg);
};
