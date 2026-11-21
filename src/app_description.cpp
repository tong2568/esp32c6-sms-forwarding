#include <esp_app_desc.h>
#include <esp_idf_version.h>
#include "config/config.h"

// extern "C" directly on definition (not inside a block) gives true external
// C linkage — overriding the arduino-lib-builder copy in libapp_update.a.
// -Wl,--allow-multiple-definition in platformio.ini lets the linker accept
// both definitions and keep the first one found (this .o before the archive).
//
// ESP-IDF 5.x 变化：
//   - esp_app_desc_t 从 <esp_app_format.h> 移到 <esp_app_desc.h>，并新增了
//     min_efuse_blk_rev_full / max_efuse_blk_rev_full / mmu_page_size / reserv3 等字段；
//     这里改用指定初始化器（designated initializer），未列出的字段自动置 0，
//     可同时兼容 IDF 4.x 与 5.x。
//   - IDF_VER 字符串宏在 5.x 被移除，这里用 ESP_IDF_VERSION_* 拼接兜底。
#ifndef IDF_VER
#define IDF_VER_STR_HELPER(x) #x
#define IDF_VER_STR(x)        IDF_VER_STR_HELPER(x)
#define IDF_VER "esp-idf-" IDF_VER_STR(ESP_IDF_VERSION_MAJOR) "." IDF_VER_STR(ESP_IDF_VERSION_MINOR) "." IDF_VER_STR(ESP_IDF_VERSION_PATCH)
#endif

extern "C" const __attribute__((section(".rodata_desc")))
__attribute__((aligned(16)))
esp_app_desc_t esp_app_desc = {
    /* magic_word     */ .magic_word     = ESP_APP_DESC_MAGIC_WORD,
    /* secure_version */ .secure_version = 0,
    /* version        */ .version        = APP_VERSION,
    /* project_name   */ .project_name   = APP_NAME " by " APP_AUTHOR,
    /* time           */ .time           = APP_BUILD_TIME,
    /* date           */ .date           = APP_BUILD_DATE,
    /* idf_ver        */ .idf_ver        = IDF_VER,
    /* app_elf_sha256 */ .app_elf_sha256 = {0},
};
