#include "nomi_phone.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define TAG "NomiPhone"

namespace {

constexpr const char* kName = "Bangle.js Nomi";  // Gadgetbridge 按名字开头认设备
constexpr size_t kMaxLine = 8192;                  // 一条消息最长（通知、天气这些也走这里），再长的丢掉

// Nordic UART（UUID 在 NimBLE 里是倒着写的字节）
const ble_uuid128_t kNusService = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3,
                                                   0xb5, 0x01, 0x00, 0x40, 0x6e);
const ble_uuid128_t kNusTx = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,
                                              0x02, 0x00, 0x40, 0x6e);  // 手机写
const ble_uuid128_t kNusRx = BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,
                                              0x03, 0x00, 0x40, 0x6e);  // 板子通知

uint8_t s_own_addr_type;
uint16_t s_rx_handle;
std::atomic<uint16_t> s_conn{BLE_HS_CONN_HANDLE_NONE};
std::string s_line;       // 收了一半的一行（只在蓝牙任务里动）
bool s_line_too_long;     // 这一行超长了，等换行再重新开始
std::mutex s_send_mutex;  // 一次发一整条，别和别的交错
std::mutex s_listener_mutex;
NomiPhone::MessageListener s_on_message;
NomiPhone::StateListener s_on_state;

ble_gatt_chr_def s_chrs[3];
ble_gatt_svc_def s_svcs[2];

void Advertise();

void NotifyState(bool connected) {
    NomiPhone::StateListener cb;
    {
        std::lock_guard<std::mutex> lock(s_listener_mutex);
        cb = s_on_state;
    }
    if (cb) cb(connected);
}

// 连上时手机发的 JavaScript：只认 setTime(秒)，拿来对时（时区用设备自己设的）
void HandleScript(const std::string& line) {
    size_t at = line.find("setTime(");
    if (at == std::string::npos) return;
    char* end = nullptr;
    long long sec = strtoll(line.c_str() + at + 8, &end, 10);
    if (end == line.c_str() + at + 8 || sec < 1700000000LL) return;
    timeval tv = {static_cast<time_t>(sec), 0};
    settimeofday(&tv, nullptr);
    ESP_LOGI(TAG, "Time set from phone: %lld", sec);
}

void OnLine(std::string line) {
    if (!line.empty() && line[0] == '\x10') line.erase(0, 1);  // Espruino 的「这行不回显」
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == ';')) line.pop_back();
    if (line.rfind("GB(", 0) != 0 || line.back() != ')') {
        HandleScript(line);
        return;
    }
    cJSON* root = cJSON_ParseWithLength(line.c_str() + 3, line.size() - 4);
    cJSON* t = cJSON_GetObjectItem(root, "t");
    if (!cJSON_IsString(t)) {
        ESP_LOGW(TAG, "Bad message: %.80s", line.c_str());
        cJSON_Delete(root);
        return;
    }
    std::string type = t->valuestring;
    // 重新打印一遍：手机发的中文可能是 \uXXXX 转义的，应用拿到的统一是 UTF-8
    char* text = cJSON_PrintUnformatted(root);
    std::string json = text ? text : "";
    cJSON_free(text);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "<< %s (%u bytes)", type.c_str(), static_cast<unsigned>(json.size()));
    NomiPhone::MessageListener cb;
    {
        std::lock_guard<std::mutex> lock(s_listener_mutex);
        cb = s_on_message;
    }
    if (cb) cb(type, json);
}

void Feed(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n') {
            if (!s_line_too_long) OnLine(std::move(s_line));
            s_line.clear();
            s_line_too_long = false;
        } else if (s_line.size() < kMaxLine) {
            s_line += data[i];
        } else if (!s_line_too_long) {
            s_line_too_long = true;
            ESP_LOGW(TAG, "Message longer than %u bytes, dropped", static_cast<unsigned>(kMaxLine));
        }
    }
}

int Access(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    char buf[512];
    uint16_t total = OS_MBUF_PKTLEN(ctxt->om);
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, std::min<uint16_t>(total, sizeof(buf)), &n) == 0) Feed(buf, n);
    return 0;
}

