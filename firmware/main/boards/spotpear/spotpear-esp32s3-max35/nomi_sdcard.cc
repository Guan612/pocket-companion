#include "nomi_sdcard.h"

#include "application.h"
#include "config.h"
#include "nomi_files.h"

#include <cJSON.h>
#include <diskio_impl.h>
#include <diskio_sdmmc.h>
#include <driver/gpio.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <ff.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdmmc_cmd.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>

#define TAG "NomiSdCard"

namespace {

constexpr int64_t kIdleUnmountUs = 10 * 1000 * 1000;  // 没人用 10 秒后卸载，把 GPIO43 还回去
constexpr int kRetries = 2;                            // 读写出错后重新初始化卡再试几次

/* ---------- FatFs 底层：照 ESP-IDF 的 diskio_sdmmc.c，读写出错先重新初始化卡再试 ---------- */

sdmmc_card_t* s_card = nullptr;  // 挂着的卡（只有一张）

bool Reinit() {
    vTaskDelay(pdMS_TO_TICKS(20));
    sdmmc_host_t host = s_card->host;  // sdmmc_card_init 会先清空 card，host 先拷出来
    esp_err_t err = sdmmc_card_init(&host, s_card);
    if (err != ESP_OK) ESP_LOGW(TAG, "Reinit failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

DSTATUS DiskInit(unsigned char) { return 0; }
DSTATUS DiskStatus(unsigned char) { return 0; }

DRESULT DiskRead(unsigned char, unsigned char* buff, uint32_t sector, UINT count) {
    for (int attempt = 0;; attempt++) {
        esp_err_t err = sdmmc_read_sectors(s_card, buff, sector, count);
        if (err == ESP_OK) return RES_OK;
        ESP_LOGW(TAG, "Read %lu+%u failed: %s (try %d)", static_cast<unsigned long>(sector), count,
                 esp_err_to_name(err), attempt + 1);
        if (attempt == kRetries || !Reinit()) break;
    }
    NomiSdCard::GetInstance().MarkBroken();
    return RES_ERROR;
}

DRESULT DiskWrite(unsigned char, const unsigned char* buff, uint32_t sector, UINT count) {
    for (int attempt = 0;; attempt++) {
        esp_err_t err = sdmmc_write_sectors(s_card, buff, sector, count);
        if (err == ESP_OK) return RES_OK;
        ESP_LOGW(TAG, "Write %lu+%u failed: %s (try %d)", static_cast<unsigned long>(sector), count,
                 esp_err_to_name(err), attempt + 1);
        if (attempt == kRetries || !Reinit()) break;
    }
    NomiSdCard::GetInstance().MarkBroken();
    return RES_ERROR;
}

DRESULT DiskIoctl(unsigned char, unsigned char cmd, void* buff) {
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            *static_cast<DWORD*>(buff) = s_card->csd.capacity;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *static_cast<WORD*>(buff) = s_card->csd.sector_size;
            return RES_OK;
#if FF_USE_TRIM
        case CTRL_TRIM: {
            if (sdmmc_can_trim(s_card) != ESP_OK) return RES_PARERR;
            auto* range = static_cast<DWORD*>(buff);
            sdmmc_erase_arg_t arg = sdmmc_can_discard(s_card) == ESP_OK ? SDMMC_DISCARD_ARG : SDMMC_ERASE_ARG;
            return sdmmc_erase_sectors(s_card, range[0], range[1] - range[0] + 1, arg) == ESP_OK ? RES_OK
                                                                                                 : RES_ERROR;
        }
#endif
    }
    return RES_ERROR;
}

const ff_diskio_impl_t kDiskImpl = {
    .init = &DiskInit,
    .status = &DiskStatus,
    .read = &DiskRead,
    .write = &DiskWrite,
    .ioctl = &DiskIoctl,
};

}  // namespace

NomiSdCard& NomiSdCard::GetInstance() {
    static NomiSdCard instance;
    return instance;
}

/* ---------- 挂载 / 卸载 ---------- */

bool NomiSdCard::MountLocked() {
    if (card_) return true;
    if (!bus_ready_) {
        spi_bus_config_t bus = {};
        bus.mosi_io_num = SD_MOSI_PIN;
        bus.miso_io_num = SD_MISO_PIN;
        bus.sclk_io_num = SD_CLK_PIN;
        bus.quadwp_io_num = -1;
        bus.quadhd_io_num = -1;
        bus.max_transfer_sz = 4096;
        esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
            state_ = kNoCard;
            return false;
        }
        bus_ready_ = true;
    }
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_SPI_HOST;
    slot.gpio_cs = SD_CS_PIN;
    esp_vfs_fat_mount_config_t mount = {};
    mount.format_if_mount_failed = false;  // 绝不自动格式化
    mount.max_files = 6;  // 看书时同时开着：书、补字的字体，再加放本地歌、应用读文件、章节缓存
    mount.allocation_unit_size = 16 * 1024;

