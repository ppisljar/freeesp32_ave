#ifndef AUDIO_DRIVER_H
#define AUDIO_DRIVER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Audio codec driver abstraction.
 *
 * Wraps codec-specific initialization (I2C bring-up, register writes for
 * clock/PLL/DAC/PA enable, sample-rate config) behind a stable interface so
 * audio_manager can stay codec-agnostic.
 *
 * Selection happens at compile time via the AUDIO_DRIVER Kconfig choice:
 *   - CONFIG_AUDIO_DRIVER_NONE   — passive DAC (PCM5102A, MAX98357A); all
 *                                  operations below are no-ops.
 *   - CONFIG_AUDIO_DRIVER_AC101  — X-Powers AC101 (AI-Thinker A1S / Audio-Kit
 *                                  boards); does full I2C init.
 *
 * Call sequence: audio_driver_init() once at startup BEFORE audio_manager
 * brings up the I2S driver. set_sample_rate / set_volume any time after init.
 */

/**
 * Initialize the codec.
 *
 * For AUDIO_DRIVER_NONE this returns ESP_OK immediately.
 *
 * For AUDIO_DRIVER_AC101 this performs:
 *   1. I2C master bring-up on (sda, scl, freq_hz, port from sdkconfig)
 *   2. Probe the codec (read CHIP_AUDIO_RS) and fail with ESP_ERR_NOT_FOUND
 *      if no AC101 is present at the configured address (0x1A by default)
 *   3. Issue the documented init register sequence (PLL, sysclk, DAC enable,
 *      output mixer enable, headphone PA enable)
 *   4. Set initial volume to the value derived from AUDIO_DEFAULT_VOLUME
 */
esp_err_t audio_driver_init(uint32_t sample_rate);

/**
 * Update codec sample-rate configuration.
 *
 * Only relevant for codec drivers that have their own clock generators
 * (AC101 PLL must match the I2S BCLK rate). NONE returns ESP_OK.
 *
 * Note: changing sample rate at runtime usually requires also reconfiguring
 * the ESP32 I2S clock; audio_manager owns that and calls this hook after
 * the I2S side is reconfigured.
 */
esp_err_t audio_driver_set_sample_rate(uint32_t sample_rate);

/**
 * Set output volume.
 *
 * @param volume linear gain in the range [0.0, 1.0]. The driver maps this
 *               to the codec's native volume control (e.g. AC101's 0-63
 *               headphone-volume register).
 */
esp_err_t audio_driver_set_volume(float volume);

/**
 * Shut down the codec and release I2C resources.
 */
esp_err_t audio_driver_deinit(void);

/**
 * Diagnostic: scan an I2C bus and log every address that ACKs.
 *
 * Walks addresses 0x08-0x77, sending a zero-byte transaction to each. Each
 * ACK is logged at INFO level along with the address in hex AND a hint at
 * which codec lives there ("0x10 = ES8388-class", "0x1A = AC101", etc).
 *
 * Brings up the I2C bus from scratch and tears it down before returning, so
 * callers don't need to worry about persistent state. This means it cannot
 * be called while the active codec driver is using the bus — typically only
 * called from the failure path when no codec was detected.
 *
 * @param label  short string prefixing every log line, useful when scanning
 *               multiple pin configurations in succession (e.g. "scan #1
 *               (WS=26 DO=25)" vs "scan #2 (WS/DO swapped)")
 */
esp_err_t audio_driver_i2c_scan(const char *label,
                                int port, int sda_gpio, int scl_gpio,
                                int freq_hz);

/**
 * Drive the AUDIO_AMP_ENABLE_GPIO pin (if any) to the requested level.
 *
 * Separated from audio_driver_init because the amplifier enable pin is a
 * board-level concern, not a codec-level one — some boards have an amp
 * enable but no codec, and vice versa. audio_manager calls this AFTER the
 * codec is up and the first DMA buffer is queued, to avoid popping the
 * speaker with uninitialized output.
 */
void audio_driver_amp_enable(bool enable);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_DRIVER_H
