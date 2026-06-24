/*
 * ES8388 codec driver — playback path.
 *
 * Adapted from the cspot/bell BSD-licensed ES8388AudioSink (which itself
 * follows the ESP-ADF reference init sequence). Strips ADC bring-up and
 * mic-bias paths; keeps stereo line-out via LOUT1/ROUT1 and headphone-out
 * via LOUT2/ROUT2, both enabled and addressable via volume control.
 *
 * The ES8388 is an 8-bit-register I2C codec at 7-bit address 0x10. Each
 * register read/write transfers one byte after a single-byte register
 * address.
 *
 * IMPORTANT: ES8388 REQUIRES an external MCLK input — unlike AC101, it
 * cannot derive its clock from BCLK alone. The ESP32 generates MCLK from
 * its I2S APLL; route it via CONFIG_AUDIO_I2S_MCLK_GPIO. On the classic
 * ESP32 only GPIO 0, 1, or 3 can output MCLK due to pin-mux constraints
 * (and GPIO 1/3 are normally the UART, so practically GPIO 0).
 */

#include "sdkconfig.h"

#if CONFIG_AUDIO_DRIVER_ES8388

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "es8388";

#define ES8388_I2C_ADDR             0x10  /* 7-bit address */

/* Control registers */
#define ES8388_REG_CONTROL1         0x00
#define ES8388_REG_CONTROL2         0x01
#define ES8388_REG_CHIPPOWER        0x02
#define ES8388_REG_ADCPOWER         0x03
#define ES8388_REG_DACPOWER         0x04
#define ES8388_REG_MASTERMODE       0x08

/* ADC */
#define ES8388_REG_ADCCONTROL1      0x09
#define ES8388_REG_ADCCONTROL2      0x0A
#define ES8388_REG_ADCCONTROL3      0x0B
#define ES8388_REG_ADCCONTROL4      0x0C
#define ES8388_REG_ADCCONTROL5      0x0D
#define ES8388_REG_ADCCONTROL8      0x10
#define ES8388_REG_ADCCONTROL9      0x11

/* DAC */
#define ES8388_REG_DACCONTROL1      0x17
#define ES8388_REG_DACCONTROL2      0x18
#define ES8388_REG_DACCONTROL3      0x19
#define ES8388_REG_DACCONTROL4      0x1A  /* LDAC volume: 0=0dB, 0xC0=-96dB */
#define ES8388_REG_DACCONTROL5      0x1B  /* RDAC volume: same scale */
#define ES8388_REG_DACCONTROL16     0x26
#define ES8388_REG_DACCONTROL17     0x27
#define ES8388_REG_DACCONTROL20     0x2A
#define ES8388_REG_DACCONTROL21     0x2B
#define ES8388_REG_DACCONTROL23     0x2D
#define ES8388_REG_DACCONTROL24     0x2E  /* LOUT1 volume 0..0x21, default 0x1E = 0dB */
#define ES8388_REG_DACCONTROL25     0x2F  /* ROUT1 volume */
#define ES8388_REG_DACCONTROL26     0x30  /* LOUT2 volume */
#define ES8388_REG_DACCONTROL27     0x31  /* ROUT2 volume */

static i2c_master_bus_handle_t s_bus_handle = NULL;
static i2c_master_dev_handle_t s_dev_handle = NULL;
static bool s_initialized = false;

/* ------------------------------------------------------------------ I2C ------ */

static esp_err_t es8388_i2c_open(void)
{
    if (s_bus_handle) return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = CONFIG_AUDIO_CODEC_I2C_PORT,
        .sda_io_num = CONFIG_AUDIO_CODEC_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_AUDIO_CODEC_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        /* Higher glitch tolerance helps on long I2C lines or boards with
         * weaker pull-ups. ESP-IDF's default is 7; the maximum is 7 on the
         * classic ESP32 (it's a 3-bit field). Keep at 7. */
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8388_I2C_ADDR,
        .scl_speed_hz = CONFIG_AUDIO_CODEC_I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s", esp_err_to_name(err));
        i2c_del_master_bus(s_bus_handle);
        s_bus_handle = NULL;
        return err;
    }
    return ESP_OK;
}

