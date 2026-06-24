/*
 * AC101 codec driver — playback-only.
 *
 * Ported from squeezelite-esp32's BSD-licensed AC101 driver
 * (components/squeezelite/ac101/ac101.c). Stripped to the playback path:
 * no ADC, no microphone, no headset-jack detection. Headphone and speaker
 * outputs are both enabled.
 *
 * The AC101 is a 16-bit-register I2C codec at address 0x1A. Each register
 * read/write transfers two bytes (MSB first) after a single-byte register
 * address.
 *
 * The PLL is sourced from BCLK (ESP32 acts as I2S master, codec as slave),
 * so no MCLK output is required from the ESP32 — leave CONFIG_AUDIO_I2S_MCLK_GPIO
 * at -1 unless your board specifically routes MCLK to the codec.
 */

#include "sdkconfig.h"

#if CONFIG_AUDIO_DRIVER_AC101

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ac101";

#define AC101_I2C_ADDR              0x1A

/* AC101 register addresses (subset — playback path only) */
#define AC101_REG_CHIP_AUDIO_RS     0x00
#define AC101_REG_PLL_CTRL1         0x01
#define AC101_REG_PLL_CTRL2         0x02
#define AC101_REG_SYSCLK_CTRL       0x03
#define AC101_REG_MOD_CLK_ENA       0x04
#define AC101_REG_MOD_RST_CTRL      0x05
#define AC101_REG_I2S_SR_CTRL       0x06
#define AC101_REG_I2S1LCK_CTRL      0x10
#define AC101_REG_I2S1_SDOUT_CTRL   0x11
#define AC101_REG_I2S1_SDIN_CTRL    0x12
#define AC101_REG_I2S1_MXR_SRC      0x13
#define AC101_REG_ADC_DIG_CTRL      0x40
#define AC101_REG_DAC_DIG_CTRL      0x48
#define AC101_REG_DAC_MXR_SRC       0x4C
#define AC101_REG_ADC_ANA_CTRL      0x50
#define AC101_REG_ADC_SRC           0x51
#define AC101_REG_ADC_SRCBST_CTRL   0x52
#define AC101_REG_OMIXER_DACA_CTRL  0x53
#define AC101_REG_OMIXER_SR         0x54
#define AC101_REG_HPOUT_CTRL        0x56
#define AC101_REG_SPKOUT_CTRL       0x58

/* Sample-rate enum values for I2S_SR_CTRL (high nibble). */
#define AC101_SR_8000               0x0000
#define AC101_SR_11025              0x1000
#define AC101_SR_12000              0x2000
#define AC101_SR_16000              0x3000
#define AC101_SR_22050              0x4000
#define AC101_SR_24000              0x5000
#define AC101_SR_32000              0x6000
#define AC101_SR_44100              0x7000
#define AC101_SR_48000              0x8000

static i2c_master_bus_handle_t s_bus_handle = NULL;
static i2c_master_dev_handle_t s_dev_handle = NULL;
static bool s_initialized = false;

/* ------------------------------------------------------------------ I2C ------ */

