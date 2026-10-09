/*
 * RMT TX channel creation — ESP32 classic (and any target without GDMA-capable
 * RMT). See led_rmt.h for why this is split per target, and legacyesp32.md.
 */

#include "sdkconfig.h"

#if CONFIG_LED_SUPPORT_NEOPIXEL

#include "led_rmt.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

static const char *TAG = "led_rmt_esp32";

/* Claim every RMT memory block for this one channel (8 x 64 = 512 symbols).
 *
 * The classic ESP32's RMT has no DMA, so the CPU refills the FIFO while the
 * frame is transmitting; if a refill is late the strip latches a partial
 * colour and flickers. A 48-LED frame is 1152+1 symbols, so the biggest
 * possible FIFO cuts refills from ~18 per frame to ~2 and widens each refill
 * window from ~40us to ~320us — comfortably clear of WiFi interrupt bursts.
 * No other code in this project uses RMT, so taking the whole pool is free. */
#define RMT_MEM_BLOCK_SYMBOLS (SOC_RMT_MEM_WORDS_PER_CHANNEL * SOC_RMT_TX_CANDIDATES_PER_GROUP)

esp_err_t led_rmt_create_tx_channel(gpio_num_t gpio, uint32_t resolution_hz,
                                    rmt_channel_handle_t *out_channel)
{
    ESP_LOGI(TAG, "RMT TX on GPIO %d: %d symbol FIFO, CPU refill (no DMA on this target)",
             (int)gpio, RMT_MEM_BLOCK_SYMBOLS);

    rmt_tx_channel_config_t cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = gpio,
        .mem_block_symbols = RMT_MEM_BLOCK_SYMBOLS,
        .resolution_hz = resolution_hz,
        .trans_queue_depth = 4,
        .flags.invert_out = false,
        .flags.with_dma = false,
    };

    esp_err_t ret = rmt_new_tx_channel(&cfg, out_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = rmt_enable(*out_channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(ret));
        rmt_del_channel(*out_channel);
        *out_channel = NULL;
    }
    return ret;
}

#endif /* CONFIG_LED_SUPPORT_NEOPIXEL */
