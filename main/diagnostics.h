/**
 * @file diagnostics.h
 * @brief WiFi-accessible diagnostics: reset reason, runtime log ring, core dumps.
 *
 * Restores serial-monitor parity for a headless / OTA-updated device that has no
 * UART attached. Three independent facilities (see diagnostics_over_wifi_plan.md):
 *
 *   Layer 1  reset reason      — why the last boot happened (panic? WDT? power?).
 *   Layer 2  runtime log ring  — an esp_log_set_vprintf() hook mirrors every
 *                                ESP_LOGx line into a PSRAM ring served at
 *                                GET /api/logs.
 *   Layer 3  core dump         — on panic ESP-IDF writes registers+stacks to the
 *                                `coredump` flash partition; retrieved over WiFi
 *                                after reboot (compiled in only when
 *                                CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y).
 *
 * Call diagnostics_early_init() as the FIRST thing in app_main so the log hook
 * captures boot-time logs and the reset reason is latched before anything else
 * can reset it.
 */

#ifndef DIAGNOSTICS_H
#define DIAGNOSTICS_H

#include "esp_system.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Latch the reset reason, allocate the PSRAM log ring, and install the
 *        esp_log vprintf hook. Idempotent; safe to call once at the very top of
 *        app_main. If the PSRAM ring can't be allocated, log capture is disabled
 *        but the reset-reason and coredump facilities still work.
 */
void diagnostics_early_init(void);

/** @brief The reset reason latched at boot (esp_reset_reason value). */
esp_reset_reason_t diagnostics_reset_reason(void);

/** @brief Human-readable reset reason ("PANIC", "TASK_WDT", "POWERON", …). */
const char *diagnostics_reset_reason_str(void);

/**
 * @brief Copy the buffered log text (oldest→newest) into `out`.
 *
 * If the ring holds more than out_size bytes, the OLDEST bytes are dropped so
 * the most recent output is preserved. Returns the number of bytes written
 * (never NUL-terminates; the caller sizes the response by the return value).
 */
size_t diagnostics_get_logs(char *out, size_t out_size);

/** @brief Number of bytes currently buffered in the log ring. */
size_t diagnostics_logs_size(void);

/** @brief Discard all buffered log text. */
void diagnostics_clear_logs(void);

/**
 * @brief Whether a core dump image is stored in flash.
 * @param size_out  If non-NULL and a dump is present, receives its size in bytes.
 * @return true if a valid core dump is present (always false when coredump-to-
 *         flash is not compiled in).
 */
bool diagnostics_coredump_present(size_t *size_out);

/**
 * @brief Read a slice of the stored core-dump image (for chunked HTTP streaming).
 * @param offset  Byte offset into the image.
 * @param out     Destination buffer.
 * @param len     Bytes to read.
 * @return Bytes actually read (0 at/after end, or when no dump / not compiled in).
 */
size_t diagnostics_coredump_read(size_t offset, void *out, size_t len);

/** @brief Erase the stored core dump. Returns ESP_OK (or ESP_ERR_NOT_SUPPORTED). */
esp_err_t diagnostics_coredump_erase(void);

#ifdef __cplusplus
}
#endif

#endif /* DIAGNOSTICS_H */
