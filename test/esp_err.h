/**
 * @file esp_err.h (host-test stub)
 * @brief Minimal ESP-IDF esp_err.h stub for host-side unit test compilation.
 *
 * This file is used ONLY when compiling wav_parser.c on a host (Linux/macOS)
 * for unit testing.  It is NOT used in the firmware build — the real ESP-IDF
 * esp_err.h takes precedence because the ESP-IDF include paths come first in
 * the idf_component_register() include list.
 *
 * Keep this in sync with the error codes used in wav_parser.c.
 */
#ifndef ESP_ERR_H
#define ESP_ERR_H

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK              (0)
#define ESP_FAIL            (-1)
#define ESP_ERR_NO_MEM      (0x101)
#define ESP_ERR_INVALID_ARG (0x102)
#define ESP_ERR_INVALID_STATE (0x103)
#define ESP_ERR_INVALID_SIZE  (0x104)
#define ESP_ERR_NOT_FOUND     (0x105)
#define ESP_ERR_NOT_SUPPORTED (0x106)

#endif /* ESP_ERR_H */