static esp_err_t es8388_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    /* CRITICAL: i2c_master_transmit takes timeout in MILLISECONDS (the
     * "xfer_timeout_ms" parameter), NOT in FreeRTOS ticks like many older
     * ESP-IDF APIs. Using `100 / portTICK_PERIOD_MS` here would evaluate
     * to 10 ms at the default 100 Hz tick rate, causing spurious timeouts
     * any time the chip clock-stretches its ACK by more than 10 ms (which
     * the ES8388 does during certain register-write state changes). Pass
     * 1000 ms — the actual transaction is sub-millisecond; the long
     * timeout is only a safety net. */
    return i2c_master_transmit(s_dev_handle, buf, sizeof(buf), 1000);
}

static esp_err_t es8388_read_reg(uint8_t reg, uint8_t *out)
{
    /* See es8388_write_reg above: timeout parameter is milliseconds. */
    return i2c_master_transmit_receive(s_dev_handle, &reg, 1, out, 1, 1000);
}

/* ----------------------------------------------------------- public API ------ */

esp_err_t es8388_init(uint32_t sample_rate)
{
    (void)sample_rate;  /* MCLK ratio fixed at 256xFs by the I2S APLL config */

    if (s_initialized) {
        ESP_LOGW(TAG, "es8388_init called when already initialized — ignoring");
        return ESP_OK;
    }

    esp_err_t err = es8388_i2c_open();
    if (err != ESP_OK) return err;

    /* Probe: read CONTROL1. A bus with no ES8388 returns NACK → I2C error. */
    uint8_t probe = 0xFF;
    err = es8388_read_reg(ES8388_REG_CONTROL1, &probe);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ES8388 not detected at I2C addr 0x%02X: %s",
                 ES8388_I2C_ADDR, esp_err_to_name(err));
        if (s_dev_handle) { i2c_master_bus_rm_device(s_dev_handle); s_dev_handle = NULL; }
        if (s_bus_handle) { i2c_del_master_bus(s_bus_handle);       s_bus_handle = NULL; }
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "ES8388 detected (CONTROL1=0x%02X)", probe);

    /* WR() logs EVERY failed write so we can see the full failure pattern,
     * not just the first one. Also tracks first-error register for the
     * post-sequence summary. */
    esp_err_t first_err = ESP_OK;
    int fail_count = 0;
    #define WR(reg, val) do {                                                  \
        esp_err_t _e = es8388_write_reg((reg), (val));                         \
        if (_e != ESP_OK) {                                                    \
            ESP_LOGE(TAG, "  reg 0x%02X = 0x%02X failed: %s",                  \
                     (reg), (val), esp_err_to_name(_e));                       \
            if (first_err == ESP_OK) first_err = _e;                           \
            fail_count++;                                                      \
        }                                                                      \
    } while (0)

    /* Init sequence — adapted from squeezelite-esp32 with two changes that
     * empirically fix I2C timeouts on AI-Thinker ESP32-A1S (ES8388 variant):
     *
     *  1. Establish CONTROL1 (analog ref / VMID) BEFORE the disruptive
     *     CHIPPOWER write. Some chip revisions need VMID active before any
     *     power-state transition, otherwise the I2C ACK timing degrades.
     *  2. Skip the "power down everything to 0xF3" step entirely. Going
     *     directly to the final 0xAA power state (DAC up, ADC down)
     *     avoids the intermediate state where chip is half-off and slow
     *     to ACK on the I2C bus. */
    WR(ES8388_REG_CONTROL1,     0x05);  /* VMID on, enable analog ref (do this FIRST) */
    WR(ES8388_REG_CONTROL2,     0x40);  /* low-power mode select */
    WR(ES8388_REG_MASTERMODE,   0x00);  /* slave mode (ESP32 is I2S master) */
    WR(ES8388_REG_DACCONTROL21, 0x80);  /* same LRCK for ADC/DAC, enable MCLK input */
    WR(ES8388_REG_DACPOWER,     0x3C);  /* enable DAC L+R + LOUT1/2 + ROUT1/2 */
    WR(ES8388_REG_DACCONTROL1,  0x18);  /* 16-bit word length, I2S format */
    WR(ES8388_REG_DACCONTROL2,  0x02);  /* MCLK/Fs ratio = 256 */
    WR(ES8388_REG_DACCONTROL4,  0x00);  /* LDAC volume = 0 dB */
    WR(ES8388_REG_DACCONTROL5,  0x00);  /* RDAC volume = 0 dB */
    WR(ES8388_REG_DACCONTROL3,  0x32);  /* auto-mute + soft ramp on volume change */
    WR(ES8388_REG_DACCONTROL16, 0x00);  /* output mixer: no LIN/RIN source */
    WR(ES8388_REG_DACCONTROL17, 0xB8);  /* LMIX = LDAC, +0dB */
    WR(ES8388_REG_DACCONTROL20, 0xB8);  /* RMIX = RDAC, +0dB */
    WR(ES8388_REG_DACCONTROL24, 0x1E);  /* LOUT1 volume = 0 dB */
    WR(ES8388_REG_DACCONTROL25, 0x1E);  /* ROUT1 volume = 0 dB */
    WR(ES8388_REG_DACCONTROL26, 0x1E);  /* LOUT2 volume = 0 dB */
    WR(ES8388_REG_DACCONTROL27, 0x1E);  /* ROUT2 volume = 0 dB */
    WR(ES8388_REG_ADCPOWER,     0xFF);  /* power down ADC entirely */
    WR(ES8388_REG_CHIPPOWER,    0xAA);  /* DAC L+R powered up, ADC stays down */

    if (fail_count > 0) {
        ESP_LOGE(TAG, "ES8388 init: %d/19 writes failed; first was %s",
                 fail_count, esp_err_to_name(first_err));
    }
    #undef WR

    s_initialized = true;
    ESP_LOGI(TAG, "ES8388 init complete (sample_rate=%u Hz, expects MCLK=256×Fs)",
             (unsigned)sample_rate);
    return ESP_OK;
}

