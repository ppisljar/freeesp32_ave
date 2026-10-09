/*
 * TI TLV320DAC3101 codec driver — playback-only.
 *
 * Used on the YelloByte YB-ESP32-S3-DAC board: stereo DAC feeding both a
 * headphone jack and a stereo class-D speaker amp (1.3 W into 8R per channel).
 * I2C control at 7-bit address 0x18; audio over I2S with the codec as SLAVE
 * (the ESP32 is I2S master and drives BCLK/WCLK).
 *
 * Register model: the chip is PAGED. Register 0x00 on every page is
 * PAGE_SELECT; writing a page number there re-points every other register
 * address at that bank. Playback init touches page 0 (clocks, I2S format, DAC
 * power, digital volume) and page 1 (analog routing, output drivers, analog
 * volume). Pages 8/9/12/13 hold BiQuad/DRC coefficient RAM and are unused here.
 *
 * CLOCKING — why BCLK and not MCLK:
 *   On this board GPIO4 reaches the codec's MCLK pin only through solder bridge
 *   JP2, which ships OPEN, so by default the codec receives no MCLK at all. We
 *   therefore run the codec PLL from BCLK, which is exactly what the board
 *   vendor's own shipping example does. BCLK is Fs x 32 here because
 *   audio_manager configures I2S as 16-bit stereo (32 bits per frame).
 *   If JP2 is closed and CONFIG_AUDIO_I2S_MCLK_GPIO is set, the ESP32 emits
 *   MCLK at 256xFs and the MCLK clock tree below is used instead.
 *
 * Sources: TI datasheet SLAS666B (register tables, and the worked example in
 * "Example Register Setup to Play Digital Data Through DAC and Headphone/
 * Speaker Outputs"), plus the per-sample-rate BCLK divider table from the board
 * vendor's TLV320DAC3101 library. Divergences from the DAC3100 (which has a
 * MONO class-D amp) are called out inline — the 3101's right-speaker registers
 * are the main difference.
 */

#include "sdkconfig.h"

#if CONFIG_AUDIO_SUPPORT_TLV320DAC3101

#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings.h"
#include <math.h>
#include <string.h>

static const char *TAG = "tlv320dac3101";

#define TLV_I2C_ADDR            0x18

/* ---- Page 0: clocks, interface, DAC ---- */
#define REG_PAGE_SELECT         0x00   /* present on every page */
#define P0_RESET                0x01
#define P0_CLOCK_MUX1           0x04
#define P0_PLL_PR               0x05   /* bit7 power, bits6:4 P, bits3:0 R */
#define P0_PLL_J                0x06
#define P0_PLL_D_MSB            0x07
#define P0_PLL_D_LSB            0x08
#define P0_NDAC                 0x0B   /* bit7 power, bits6:0 divider */
#define P0_MDAC                 0x0C
#define P0_DOSR_MSB             0x0D
#define P0_DOSR_LSB             0x0E
#define P0_CODEC_IF_CTRL1       0x1B
#define P0_DAC_PRB              0x3C
#define P0_DAC_DATAPATH         0x3F
#define P0_DAC_VOL_CTRL         0x40   /* mute bits */
#define P0_DAC_VOL_L            0x41   /* signed, 0.5 dB/step */
#define P0_DAC_VOL_R            0x42

/* ---- Page 1: analog ---- */
#define P1_HP_SPK_ERR_CTL       0x1E
#define P1_HP_DRIVERS           0x1F   /* common-mode, then HP power-up */
#define P1_SPK_AMP              0x20
#define P1_HP_POP               0x21
#define P1_OUT_ROUTING          0x23
#define P1_HPL_ANALOG_VOL       0x24   /* bit7 route-enable, bits6:0 gain code */
#define P1_HPR_ANALOG_VOL       0x25
#define P1_SPKL_ANALOG_VOL      0x26
#define P1_SPKR_ANALOG_VOL      0x27   /* DAC3101-only (3100 speaker is mono) */
#define P1_HPL_DRIVER           0x28   /* bits4:3 PGA gain, bit2 unmute */
#define P1_HPR_DRIVER           0x29
#define P1_SPKL_DRIVER          0x2A
#define P1_SPKR_DRIVER          0x2B   /* DAC3101-only */

