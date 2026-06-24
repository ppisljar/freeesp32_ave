#include "audio_driver.h"
#include "audio_config.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

static const char *TAG = "audio_driver";

/* The AC101 implementation lives in audio_driver_ac101.c. When AUDIO_DRIVER_AC101
 * is NOT selected, that file compiles to an empty translation unit and the
 * functions below provide the NONE stubs. When it IS selected, audio_driver.c
 * here forwards to the AC101 implementation. */

#if CONFIG_AUDIO_DRIVER_AC101
esp_err_t ac101_init(uint32_t sample_rate);
esp_err_t ac101_set_sample_rate(uint32_t sample_rate);
esp_err_t ac101_set_volume(float volume);
esp_err_t ac101_deinit(void);
#endif

#if CONFIG_AUDIO_DRIVER_ES8388
esp_err_t es8388_init(uint32_t sample_rate);
esp_err_t es8388_set_sample_rate(uint32_t sample_rate);
esp_err_t es8388_set_volume(float volume);
esp_err_t es8388_deinit(void);
#endif

esp_err_t audio_driver_init(uint32_t sample_rate)
{
#if CONFIG_AUDIO_DRIVER_AC101
    ESP_LOGI(TAG, "Initializing AC101 codec @ %u Hz", (unsigned)sample_rate);
    return ac101_init(sample_rate);
#elif CONFIG_AUDIO_DRIVER_ES8388
    ESP_LOGI(TAG, "Initializing ES8388 codec @ %u Hz", (unsigned)sample_rate);
    return es8388_init(sample_rate);
#else
    ESP_LOGI(TAG, "Audio driver: none (passive DAC / raw I2S)");
    (void)sample_rate;
    return ESP_OK;
#endif
}

esp_err_t audio_driver_set_sample_rate(uint32_t sample_rate)
{
#if CONFIG_AUDIO_DRIVER_AC101
    return ac101_set_sample_rate(sample_rate);
#elif CONFIG_AUDIO_DRIVER_ES8388
    return es8388_set_sample_rate(sample_rate);
#else
    (void)sample_rate;
    return ESP_OK;
#endif
}

esp_err_t audio_driver_set_volume(float volume)
{
#if CONFIG_AUDIO_DRIVER_AC101
    return ac101_set_volume(volume);
#elif CONFIG_AUDIO_DRIVER_ES8388
    return es8388_set_volume(volume);
#else
    (void)volume;
    return ESP_OK;
#endif
}

esp_err_t audio_driver_deinit(void)
{
#if CONFIG_AUDIO_DRIVER_AC101
    return ac101_deinit();
#elif CONFIG_AUDIO_DRIVER_ES8388
    return es8388_deinit();
#else
    return ESP_OK;
#endif
}

static const char *i2c_addr_hint(uint8_t addr)
{
    switch (addr) {
        case 0x10: case 0x11: return " (ES8388 / ES8311 class)";
        case 0x18:            return " (TAS5713)";
        case 0x1A:            return " (AC101)";
        case 0x20: case 0x21: case 0x22: case 0x23:
        case 0x24: case 0x25: case 0x26: case 0x27:
                              return " (PCA9554 / I/O expander)";
        case 0x40: case 0x41: case 0x42: case 0x43:
                              return " (PCA9685 PWM)";
        case 0x50: case 0x51: case 0x52: case 0x53:
                              return " (EEPROM / NXP)";
        case 0x68: case 0x69: return " (IMU / RTC class)";
        default:              return "";
    }
}

esp_err_t audio_driver_i2c_scan(const char *label,
                                int port, int sda_gpio, int scl_gpio,
                                int freq_hz)
{
    ESP_LOGI(TAG, "=== I2C bus scan: %s ===", label ? label : "(no label)");
    ESP_LOGI(TAG, "    port=%d sda=GPIO%d scl=GPIO%d freq=%d Hz",
             port, sda_gpio, scl_gpio, freq_hz);

    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = port,
        .sda_io_num = sda_gpio,
        .scl_io_num = scl_gpio,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "    i2c_new_master_bus failed: %s — scan aborted",
                 esp_err_to_name(err));
        return err;
    }

    int found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        /* i2c_master_probe is the dedicated API for address probes — it
         * issues a START + addr+W, watches for ACK, then STOP. No data. */
        esp_err_t probe = i2c_master_probe(bus, addr, 50 /* timeout ms */);
        if (probe == ESP_OK) {
            ESP_LOGI(TAG, "    [+] 0x%02X ACK%s", addr, i2c_addr_hint(addr));
            found++;
        }
    }

    if (found == 0) {
        ESP_LOGW(TAG, "    no devices ACKed — check wiring, pull-ups, "
                 "and whether the codec needs an enable pin");
    } else {
        ESP_LOGI(TAG, "    %d device(s) found", found);
    }

    i2c_del_master_bus(bus);
    return (found > 0) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void audio_driver_amp_enable(bool enable)
{
#if CONFIG_AUDIO_AMP_ENABLE_GPIO >= 0
    static bool s_amp_pin_configured = false;
    if (!s_amp_pin_configured) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << CONFIG_AUDIO_AMP_ENABLE_GPIO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        s_amp_pin_configured = true;
    }
    gpio_set_level((gpio_num_t)CONFIG_AUDIO_AMP_ENABLE_GPIO, enable ? 1 : 0);
    ESP_LOGI(TAG, "Amp enable GPIO %d -> %d", CONFIG_AUDIO_AMP_ENABLE_GPIO, enable ? 1 : 0);
#else
    (void)enable;
#endif
}