int OnGap(ble_gap_event* event, void*) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_line.clear();
            s_line_too_long = false;
            ESP_LOGI(TAG, "Phone connected, internal free %u",
                     static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
            NotifyState(true);
        } else {
            Advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Phone disconnected (reason %d)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        NotifyState(false);
        Advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        Advertise();
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

// 广播包放服务 UUID（Gadgetbridge 按它过滤），名字放扫描回复（放不下一起）
void Advertise() {
    ble_hs_adv_fields fields = {};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &kNusService;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    ble_hs_adv_fields rsp = {};
    rsp.name = reinterpret_cast<const uint8_t*>(kName);
    rsp.name_len = strlen(kName);
    rsp.name_is_complete = 1;
    if (rc == 0) rc = ble_gap_adv_rsp_set_fields(&rsp);
    ble_gap_adv_params adv = {};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (rc == 0) rc = ble_gap_adv_start(s_own_addr_type, nullptr, BLE_HS_FOREVER, &adv, OnGap, nullptr);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "Advertise failed: %d", rc);
}

void OnSync() {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    Advertise();
    ESP_LOGI(TAG, "Advertising as %s, internal free %u (largest %u)", kName,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
}

void HostTask(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

}  // namespace

namespace NomiPhone {

// 开机就开：控制器要一大块连续的内部内存，开机一阵过去内存就碎了，申请不到
void Start() {
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return;
    }
    ble_hs_cfg.sync_cb = OnSync;
    ble_hs_cfg.reset_cb = [](int reason) { ESP_LOGW(TAG, "Host reset: %d", reason); };
    ble_svc_gap_init();
    ble_svc_gatt_init();
    s_chrs[0] = {};
    s_chrs[0].uuid = &kNusTx.u;
    s_chrs[0].access_cb = Access;
    s_chrs[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;
    s_chrs[1] = {};
    s_chrs[1].uuid = &kNusRx.u;
    s_chrs[1].access_cb = Access;
    s_chrs[1].flags = BLE_GATT_CHR_F_NOTIFY;
    s_chrs[1].val_handle = &s_rx_handle;
    s_chrs[2] = {};
    s_svcs[0] = {};
    s_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    s_svcs[0].uuid = &kNusService.u;
    s_svcs[0].characteristics = s_chrs;
    s_svcs[1] = {};
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) ESP_LOGE(TAG, "GATT setup failed: %d", rc);
    ble_svc_gap_device_name_set(kName);
    nimble_port_freertos_init(HostTask);
    ESP_LOGI(TAG, "BLE started, internal RAM used %d bytes",
             static_cast<int>(before) - static_cast<int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
}

bool Connected() { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

bool Send(const std::string& json) {
    std::lock_guard<std::mutex> lock(s_send_mutex);
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) return false;
    // 行尾要 \r\n：Gadgetbridge 按 Espruino 的习惯把换行前一个字符当 \r 去掉，只发 \n 会吃掉 JSON 最后的 }
    std::string line = json + "\r\n";
    uint16_t mtu = ble_att_mtu(conn);
    size_t chunk = mtu > 3 ? mtu - 3 : 20;
    for (size_t pos = 0; pos < line.size();) {
        size_t n = std::min(chunk, line.size() - pos);
        int rc = BLE_HS_ENOMEM;
        for (int attempt = 0; attempt < 20 && rc == BLE_HS_ENOMEM; attempt++) {
            os_mbuf* om = ble_hs_mbuf_from_flat(line.data() + pos, n);
            rc = om ? ble_gatts_notify_custom(conn, s_rx_handle, om) : BLE_HS_ENOMEM;
            if (rc == BLE_HS_ENOMEM) vTaskDelay(pdMS_TO_TICKS(10));  // 缓冲满了：等发出去一些
        }
        if (rc != 0) {
            ESP_LOGW(TAG, "Send failed: %d", rc);
            return false;
        }
        pos += n;
    }
    return true;
}

void SetListeners(MessageListener on_message, StateListener on_state) {
    std::lock_guard<std::mutex> lock(s_listener_mutex);
    s_on_message = std::move(on_message);
    s_on_state = std::move(on_state);
}

}  // namespace NomiPhone
