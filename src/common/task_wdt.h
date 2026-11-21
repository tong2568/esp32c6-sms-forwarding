#pragma once
#include <esp_task_wdt.h>

// 安全喂狗：仅当当前任务已订阅任务看门狗(TWDT)时才调用 esp_task_wdt_reset()。
//
// ESP-IDF 5.x（arduino-esp32 3.x）对未订阅 TWDT 的任务调用 esp_task_wdt_reset()
// 会打印 "task_wdt: esp_task_wdt_reset(...): task not found" 错误，在 AT 指令超时
// 重试等场景下会刷屏。此封装在喂狗前先检查订阅状态，消除噪音，同时保留已订阅任务
// 真正的喂狗行为（防止长阻塞操作误触发看门狗复位）。
static inline void taskWdtReset() {
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        esp_task_wdt_reset();
    }
}