/* Analog volume gain code used for the output stages at init. 0x92 = route
 * enabled (bit7) + code 0x12 (-9 dB per the datasheet's gain table). Leaves
 * headroom; fine-grained level control is done digitally in set_volume(). */
#define ANALOG_VOL_MINUS_9DB    0x92

/* Digital DAC volume limits, in half-dB steps (the register unit). */
#define DAC_VOL_MIN_HALF_DB     (-127)  /* -63.5 dB */
#define DAC_VOL_MAX_HALF_DB     (0)     /* 0 dB — we never apply digital boost */

static i2c_master_bus_handle_t s_bus_handle = NULL;
static i2c_master_dev_handle_t s_dev_handle = NULL;
static bool s_initialized = false;
static uint8_t s_current_page = 0xFF;   /* force a page write on first access */

/* Dispatched through audio_driver.c, which declares these itself; repeated here
 * because init() applies the startup volume before set_volume() is defined. */
esp_err_t tlv320dac3101_init(uint32_t sample_rate);
esp_err_t tlv320dac3101_set_sample_rate(uint32_t sample_rate);
esp_err_t tlv320dac3101_set_volume(float volume);
esp_err_t tlv320dac3101_deinit(void);

/* --------------------------------------------------------- hardware reset --- */

/* Pulse the codec's active-low ~RESET pin before any I2C traffic.
 *
 * This is NOT optional on a board that wires ~RESET to a GPIO: the pin comes up
 * floating, the codec stays held in reset, and it then ACKs nothing — the whole
 * bus scans as empty, which reads like a wiring fault. The software reset in
 * tlv320dac3101_init() cannot substitute, because it is itself an I2C write.
 *
 * Timing per TI SLAS666B: ~RESET must be held low (the datasheet's minimum is
 * ~10 ns, but it recommends low while the supplies settle), and the control
 * interface needs >=1 ms after release before it is addressable. The 10 ms /
 * 10 ms used here is far inside both and costs nothing at boot. */
static void tlv_hw_reset(void)
{
    int pin = settings_get()->codec_reset_pin;
    if (pin < 0) {
        ESP_LOGD(TAG, "no codec reset GPIO configured — skipping hardware reset");
        return;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    gpio_set_level((gpio_num_t)pin, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)pin, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "hardware reset pulsed on GPIO %d", pin);
}

/* ------------------------------------------------------------------ I2C ------ */

static esp_err_t tlv_i2c_open(void)
{
    if (s_bus_handle) return ESP_OK;

    const device_settings_t *cfg = settings_get();
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = cfg->codec_i2c_port,
        .sda_io_num = cfg->codec_i2c_sda,
        .scl_io_num = cfg->codec_i2c_scl,
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
        .device_address = TLV_I2C_ADDR,
        .scl_speed_hz = cfg->codec_i2c_freq_hz,
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

static void tlv_i2c_close(void)
{
    if (s_dev_handle) { i2c_master_bus_rm_device(s_dev_handle); s_dev_handle = NULL; }
    if (s_bus_handle) { i2c_del_master_bus(s_bus_handle);       s_bus_handle = NULL; }
    s_current_page = 0xFF;
}

/* Raw single-register write (no page handling). Timeout is in MILLISECONDS,
 * not ticks — same ESP-IDF API gotcha the AC101/ES8388 drivers document. */
static esp_err_t tlv_write_raw(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev_handle, buf, sizeof(buf), 1000);
}

static esp_err_t tlv_read_raw(uint8_t reg, uint8_t *out)
{
    return i2c_master_transmit_receive(s_dev_handle, &reg, 1, out, 1, 1000);
}

/* Select a register page, skipping the write if already there. */
static esp_err_t tlv_set_page(uint8_t page)
{
    if (s_current_page == page) return ESP_OK;
    esp_err_t err = tlv_write_raw(REG_PAGE_SELECT, page);
    if (err == ESP_OK) s_current_page = page;
    return err;
}

