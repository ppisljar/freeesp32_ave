#ifndef SDCARD_H
#define SDCARD_H

/*
 * microSD card (SPI) mount, for offline session audio.
 *
 * The card is REMOVABLE MEDIA: every function here is safe to call when no card
 * is present, and a missing/unreadable card must never stop the device booting
 * or interrupt a running session. Callers check sdcard_is_mounted() and fall
 * back to the network/browser path, exactly as the codec failure path degrades
 * rather than aborts.
 *
 * Mount point is "/sdcard", so a BG url "sdcard://rain.wav" maps to the VFS
 * path "/sdcard/rain.wav".
 *
 * The card is NEVER formatted automatically — it is the user's card and may
 * hold their only copy of something. A card that fails to mount is reported,
 * not reformatted.
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Mount the card on the pins in the runtime settings (sd_cs/mosi/miso/clk).
 *  Returns ESP_OK on success. Any failure is non-fatal: the function logs,
 *  leaves the card unmounted, and the caller carries on. */
esp_err_t sdcard_mount(void);

/** Unmount and release the SPI bus. Safe if not mounted. */
esp_err_t sdcard_unmount(void);

/** True when a card is mounted and /sdcard is usable. */
bool sdcard_is_mounted(void);

/** Capacity in bytes. Returns ESP_ERR_INVALID_STATE when not mounted.
 *  Either pointer may be NULL. */
esp_err_t sdcard_get_space(uint64_t *total_bytes, uint64_t *free_bytes);

/** Human-readable card name ("SD32G"), or "" when not mounted. */
const char *sdcard_name(void);

#ifdef __cplusplus
}
#endif

#endif // SDCARD_H