    // 卡应答了但 FatFs 挂不上（exFAT、NTFS、没分区）时返回 ESP_FAIL；卡不应答是超时、响应不对之类。
    // 偶尔的读写错也可能落在挂载这一下，所以都再试一次
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2; attempt++) {
        err = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slot, &mount, &card_);
        if (err == ESP_OK) break;
        card_ = nullptr;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (err != ESP_OK) {
        state_ = err == ESP_FAIL ? kUnsupported : kNoCard;
        epoch_++;
        ESP_LOGW(TAG, "Mount failed: %s (%s)", esp_err_to_name(err),
                 state_ == kUnsupported ? "card answered, not FAT" : "no card");
        ReleaseBusLocked();
        return false;
    }
    s_card = card_;
    ff_diskio_register(ff_diskio_get_pdrv_card(card_), &kDiskImpl);  // 换成会重试的读写
    broken_ = false;
    state_ = kReady;
    ESP_LOGI(TAG, "Mounted %s: %s, %llu MB", kMountPoint, card_->cid.name,
             static_cast<unsigned long long>(card_->csd.capacity) * card_->csd.sector_size >> 20);
    return true;
}

void NomiSdCard::UnmountLocked() {
    if (idle_timer_) esp_timer_stop(idle_timer_);
    if (card_) {
        esp_vfs_fat_sdcard_unmount(kMountPoint, card_);  // 也会移除 SPI 设备、释放 card_
        card_ = nullptr;
        s_card = nullptr;
        ESP_LOGI(TAG, "Unmounted");
    }
    ReleaseBusLocked();
}

void NomiSdCard::ReleaseBusLocked() {
    if (!bus_ready_) return;
    spi_bus_free(SD_SPI_HOST);
    bus_ready_ = false;
    gpio_reset_pin(SD_MOSI_PIN);
    gpio_reset_pin(SD_MISO_PIN);
    gpio_reset_pin(SD_CS_PIN);
    if (on_bus_released) on_bus_released();  // GPIO43 交给板子（功放）
}

void NomiSdCard::UnmountIfIdle() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (users_ == 0) UnmountLocked();
}

uint32_t NomiSdCard::Serial() {
    std::lock_guard<std::mutex> lock(mutex_);
    return card_ ? static_cast<uint32_t>(card_->cid.serial) : 0;
}

bool NomiSdCard::Acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (idle_timer_) esp_timer_stop(idle_timer_);
    // 闲着的时候卡可能被拔了、换了：没人在用时先问一声，不应答就卸掉重挂
    if (card_ && users_ == 0 && (broken_ || sdmmc_get_status(card_) != ESP_OK)) {
        ESP_LOGI(TAG, "Card gone or broken, remounting");
        UnmountLocked();
        epoch_++;
    }
    if (!MountLocked()) return false;
    users_++;
    return true;
}

void NomiSdCard::Release() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (users_ > 0) users_--;
    if (users_ > 0) return;
    if (broken_) {
        state_ = kNoCard;
        UnmountLocked();
        epoch_++;
        return;
    }
    if (!idle_timer_) {
        esp_timer_create_args_t args = {};
        args.callback = [](void*) {
            Application::GetInstance().Schedule([]() { NomiSdCard::GetInstance().UnmountIfIdle(); });
        };
        args.name = "sd_idle";
        esp_timer_create(&args, &idle_timer_);
    }
    esp_timer_stop(idle_timer_);
    esp_timer_start_once(idle_timer_, kIdleUnmountUs);
}