static esp_err_t tlv_write(uint8_t page, uint8_t reg, uint8_t value)
{
    esp_err_t err = tlv_set_page(page);
    if (err != ESP_OK) return err;
    return tlv_write_raw(reg, value);
}

/* ------------------------------------------------------- clock tree ---------- */

/* PLL + divider settings for one sample rate. CODEC_CLKIN = NDAC*MDAC*DOSR*Fs,
 * and PLL_CLK = PLL_CLKIN * R * J.D / P (D is a DECIMAL fraction out of 10000,
 * not a binary one — a detail some third-party drivers get wrong). Every entry
 * here uses D = 0, so only R/J matter. */
typedef struct {
    uint32_t sample_rate;
    uint8_t  p, r, j;
    uint8_t  ndac, mdac;
    uint16_t dosr;
} tlv_clock_cfg_t;

/* BCLK as PLL input (JP2 open — the as-shipped board wiring). BCLK = Fs*32
 * because I2S runs 16-bit stereo. Values from the board vendor's library. */
static const tlv_clock_cfg_t s_clocks_bclk[] = {
    /* Fs      P  R   J  NDAC MDAC DOSR */
    { 16000,   1, 4, 48,   8,   4, 192 },
    { 22050,   1, 4, 36,  12,   2, 192 },
    { 32000,   1, 2, 48,   6,   4, 128 },
    { 44100,   1, 2, 32,   4,   4, 128 },
    { 48000,   1, 2, 32,   4,   4, 128 },
    { 88200,   1, 2, 16,   2,   8,  64 },
    { 96000,   1, 2, 16,   2,   8,  64 },
};

/* MCLK at 256*Fs as PLL input (only reachable with solder bridge JP2 closed
 * AND CONFIG_AUDIO_I2S_MCLK_GPIO set). 44.1k/48k values are TI's own worked
 * example; the rest follow the same NDAC*MDAC*DOSR = 256 relation. */
static const tlv_clock_cfg_t s_clocks_mclk[] = {
    /* Fs      P  R   J  NDAC MDAC DOSR */
    { 16000,   1, 1,  8,   8,   2, 128 },
    { 22050,   1, 1,  8,   8,   2, 128 },
    { 32000,   1, 1,  8,   8,   2, 128 },
    { 44100,   1, 1,  8,   8,   2, 128 },
    { 48000,   1, 1,  8,   8,   2, 128 },
};

static bool tlv_using_mclk(void)
{
    return settings_get()->i2s_mclk_pin >= 0;
}

static const tlv_clock_cfg_t *tlv_find_clock(uint32_t sample_rate)
{
    bool mclk = tlv_using_mclk();
    const tlv_clock_cfg_t *tbl = mclk ? s_clocks_mclk : s_clocks_bclk;
    size_t n = mclk ? (sizeof(s_clocks_mclk) / sizeof(s_clocks_mclk[0]))
                    : (sizeof(s_clocks_bclk) / sizeof(s_clocks_bclk[0]));
    for (size_t i = 0; i < n; i++) {
        if (tbl[i].sample_rate == sample_rate) return &tbl[i];
    }
    return NULL;
}

