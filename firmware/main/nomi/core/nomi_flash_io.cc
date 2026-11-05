#include "nomi_flash_io.h"

#include <esp_log.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <mutex>

#define TAG "NomiFlashIo"

namespace {

struct Job {
    const std::function<void()>* fn;
};

QueueHandle_t s_queue;
SemaphoreHandle_t s_done;
std::mutex s_mutex;  // 一次一件：做完再收下一件

void Task(void*) {
    for (;;) {
        Job job;
        xQueueReceive(s_queue, &job, portMAX_DELAY);
        (*job.fn)();
        xSemaphoreGive(s_done);
    }
}

}  // namespace

namespace NomiFlashIo {

void Start() {
    if (s_queue) return;
    s_queue = xQueueCreate(1, sizeof(Job));
    s_done = xSemaphoreCreateBinary();
    // 栈在内部 RAM（xTaskCreate 默认）：FATFS → 磨损均衡 → 闪存驱动，6KB 够用
    if (xTaskCreate(Task, "nomi_fs", 6144, nullptr, 4, nullptr) != pdPASS) ESP_LOGE(TAG, "Failed to create task");
}

void Run(const std::function<void()>& fn) {
    int probe = 0;  // 看它在哪：就是看这个任务的栈在哪
    if (!s_queue || !esp_ptr_external_ram(&probe)) {  // 自己的栈就在内部 RAM：直接做
        fn();
        return;
    }
    std::lock_guard<std::mutex> lock(s_mutex);
    Job job = {&fn};
    xQueueSend(s_queue, &job, portMAX_DELAY);
    xSemaphoreTake(s_done, portMAX_DELAY);
}

void ForPath(const std::string& path, const std::function<void()>& fn) {
    if (path.rfind("/int/", 0) == 0 || path == "/int") Run(fn);
    else fn();
}

}  // namespace NomiFlashIo
