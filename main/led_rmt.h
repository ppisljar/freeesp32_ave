#ifndef LED_RMT_H
#define LED_RMT_H

/*
 * Per-target RMT TX channel creation for the Neopixel backend.
 *
 * Creating the channel is the ONE place where the WS2812 driver has to care
 * which chip it is on, because the mitigation for "RMT ran dry mid-frame and
 * the strip latched a half-written colour" is completely different:
 *
 *   ESP32 classic (led_rmt_esp32.c)  — no GDMA on the RMT peripheral, so the
 *       CPU must refill the channel's FIFO mid-transmission. The defence is a
 *       big FIFO: claim all 8 memory blocks so refills are rare and each
 *       refill window is wide enough to survive a WiFi interrupt burst.
 *
 *   ESP32-S3 (led_rmt_esp32s3.c)     — RMT is GDMA-capable, so the DMA engine
 *       streams the frame and there is no CPU refill to starve in the first
 *       place. Strictly better; see legacyesp32.md.
 *
 * Everything else about the Neopixel path (encoding, transmit, the frame
 * buffer) is identical across targets and stays in led_strip.c.
 */

#include "driver/rmt_tx.h"
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create and enable an RMT TX channel suitable for driving a WS2812/SK6812
 * strip on this chip.
 *
 * @param gpio           Data line.
 * @param resolution_hz  RMT tick rate (the caller's bit timings assume this).
 * @param out_channel    Receives the enabled channel handle on success.
 */
esp_err_t led_rmt_create_tx_channel(gpio_num_t gpio, uint32_t resolution_hz,
                                    rmt_channel_handle_t *out_channel);

#ifdef __cplusplus
}
#endif

#endif // LED_RMT_H