static esp_err_t tlv_apply_clocks(uint32_t sample_rate)
{
    const tlv_clock_cfg_t *c = tlv_find_clock(sample_rate);
    if (!c) {
        ESP_LOGW(TAG, "no clock entry for %u Hz — falling back to 44100",
                 (unsigned)sample_rate);
        c = tlv_find_clock(44100);
        if (!c) return ESP_ERR_INVALID_ARG;
    }

    /* CLOCK_MUX1: PLL_CLKIN in bits3:2 (00=MCLK, 01=BCLK), CODEC_CLKIN in
     * bits1:0 (11=PLL_CLK). We always clock the codec from its PLL. */
    uint8_t mux = tlv_using_mclk() ? 0x03 /* MCLK->PLL */ : 0x07 /* BCLK->PLL */;
    tlv_write(0, P0_CLOCK_MUX1, mux);

    tlv_write(0, P0_PLL_J,     c->j);
    tlv_write(0, P0_PLL_D_MSB, 0x00);
    tlv_write(0, P0_PLL_D_LSB, 0x00);
    /* Power the PLL on and latch P/R in the same write. */
    tlv_write(0, P0_PLL_PR, 0x80 | ((c->p & 0x07) << 4) | (c->r & 0x0F));

    /* The PLL needs ~10 ms to lock before anything downstream is valid. */
    vTaskDelay(pdMS_TO_TICKS(15));

    tlv_write(0, P0_NDAC,     0x80 | (c->ndac & 0x7F));
    tlv_write(0, P0_MDAC,     0x80 | (c->mdac & 0x7F));
    tlv_write(0, P0_DOSR_MSB, (uint8_t)(c->dosr >> 8));
    tlv_write(0, P0_DOSR_LSB, (uint8_t)(c->dosr & 0xFF));

    ESP_LOGI(TAG, "clocks: %u Hz via %s — P=%u R=%u J=%u NDAC=%u MDAC=%u DOSR=%u",
             (unsigned)c->sample_rate, tlv_using_mclk() ? "MCLK" : "BCLK",
             c->p, c->r, c->j, c->ndac, c->mdac, (unsigned)c->dosr);
    return ESP_OK;
}

/* ----------------------------------------------------------- public API ------ */

esp_err_t tlv320dac3101_init(uint32_t sample_rate)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "tlv320dac3101_init called when already initialized — ignoring");
        return ESP_OK;
    }

    /* Bring the codec out of reset BEFORE opening the bus — see tlv_hw_reset(). */
    tlv_hw_reset();

    esp_err_t err = tlv_i2c_open();
    if (err != ESP_OK) return err;

    /* Probe: select page 0, then read the page register back. A missing codec
     * NACKs the write; a device that ACKs but isn't a TLV320 is unlikely to
     * echo the page we just set (an open bus reads back 0xFF). */
    s_current_page = 0xFF;
    uint8_t page_readback = 0xFF;
    if (tlv_set_page(0) != ESP_OK ||
        tlv_read_raw(REG_PAGE_SELECT, &page_readback) != ESP_OK ||
        page_readback != 0x00) {
        ESP_LOGE(TAG, "TLV320DAC3101 not detected at I2C addr 0x%02X "
                 "(page readback=0x%02X)", TLV_I2C_ADDR, page_readback);
        tlv_i2c_close();
        return ESP_ERR_NOT_FOUND;
    }

    /* Software reset, then let the chip settle. This self-clears and also
     * powers the PLL down, so all clock config must come after it. */
    tlv_write(0, P0_RESET, 0x01);
    vTaskDelay(pdMS_TO_TICKS(10));
    s_current_page = 0;   /* reset leaves us on page 0 */

    err = tlv_apply_clocks(sample_rate);
    if (err != ESP_OK) {
        tlv_i2c_close();
        return err;
    }

    /* I2S format, 16-bit, BCLK/WCLK as INPUTS => codec is the I2S slave,
     * which is what we want with the ESP32 as master. */
    tlv_write(0, P0_CODEC_IF_CTRL1, 0x00);
    /* Processing block PRB_P11 — the datasheet example's choice for stereo
     * playback with the standard interpolation filter. */
    tlv_write(0, P0_DAC_PRB, 0x0B);

    /* ---- Analog section (page 1) ------------------------------------- */
    tlv_write(1, P1_HP_DRIVERS, 0x04);   /* common-mode voltage 1.35 V */
    tlv_write(1, P1_HP_POP,     0x4E);   /* headphone de-pop ramp timing */
    tlv_write(1, P1_OUT_ROUTING, 0x44);  /* DAC L->HPL, DAC R->HPR */

    /* Output driver gain + unmute. 0x06 = 0 dB PGA, unmuted (bit2).
     * 0x1C on the class-D drivers is the datasheet example's value; note the
     * datasheet's own prose labels it 18 dB while its bit-field table decodes
     * the same byte as 24 dB. The byte to write is not in dispute, only the
     * label, so we keep TI's value. */
    tlv_write(1, P1_HPL_DRIVER,  0x06);
    tlv_write(1, P1_HPR_DRIVER,  0x06);
    tlv_write(1, P1_SPKL_DRIVER, 0x1C);
    tlv_write(1, P1_SPKR_DRIVER, 0x1C);  /* 3101 stereo amp; absent on the 3100 */

    /* Power the output drivers. HP: bits7:6. Speaker: bit7 left + bit6 right
     * (the mono DAC3100 only has the left bit, which is why a 3100 driver
     * yields mono speaker output on this part). */
    tlv_write(1, P1_HP_DRIVERS, 0xC2);
    tlv_write(1, P1_SPK_AMP,    0xC6);

    /* Analog volumes: route enabled, -9 dB. Headphone and speaker can run
     * simultaneously at independent levels. */
    tlv_write(1, P1_HPL_ANALOG_VOL,  ANALOG_VOL_MINUS_9DB);
    tlv_write(1, P1_HPR_ANALOG_VOL,  ANALOG_VOL_MINUS_9DB);
    tlv_write(1, P1_SPKL_ANALOG_VOL, ANALOG_VOL_MINUS_9DB);
    tlv_write(1, P1_SPKR_ANALOG_VOL, ANALOG_VOL_MINUS_9DB);

    /* The de-pop ramp above has to finish before the DAC is powered, or the
     * output pops. TI's flow says to wait out the configured de-pop time (or
     * poll page 1 / register 63); the fixed wait is simpler and only costs
     * boot time once. */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* ---- Power up the DAC (page 0) ----------------------------------- */
    tlv_write(0, P0_DAC_DATAPATH, 0xD4);  /* L+R on, soft-stepping enabled */
    tlv_write(0, P0_DAC_VOL_CTRL, 0x00);  /* unmute both channels */

    s_initialized = true;

    /* Apply the configured startup level rather than leaving the DAC at the
     * reset default. */
    const device_settings_t *cfg = settings_get();
    tlv320dac3101_set_volume(cfg->default_volume);

    ESP_LOGI(TAG, "TLV320DAC3101 init complete (%u Hz, headphone + stereo speaker)",
             (unsigned)sample_rate);
    return ESP_OK;
}

