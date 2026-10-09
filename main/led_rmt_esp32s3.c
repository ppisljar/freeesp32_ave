/*
 * RMT TX channel creation — ESP32-S3 (GDMA-capable RMT).
 * See led_rmt.h for why this is split per target, and legacyesp32.md.
 */

#include "sdkconfig.h"

#if CONFIG_LED_SUPPORT_NEOPIXEL

#include "led_rmt.h"
#include "esp_log.h"
#include "soc/soc_caps.h"

static const char *TAG = "led_rmt_esp32s3";

/* In DMA mode mem_block_symbols is NOT a hardware FIFO claim — the driver
 * allocates a DMA buffer of this many symbols and the GDMA engine refills it
 * from the frame buffer without CPU involvement. So the value only has to be
 * big enough to keep DMA comfortably ahead of the ~1.25us/bit output rate,
 * not big enough to hold a frame. Must be even and >= SOC_RMT_MEM_WORDS_PER_CHANNEL;
 * the driver's ping-pong descriptor pair caps it near 2047.
 *
 * Note the S3's non-DMA path would be WORSE than the classic ESP32's: only
 * 4 TX channels x 48 words = 192 symbols of FIFO, vs the classic's 512. DMA
 * is what makes the S3 the better target here, not raw FIFO size. */
#define RMT_DMA_BUFFER_SYMBOLS 1024

esp_err_t led_rmt_create_tx_channel(gpio_num_t gpio, uint32_t resolution_hz,
                                    rmt_channel_handle_t *out_channel)
{
    ESP_LOGI(TAG, "RMT TX on GPIO %d: %d symbol DMA buffer (GDMA, no CPU refill)",
             (int)gpio, RMT_DMA_BUFFER_SYMBOLS);

    rmt_tx_channel_config_t cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = gpio,
        .mem_block_symbols = RMT_DMA_BUFFER_SYMBOLS,
        .resolution_hz = resolution_hz,
        .trans_queue_depth = 4,
        .flags.invert_out = false,
        .flags.with_dma = true,
    };

    esp_err_t ret = rmt_new_tx_channel(&cfg, out_channel);
    if (ret != ESP_OK) {
        /* Only the last TX channel in the group is DMA-capable, so this fails
         * if something else already took it. Fall back to the CPU-refilled
         * path rather than losing the LEDs entirely — it is the classic-ESP32
         * behaviour, and on the S3 the FIFO is smaller, so expect to need a
         * shorter strip or a quieter radio to stay flicker-free. */
        ESP_LOGW(TAG, "DMA RMT channel unavailable (%s) — retrying without DMA",
                 esp_err_to_name(ret));
        cfg.flags.with_dma = false;
        cfg.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL * SOC_RMT_TX_CANDIDATES_PER_GROUP;
        ret = rmt_new_tx_channel(&cfg, out_channel);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(ret));
            return ret;
        }
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
