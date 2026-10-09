/*
 * microSD card (SDSPI) mount. See sdcard.h for the contract.
 *
 * SPI host choice: SPI2_HOST. The DotStar LED backend takes SPI3_HOST
 * (led_strip.c), so this keeps the two off each other even if someone selects
 * DotStar at runtime with SD enabled.
 *
 * Pins come from the runtime settings store, not from #defines, so a board can
 * be re-pinned from the web UI. On the YB-ESP32-S3-DAC they are
 * CS=10, MOSI=11, SCK=12, MISO=13.
 */

#include "sdcard.h"
#include "sdkconfig.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include <string.h>

static const char *TAG = "sdcard";

#define SDCARD_MOUNT_POINT "/sdcard"
#define SDCARD_SPI_HOST    SPI2_HOST

static sdmmc_card_t *s_card   = NULL;
static bool          s_mounted = false;
static bool          s_bus_up  = false;

bool sdcard_is_mounted(void) { return s_mounted; }

const char *sdcard_name(void)
{
    return (s_mounted && s_card) ? s_card->cid.name : "";
}

esp_err_t sdcard_mount(void)
{
    if (s_mounted) return ESP_OK;

    const device_settings_t *cfg = settings_get();
    if (cfg->sd_cs < 0 || cfg->sd_mosi < 0 || cfg->sd_miso < 0 || cfg->sd_clk < 0) {
        ESP_LOGI(TAG, "SD pins not configured (cs=%d mosi=%d miso=%d clk=%d) — skipping",
                 cfg->sd_cs, cfg->sd_mosi, cfg->sd_miso, cfg->sd_clk);
        return ESP_ERR_INVALID_STATE;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = cfg->sd_mosi,
        .miso_io_num     = cfg->sd_miso,
        .sclk_io_num     = cfg->sd_clk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        /* One SD block (512 B) plus slack. Larger buys nothing here and costs
         * DMA-capable internal RAM, which is the scarce pool on this board. */
        .max_transfer_sz = 4096,
    };

    esp_err_t err = spi_bus_initialize(SDCARD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Someone already brought this host up — reuse it rather than failing. */
        ESP_LOGW(TAG, "SPI%d already initialized — reusing", SDCARD_SPI_HOST + 1);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    } else {
        s_bus_up = true;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = (gpio_num_t)cfg->sd_cs;
    slot.host_id = SDCARD_SPI_HOST;

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SDCARD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        /* NEVER true: this is the user's card and may hold their only copy of
         * something. A card we cannot read is reported, not reformatted. */
        .format_if_mount_failed = false,
        /* Concurrent open files: a BG stream + a speech phrase + an HTTP
         * upload, with one spare. */
        .max_files            = 4,
        .allocation_unit_size  = 16 * 1024,
    };

    err = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGW(TAG, "no filesystem on the card (not formatted FAT?) — "
                          "NOT auto-formatting");
        } else {
            ESP_LOGW(TAG, "mount failed: %s (card absent or wiring?) "
                          "cs=%d mosi=%d miso=%d clk=%d",
                     esp_err_to_name(err),
                     cfg->sd_cs, cfg->sd_mosi, cfg->sd_miso, cfg->sd_clk);
        }
        if (s_bus_up) { spi_bus_free(SDCARD_SPI_HOST); s_bus_up = false; }
        s_card = NULL;
        return err;
    }

    s_mounted = true;

    uint64_t total = 0, freeb = 0;
    sdcard_get_space(&total, &freeb);
    ESP_LOGI(TAG, "mounted '%s' at %s — %llu MB total, %llu MB free",
             s_card->cid.name, SDCARD_MOUNT_POINT,
             total / (1024ULL * 1024ULL), freeb / (1024ULL * 1024ULL));
    return ESP_OK;
}

esp_err_t sdcard_unmount(void)
{
    if (!s_mounted) return ESP_OK;
    esp_err_t err = esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
    if (s_bus_up) { spi_bus_free(SDCARD_SPI_HOST); s_bus_up = false; }
    ESP_LOGI(TAG, "unmounted (%s)", esp_err_to_name(err));
    return err;
}

esp_err_t sdcard_get_space(uint64_t *total_bytes, uint64_t *free_bytes)
{
    if (!s_mounted) return ESP_ERR_INVALID_STATE;

    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    /* FATFS wants the drive number as a string; esp_vfs_fat assigns "0:" to
     * the first mounted volume. Using the mount point here would not work. */
    FRESULT fr = f_getfree("0:", &free_clusters, &fs);
    if (fr != FR_OK || !fs) {
        ESP_LOGW(TAG, "f_getfree failed (%d)", fr);
        return ESP_FAIL;
    }
    const uint64_t sector = (uint64_t)fs->ssize;
    const uint64_t total_sectors = (uint64_t)(fs->n_fatent - 2) * fs->csize;
    const uint64_t free_sectors  = (uint64_t)free_clusters * fs->csize;

    if (total_bytes) *total_bytes = total_sectors * sector;
    if (free_bytes)  *free_bytes  = free_sectors  * sector;
    return ESP_OK;
}