esp_err_t tlv320dac3101_set_sample_rate(uint32_t sample_rate)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    return tlv_apply_clocks(sample_rate);
}

esp_err_t tlv320dac3101_set_volume(float volume)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;

    if (volume < 0.0f) volume = 0.0f;
    if (volume > 1.0f) volume = 1.0f;

    /* Digital volume is linear in dB, so map the linear 0..1 gain through
     * 20*log10 to keep the control perceptually even. 0 is a true mute rather
     * than -inf dB. The register is signed in 0.5 dB steps; we never go above
     * 0 dB so a full-scale sample can't clip inside the DAC. */
    int half_db;
    if (volume <= 0.0f) {
        half_db = DAC_VOL_MIN_HALF_DB;
    } else {
        float db = 20.0f * log10f(volume);
        half_db = (int)lrintf(db * 2.0f);
        if (half_db < DAC_VOL_MIN_HALF_DB) half_db = DAC_VOL_MIN_HALF_DB;
        if (half_db > DAC_VOL_MAX_HALF_DB) half_db = DAC_VOL_MAX_HALF_DB;
    }

    uint8_t reg = (uint8_t)(int8_t)half_db;
    esp_err_t err = tlv_write(0, P0_DAC_VOL_L, reg);
    if (err != ESP_OK) return err;
    return tlv_write(0, P0_DAC_VOL_R, reg);
}

esp_err_t tlv320dac3101_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    /* Mute and power the DAC down before dropping the bus, so the amp doesn't
     * pop on the way out. */
    tlv_write(0, P0_DAC_VOL_CTRL, 0x0C);   /* mute L+R */
    tlv_write(0, P0_DAC_DATAPATH, 0x14);   /* DAC channels off */
    tlv_write(1, P1_HP_DRIVERS, 0x02);     /* headphone drivers off */
    tlv_write(1, P1_SPK_AMP,    0x06);     /* class-D off */

    tlv_i2c_close();
    s_initialized = false;
    return ESP_OK;
}

#endif /* CONFIG_AUDIO_SUPPORT_TLV320DAC3101 */