esp_err_t es8388_set_sample_rate(uint32_t sample_rate)
{
    /* ES8388 derives Fs from the incoming MCLK. With MCLK/Fs ratio of 256
     * (set in DACCONTROL2 above), changing sample rate is purely a function
     * of the I2S APLL — no codec register changes needed. */
    (void)sample_rate;
    return ESP_OK;
}

esp_err_t es8388_set_volume(float volume)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    /* Map 0.0..1.0 to LOUT1/ROUT1/LOUT2/ROUT2 output volume.
     * Per datasheet: register values 0..0x21, where 0x1E = 0 dB and each
     * step = 1.5 dB. We clip the upper bound to 0x1E so we never amplify
     * above unity gain (avoids clipping the analog stage). */
    uint8_t v = (uint8_t)(volume * 0x1E + 0.5f);

    esp_err_t err;
    if ((err = es8388_write_reg(ES8388_REG_DACCONTROL24, v)) != ESP_OK) return err;
    if ((err = es8388_write_reg(ES8388_REG_DACCONTROL25, v)) != ESP_OK) return err;
    if ((err = es8388_write_reg(ES8388_REG_DACCONTROL26, v)) != ESP_OK) return err;
    if ((err = es8388_write_reg(ES8388_REG_DACCONTROL27, v)) != ESP_OK) return err;
    return ESP_OK;
}

esp_err_t es8388_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    /* Mute and power down for clean shutdown. */
    es8388_write_reg(ES8388_REG_DACCONTROL3, 0x06);  /* mute both DAC channels */
    es8388_write_reg(ES8388_REG_DACPOWER,    0xC0);  /* power down both outputs */
    es8388_write_reg(ES8388_REG_CHIPPOWER,   0xFF);  /* power down chip */

    if (s_dev_handle) {
        i2c_master_bus_rm_device(s_dev_handle);
        s_dev_handle = NULL;
    }
    if (s_bus_handle) {
        i2c_del_master_bus(s_bus_handle);
        s_bus_handle = NULL;
    }
    s_initialized = false;
    return ESP_OK;
}

#endif /* CONFIG_AUDIO_DRIVER_ES8388 */