NomiSdCard::State NomiSdCard::GetState() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::string NomiSdCard::StatusJson() {
    uint64_t total = 0, free_bytes = 0;
    {
        Use sd;  // 当场挂一下：没有插卡检测脚，只能这样知道卡在不在
        if (sd) esp_vfs_fat_info(kMountPoint, &total, &free_bytes);
    }
    State state = GetState();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "state",
                            state == kReady ? "ready" : state == kUnsupported ? "unsupported" : "none");
    if (state == kReady) {
        cJSON_AddNumberToObject(root, "total", static_cast<double>(total));
        cJSON_AddNumberToObject(root, "free", static_cast<double>(free_bytes));
    }
    char* json = cJSON_PrintUnformatted(root);
    std::string out = json ? json : "{}";
    cJSON_free(json);
    cJSON_Delete(root);
    return out;
}

// 网页往卡上传一个文件（POST /sd/upload）：请求体第一行是卡上的路径（比如 fonts/x.ttf，UTF-8），
// 后面是文件内容。先写到 .part，收完再改名，传一半断了不会留下半个文件。系统的 nomi/ 下不让写
std::string NomiSdCard::UploadJson(size_t length, const std::function<int(char*, size_t)>& read) {
    constexpr size_t kChunk = 8192;
    auto* buf = static_cast<char*>(heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM));
    if (!buf) return "!内存不够";
    size_t consumed = 0, have = 0;
    auto fill = [&]() -> bool {  // 再收一块到 buf（have 是收到多少）
        size_t want = std::min(kChunk, length - consumed);
        if (want == 0) return false;
        int n = read(buf, want);
        if (n <= 0) return false;
        have = n;
        consumed += n;
        return true;
    };
    std::string error, path, tmp;
    FILE* f = nullptr;
    size_t start = 0, written = 0;
    // 第一行：路径
    while (error.empty()) {
        if (!fill()) {
            error = "上传断了";
            break;
        }
        size_t nl = 0;
        while (nl < have && buf[nl] != '\n') nl++;
        path.append(buf, nl);
        if (path.size() > 240) error = "路径太长";
        if (nl < have) {
            start = nl + 1;
            break;
        }
    }
    if (!path.empty() && path.back() == '\r') path.pop_back();
    while (!path.empty() && path[0] == '/') path.erase(0, 1);
    if (error.empty() && (path.empty() || path.find("..") != std::string::npos || path.find('\\') != std::string::npos ||
                          path.back() == '/' || path.rfind("nomi/", 0) == 0)) {
        error = "路径不对（要写成 fonts/x.ttf 这样，不能写 nomi/ 下面）";
    }
    Use sd;
    if (error.empty() && !sd) error = "SD 卡没插";
    std::string full = std::string(kMountPoint) + "/" + path;
    if (error.empty()) {
        // 一层层建文件夹
        for (size_t i = strlen(kMountPoint) + 1; i < full.size(); i++) {
            if (full[i] != '/') continue;
            std::string dir = full.substr(0, i);
            struct stat st;
            if (stat(dir.c_str(), &st) != 0 && mkdir(dir.c_str(), 0775) != 0) {
                error = "建不了文件夹";
                break;
            }
        }
    }
    if (error.empty()) {
        tmp = full + ".part";
        f = fopen(tmp.c_str(), "wb");
        if (!f) error = "写不了卡（满了？只读？）";
    }
    int64_t t0 = esp_timer_get_time();
    while (error.empty()) {
        if (have > start && fwrite(buf + start, 1, have - start, f) != have - start) {
            error = "写卡出错（满了？）";
            break;
        }
        written += have - start;
        start = 0;
        if (consumed >= length) break;
        if (!fill()) error = "上传断了";
    }
    if (f) fclose(f);
    while (consumed < length && fill()) {
    }  // 出错了也把剩下的收完，浏览器才拿得到回复
    heap_caps_free(buf);
    if (error.empty()) {
        remove(full.c_str());
        if (rename(tmp.c_str(), full.c_str()) != 0) error = "改名失败";
    }
    if (!error.empty()) {
        if (!tmp.empty()) remove(tmp.c_str());
        ESP_LOGW(TAG, "Upload %s failed: %s", path.c_str(), error.c_str());
        return "!" + error;
    }
    NomiFiles::Changed();  // 媒体库要重扫
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    ESP_LOGI(TAG, "Uploaded %s: %u bytes in %lld ms", full.c_str(), static_cast<unsigned>(written), ms);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "path", path.c_str());
    cJSON_AddNumberToObject(root, "size", static_cast<double>(written));
    char* json = cJSON_PrintUnformatted(root);
    std::string out = json ? json : "{}";
    cJSON_free(json);
    cJSON_Delete(root);
    return out;
}
