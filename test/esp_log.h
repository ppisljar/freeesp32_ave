/**
 * @file esp_log.h (host-test stub)
 * @brief Minimal ESP-IDF esp_log.h stub for host-side unit test compilation.
 *
 * Maps ESP_LOGE/W/I/D to printf so wav_parser.c compiles and runs on a host.
 */
#ifndef ESP_LOG_H
#define ESP_LOG_H

#include <stdio.h>

/* Map ESP log macros to printf with level prefix. */
#define ESP_LOGE(tag, fmt, ...) printf("[E][%s] " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", (tag), ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) /* suppress debug output in tests */
#define ESP_LOGV(tag, fmt, ...) /* suppress verbose output in tests */

#endif /* ESP_LOG_H */
