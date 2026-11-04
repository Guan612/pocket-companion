// 应用包（.npk）的安装、删除和开机登记。包的格式、manifest 字段见 docs/app-sdk.md。
//
// 装在两个地方，目录里都是 manifest.json、app.wasm、assets/、data/：
// - 机器内部的应用分区（闪存里 2MB 的 FAT，挂在 /int；nomi/apps/<id>/）：预装应用，不插 SD 卡也能用
// - SD 卡（/sd/nomi/apps/<id>/）
// 预装应用的包（.npk）编在资源分区里（板子目录的 preinstall/），开机时比内部装着的新（或者还没装）就装进去；
// SD 卡上有同一个应用的老安装，把它的 data/ 搬过来、删掉卡上那份。预装应用现在不能删，只能被新版本盖过。
// 开机后在应用任务里扫一遍两个地方，登记到桌面（nomi_apps_add）。
// 装同一个 id 的新版本：装回原来的位置，换掉程序和 assets，保留 data/（进度）。删除连 data/ 一起删。
//
// SD 卡上的应用，机器内部分区里记着一份 manifest（/int/nomi/sdapps/<id>.json）：卡不在（开机就没插、
// 中途拔了）时照样在桌面上，只是灰着，点了提示插卡。没有插卡检测脚，所以桌面翻到眼前、点了灰着的应用时
// 去挂一下卡看看：插回来了就恢复（点的那个顺手打开），卡上新装的也登记上。卡在、但卡上没有这个应用的
// 也灰着（提示重新装）。卡不在时不让删卡上的应用：卡上的文件删不掉，下次插卡又会冒出来。
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
    bool internal = false;  // 装在机器内部（不是 SD 卡）
    bool builtin = false;   // 预装的（跟着固件走）
    bool tile = false;  // 占首页的收听卡片（开机后在后台跑起来画）
    bool absent = false;   // 装在 SD 卡上、现在用不了（卡不在，或者卡上没有它）：桌面上灰着
    std::string hint;      // 灰着时点了弹什么
    std::string manifest;  // manifest.json 原文（装在 SD 卡上的才留：机器里记一份）
    // 大模型能调的指令（self.app.command）：NOMI_EV_VOICE 的 id 是第几个（从 1 起）
    struct Voice {
        std::string name, description;
    };
    std::vector<Voice> voice;
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
    static constexpr const char* kInternalMount = "/int";
    static constexpr const char* kInternalAppsDir = "/int/nomi/apps";
    static constexpr const char* kSdCacheDir = "/int/nomi/sdapps";  // SD 卡上的应用的 manifest，机器里记一份

    static NomiPkg& GetInstance();

    // 开机时调一次：在应用任务里扫 SD 卡上装好的包、登记到桌面，挂上应用管理的删除和说明
    void Start();

    // 网页设置页：POST /apps/install（请求体就是 .npk）、GET /apps.json、POST /apps/remove {"id":…}
    std::string InstallJson(size_t length, const std::function<int(char*, size_t)>& read);
    std::string ListJson();
    std::string RemoveJson(const std::string& body);
    std::string OpenJson(const std::string& body);  // POST /apps/open {"id":…}：在设备上打开
    // 应用自带的网页：GET /app/<id>/<文件> 读包里 assets/web/ 下的文件（不写文件名是 index.html），
    // POST /app/<id>/call 把请求体交给应用（NOMI_EV_WEB）、回它给的内容。不认识的返回 false（404）
    bool WebRequest(const std::string& uri, bool post, const std::string& body, std::string& type,
                    std::string& out);

    // 删程序和数据、从桌面去掉。删不了返回原因（没装、预装的、装在卡上但卡不在），删了返回空
    std::string Remove(const std::string& id);
    // 去看一眼 SD 卡（排到应用任务里做，不等）：卡没了，卡上的应用在桌面上灰掉；插回来了恢复，卡上新装的
    // 也登记上。open_id 不空时（点了灰着的那个），看完它能用了就打开。
    // throttled：桌面翻到眼前时调的——没有卡上的应用、或者 3 秒内看过就不看（看一次要挂一下卡）
    void CheckSd(const std::string& open_id = "", bool throttled = false);
    int Count();
    // 装好的包（指针一直有效：换下来的旧包也留着）；没装返回 nullptr
    const NomiPackage* Find(const std::string& id);
    // 每轮对话的系统提示里加的：装好的应用有哪些语音指令（没有为空）
    std::string PromptContext();

private:
    std::mutex mutex_;
    std::vector<std::unique_ptr<NomiPackage>> packages_;
    // 换下来的旧包对象不删：应用任务可能还在收尾、引用着它（一个几百字节，留着无所谓）
    std::vector<std::unique_ptr<NomiPackage>> retired_;
    bool internal_ok_ = false;  // 内部应用分区挂上了
    // 看 SD 卡：最多排着一个（应用任务的队列满了会把调的人卡住），点了的那个记在这里，看完打开
    bool sd_check_queued_ = false;
    std::string sd_open_id_;
    int64_t last_sd_check_us_ = 0;

    NomiPkg() = default;
    void MountInternal();
    void Preinstall();
    // 装一个包（tar 流）。base 空 = 按规则选（装过的装回原处；新的 SD 卡在就装卡上，不在装机器里）。
    // web 为 true 时装好登记到桌面、弹提示；false（开机预装）只装文件，等扫描登记
    std::string Install(const std::string& base, size_t length, const std::function<int(char*, size_t)>& read,
                        bool web);
    // 一个地方装着的包（不登记）；内部分区的要在 NomiFlashIo::Run 里调
    std::vector<std::unique_ptr<NomiPackage>> CollectDir(const std::string& base, bool internal);
    void Scan();
    void DoCheckSd();
    const NomiPackage* Register(std::unique_ptr<NomiPackage> pkg);
    // 机器里记着的 SD 卡上的应用（要在 NomiFlashIo::Run 里调）
    std::vector<std::unique_ptr<NomiPackage>> ReadSdCache();
    void CacheSd(const NomiPackage& pkg);  // 和记着的不一样才写（省闪存）
    void UncacheSd(const std::string& id);
};