static esp_err_t ac101_i2c_open(void)
{
    if (s_bus_handle) return ESP_OK;  /* already open */

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = CONFIG_AUDIO_CODEC_I2C_PORT,
        .sda_io_num = CONFIG_AUDIO_CODEC_I2C_SDA_GPIO,
        .scl_io_num = CONFIG_AUDIO_CODEC_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
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
        .device_address = AC101_I2C_ADDR,
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

static esp_err_t ac101_write_reg(uint8_t reg, uint16_t value)
{
    /* AC101 wants the high byte first, then the low byte.
     * Timeout is in milliseconds (NOT ticks) — see ES8388 driver for the
     * full explanation of this ESP-IDF API gotcha. Pass 1000 ms as a
     * generous safety net; actual transactions complete in <1 ms. */
    uint8_t buf[3] = { reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    return i2c_master_transmit(s_dev_handle, buf, sizeof(buf), 1000);
}

static esp_err_t ac101_read_reg(uint8_t reg, uint16_t *out)
{
    uint8_t rx[2] = {0};
    /* Timeout in ms (not ticks) — same gotcha as ac101_write_reg above. */
    esp_err_t err = i2c_master_transmit_receive(s_dev_handle, &reg, 1, rx, 2, 1000);
    if (err == ESP_OK) {
        *out = ((uint16_t)rx[0] << 8) | rx[1];
    }
    return err;
}

/* ----------------------------------------------------------- public API ------ */

static esp_err_t ac101_apply_sample_rate(uint32_t sample_rate)
{
    uint16_t sr;
    switch (sample_rate) {
        case 8000:  sr = AC101_SR_8000;  break;
        case 11025: sr = AC101_SR_11025; break;
        case 12000: sr = AC101_SR_12000; break;
        case 16000: sr = AC101_SR_16000; break;
        case 22050: sr = AC101_SR_22050; break;
        case 24000: sr = AC101_SR_24000; break;
        case 32000: sr = AC101_SR_32000; break;
        case 44100: sr = AC101_SR_44100; break;
        case 48000: sr = AC101_SR_48000; break;
        default:
            ESP_LOGW(TAG, "Unsupported sample rate %u; falling back to 44100",
                     (unsigned)sample_rate);
            sr = AC101_SR_44100;
            break;
    }
    return ac101_write_reg(AC101_REG_I2S_SR_CTRL, sr);
}

esp_err_t ac101_init(uint32_t sample_rate)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "ac101_init called when already initialized — ignoring");
        return ESP_OK;
    }

    esp_err_t err = ac101_i2c_open();
    if (err != ESP_OK) return err;

    /* Probe — read CHIP_AUDIO_RS. A bus with no AC101 returns 0xFFFF (open
     * bus reads back as all-ones on most ESP32 I2C peripherals with internal
     * pull-ups). */
    uint16_t probe = 0xFFFF;
    err = ac101_read_reg(AC101_REG_CHIP_AUDIO_RS, &probe);
    if (err != ESP_OK || probe == 0xFFFF) {
        ESP_LOGE(TAG, "AC101 not detected at I2C addr 0x%02X (read=0x%04X, err=%s)",
                 AC101_I2C_ADDR, probe, esp_err_to_name(err));
        /* Tear down the I2C bus so the diagnostic scanner in audio_manager
         * can re-open it. Without this, the second i2c_new_master_bus call
         * would fail because the port is still claimed. */
        if (s_dev_handle) { i2c_master_bus_rm_device(s_dev_handle); s_dev_handle = NULL; }
        if (s_bus_handle) { i2c_del_master_bus(s_bus_handle);       s_bus_handle = NULL; }
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "AC101 detected (CHIP_AUDIO_RS=0x%04X)", probe);

    /* Soft reset and let the chip stabilize. */
    ac101_write_reg(AC101_REG_CHIP_AUDIO_RS, 0x0123);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* PLL: source from BCLK (ESP32 is I2S master, codec is slave).
     * Values are copied verbatim from squeezelite's working configuration. */
    ac101_write_reg(AC101_REG_PLL_CTRL1,        0x014F); /* F=1, M=1, INT=31 */
    ac101_write_reg(AC101_REG_PLL_CTRL2,        0x8200); /* PLL on, N_i=64 */

    /* System clock: source from PLL, drive I2S1 from BCLK1. */
    ac101_write_reg(AC101_REG_SYSCLK_CTRL,      0xAA08);
    ac101_write_reg(AC101_REG_MOD_CLK_ENA,      0x800C); /* I2S1 + ADC + DAC clocks */
    ac101_write_reg(AC101_REG_MOD_RST_CTRL,     0x800C); /* release reset */

    /* Sample rate. */
    ac101_apply_sample_rate(sample_rate);

    /* I2S1 format: slave, BCLK = I2S/8, LRCK = 32×, 16-bit, I2S mode, stereo. */
    ac101_write_reg(AC101_REG_I2S1LCK_CTRL,     0x8850);
    ac101_write_reg(AC101_REG_I2S1_SDOUT_CTRL,  0xC000); /* I2S1 ADC L+R out (unused) */
    ac101_write_reg(AC101_REG_I2S1_SDIN_CTRL,   0xC000); /* I2S1 DAC L+R in */
    ac101_write_reg(AC101_REG_I2S1_MXR_SRC,     0x2200);

    /* ADC fully disabled (playback-only). */
    ac101_write_reg(AC101_REG_ADC_SRCBST_CTRL,  0x4440);
    ac101_write_reg(AC101_REG_ADC_SRC,          0x0000);
    ac101_write_reg(AC101_REG_ADC_DIG_CTRL,     0x0000);
    ac101_write_reg(AC101_REG_ADC_ANA_CTRL,     0x3300);

    /* DAC path. */
    ac101_write_reg(AC101_REG_DAC_MXR_SRC,      0x8800); /* DAC source = I2S */
    ac101_write_reg(AC101_REG_DAC_DIG_CTRL,     0x8000); /* enable digital DAC */
    ac101_write_reg(AC101_REG_OMIXER_DACA_CTRL, 0xF000); /* enable analogue DAC */
    ac101_write_reg(AC101_REG_OMIXER_DACA_CTRL, 0xFF00); /* toggle for PA offset */
    ac101_write_reg(AC101_REG_OMIXER_SR,        0x050A); /* DAC L/R + LINEIN L/R */

    /* Enable both speaker and headphone outputs at moderate volume. */
    ac101_write_reg(AC101_REG_SPKOUT_CTRL,      0x0220);
    ac101_write_reg(AC101_REG_HPOUT_CTRL,       0xF801);

    s_initialized = true;
    ESP_LOGI(TAG, "AC101 init complete (sample_rate=%u Hz)", (unsigned)sample_rate);
    return ESP_OK;
}

esp_err_t ac101_set_sample_rate(uint32_t sample_rate)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return ac101_apply_sample_rate(sample_rate);
}

esp_err_t ac101_set_volume(float volume)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    /* Clamp and map 0.0..1.0 -> AC101 headphone volume 0..63 (6-bit field
     * at bits [10:4] of HPOUT_CTRL — wait, actually bits [4:9] per the
     * squeezelite driver's `<<4` shift with a 0x3F mask: 6 bits starting
     * at bit 4). */
    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;
    uint16_t hp_vol = (uint16_t)(volume * 63.0f + 0.5f);

    uint16_t current = 0;
    esp_err_t err = ac101_read_reg(AC101_REG_HPOUT_CTRL, &current);
    if (err != ESP_OK) return err;

    /* Clear the volume field (bits 4..9 = 0x3F0) and re-insert. */
    uint16_t updated = (current & ~(0x3F << 4)) | ((hp_vol & 0x3F) << 4);
    return ac101_write_reg(AC101_REG_HPOUT_CTRL, updated);
}

esp_err_t ac101_deinit(void)
{
    if (!s_initialized) return ESP_OK;

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

#endif /* CONFIG_AUDIO_DRIVER_AC101 */
