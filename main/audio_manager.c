#include "audio_manager.h"
#include "audio_config.h"
#include "audio_driver.h"
#include "audio_generator.h"  // for NUM_AUDIO_CHANNELS
#include "lock_free_comm.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "audio_manager";

// Global audio manager state
static audio_manager_state_t g_audio_state = {
    .current_source = AUDIO_SOURCE_NONE,
    .state = AUDIO_STATE_STOPPED,
    .volume = AUDIO_DEFAULT_VOLUME,
    .muted = false
};

// I2S configuration
i2s_chan_handle_t tx_handle = NULL;

esp_err_t audio_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing Audio Manager");

    // I2S configuration. We override three fields of the default config:
    //   1. dma_desc_num and dma_frame_num — the ESP-IDF defaults (6 × 240)
    //      don't match AUDIO_DMA_BUFFER_COUNT × AUDIO_DMA_FRAMES_PER_BUF
    //      (8 × 256). The mismatch made AUDIO_DMA_PIPELINE_SAMPLES (and
    //      thus AUDIO_DMA_PIPELINE_LAG_US, which anchors LED-audio sync)
    //      off by ~14 ms. Pinning the I2S ring to the constants is what
    //      keeps the phase pre-advance and LED anchor honest.
    //   2. auto_clear_after_cb — the default is false, meaning an underrun
    //      replays the last DMA descriptor (audible as a brief stutter loop
    //      of ~5 ms of recent audio). true makes the driver emit silence
    //      instead, which is the only acceptable underrun behaviour for
    //      a therapy device.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num        = AUDIO_DMA_BUFFER_COUNT;
    chan_cfg.dma_frame_num       = AUDIO_DMA_FRAMES_PER_BUF;
    chan_cfg.auto_clear_after_cb = true;

    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // I2S standard configuration for audio output. MCLK and DIN map to
    // I2S_GPIO_UNUSED (-1) when the matching CONFIG_AUDIO_I2S_*_GPIO is -1,
    // so the same struct works whether or not MCLK/DIN are wired.
    //
    // Clock source: use APLL when we need a precise MCLK output (most I2C
    // codecs derive their internal state-machine clock from MCLK and will
    // misbehave if MCLK is missing or has poor jitter). The default 160 MHz
    // PLL cannot integer-divide to standard audio rates × 256 — for
    // 44100 × 256 = 11.2896 MHz, you'd get ~11.43 MHz from PLL_160M.
    // APLL has fractional-N synthesis and produces audio rates exactly.
    // squeezelite-esp32 and ESP-ADF both unconditionally use APLL for the
    // same reason.
    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE);
    if (AUDIO_I2S_MCLK_GPIO != GPIO_NUM_NC) {
        clk_cfg.clk_src = I2S_CLK_SRC_APLL;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = clk_cfg,
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_GPIO,
            .bclk = AUDIO_I2S_BCK_GPIO,
            .ws = AUDIO_I2S_WS_GPIO,
            .dout = AUDIO_I2S_DATA_GPIO,
            .din = AUDIO_I2S_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ret = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S standard mode: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialise lock-free comm primitives — used by config_parser's
    // timeline-event ring buffer. Previously bootstrapped from inside
    // audio_led_sync_init(); that module is gone, so the init has moved
    // up here to the next-natural owner.
    ret = lock_free_comm_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "lock_free_comm_init failed: %s — timeline events disabled",
                 esp_err_to_name(ret));
    }

    // Enable I2S BEFORE codec init. Some codecs (ES8388 in particular) need
    // MCLK to be flowing in order to acknowledge clock-related register
    // writes; without it the codec NACKs mid-sequence and we get an I2C
    // software timeout. AC101 derives its PLL from BCK and tolerates either
    // order, so enabling I2S first is the strictly safer choice.
    // Cost: the I2S DMA emits a few buffers of zeros (silence) between this
    // enable and the first audio_test_output_task fill — auto_clear_after_cb
    // above ensures the buffer is zeroed, so no junk noise.
    esp_err_t i2s_en_err = i2s_channel_enable(tx_handle);
    if (i2s_en_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(i2s_en_err));
        return i2s_en_err;
    }

    // Initialize the codec driver. For AUDIO_DRIVER_NONE this is a no-op;
    // for AUDIO_DRIVER_AC101 / ES8388 this brings up I2C and writes the
    // codec's init register sequence.
    ret = audio_driver_init(AUDIO_SAMPLE_RATE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "audio_driver_init failed: %s — running I2C diagnostic", esp_err_to_name(ret));

#if !CONFIG_AUDIO_DRIVER_NONE
        /* Diagnostic mode — the configured codec didn't probe successfully.
         * Tear down I2S and run two I2C bus scans:
         *   1. with the current (configured) I2S pin layout
         *   2. with WS and DO swapped (the common confusion between AC101
         *      and ES8388 wirings on the same board family)
         * I2C is independent of I2S, so the scan results should be identical
         * across the two — divergence would suggest a hardware quirk worth
         * investigating. */
        i2s_del_channel(tx_handle);
        tx_handle = NULL;

        audio_driver_i2c_scan("scan #1 (configured pins)",
                              CONFIG_AUDIO_CODEC_I2C_PORT,
                              CONFIG_AUDIO_CODEC_I2C_SDA_GPIO,
                              CONFIG_AUDIO_CODEC_I2C_SCL_GPIO,
                              CONFIG_AUDIO_CODEC_I2C_FREQ_HZ);

        /* Re-create I2S with WS and DO swapped, then re-scan. */
        ESP_LOGI(TAG, "Re-initializing I2S with WS/DO swapped (was WS=%d DO=%d, now WS=%d DO=%d)",
                 AUDIO_I2S_WS_GPIO, AUDIO_I2S_DATA_GPIO,
                 AUDIO_I2S_DATA_GPIO, AUDIO_I2S_WS_GPIO);
        i2s_chan_config_t alt_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        alt_chan_cfg.dma_desc_num = AUDIO_DMA_BUFFER_COUNT;
        alt_chan_cfg.dma_frame_num = AUDIO_DMA_FRAMES_PER_BUF;
        alt_chan_cfg.auto_clear_after_cb = true;
        if (i2s_new_channel(&alt_chan_cfg, &tx_handle, NULL) == ESP_OK) {
            i2s_std_config_t alt_std_cfg = std_cfg;
            alt_std_cfg.gpio_cfg.ws   = AUDIO_I2S_DATA_GPIO;  /* swapped */
            alt_std_cfg.gpio_cfg.dout = AUDIO_I2S_WS_GPIO;    /* swapped */
            i2s_channel_init_std_mode(tx_handle, &alt_std_cfg);

            audio_driver_i2c_scan("scan #2 (WS/DO swapped)",
                                  CONFIG_AUDIO_CODEC_I2C_PORT,
                                  CONFIG_AUDIO_CODEC_I2C_SDA_GPIO,
                                  CONFIG_AUDIO_CODEC_I2C_SCL_GPIO,
                                  CONFIG_AUDIO_CODEC_I2C_FREQ_HZ);

            i2s_del_channel(tx_handle);
            tx_handle = NULL;
        }

        ESP_LOGE(TAG, "=== diagnostic complete — boot aborted ===");
        ESP_LOGE(TAG, "Compare the addresses found above against:");
        ESP_LOGE(TAG, "  0x10/0x11 = ES8388 / ES8311 (try AUDIO_DRIVER_ES8388, swap WS/DO)");
        ESP_LOGE(TAG, "  0x1A      = AC101 (current driver; check power and wiring)");
        ESP_LOGE(TAG, "  no ACKs   = check SDA/SCL pins, codec power, pull-ups");
#endif
        return ret;
    }

    // Drive the amplifier-enable pin (if configured) HIGH now that the codec
    // is up and I2S is clocking — this avoids the speaker-pop you'd get if
    // you enabled the amp before the codec settled.
    audio_driver_amp_enable(true);

    // Initialize audio generator
    ret = audio_generator_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio generator: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Audio Manager initialized successfully");
    ESP_LOGI(TAG, "I2S pins - BCK: %d, WS: %d, DATA: %d, MCLK: %d, DIN: %d",
             AUDIO_I2S_BCK_GPIO, AUDIO_I2S_WS_GPIO, AUDIO_I2S_DATA_GPIO,
             AUDIO_I2S_MCLK_GPIO, AUDIO_I2S_DIN_GPIO);
    ESP_LOGI(TAG, "Audio format - Sample rate: %d Hz, Bits: %d, Channels: %d",
             AUDIO_SAMPLE_RATE, AUDIO_BITS_PER_SAMPLE, AUDIO_CHANNELS);

    return ESP_OK;
}

esp_err_t audio_manager_play(audio_source_t source, const char* uri)
{
    ESP_LOGI(TAG, "Play request - Source: %d, URI: %s", source, uri ? uri : "NULL");

    if (g_audio_state.state == AUDIO_STATE_PLAYING) {
        ESP_LOGW(TAG, "Already playing, stopping current playback");
        audio_manager_stop();
    }

    g_audio_state.current_source = source;
    g_audio_state.state = AUDIO_STATE_PLAYING;

    // TODO: Implement actual playback logic based on source type
    switch (source) {
        case AUDIO_SOURCE_SDCARD:
            ESP_LOGI(TAG, "Starting SD card playback: %s", uri);
            // TODO: Implement SD card audio file reading and I2S output
            break;

        case AUDIO_SOURCE_RTP:
            ESP_LOGI(TAG, "Starting RTP stream playback");
            // TODO: Implement RTP receiver and I2S output
            break;

        case AUDIO_SOURCE_VBAN:
            ESP_LOGI(TAG, "Starting VBAN stream playback");
            // TODO: Implement VBAN receiver and I2S output
            break;

        case AUDIO_SOURCE_BLUETOOTH:
            ESP_LOGI(TAG, "Starting Bluetooth A2DP playback");
            // TODO: Implement Bluetooth A2DP source and I2S output
            break;

        case AUDIO_SOURCE_GENERATOR:
            ESP_LOGI(TAG, "Starting generated audio playback");
            // Generate audio will be handled by audio generation functions
            break;

        default:
            ESP_LOGE(TAG, "Unsupported audio source: %d", source);
            g_audio_state.state = AUDIO_STATE_ERROR;
            return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

esp_err_t audio_manager_pause(void)
{
    ESP_LOGI(TAG, "Pausing playback");

    if (g_audio_state.state != AUDIO_STATE_PLAYING) {
        ESP_LOGW(TAG, "Not currently playing, ignoring pause request");
        return ESP_ERR_INVALID_STATE;
    }

    g_audio_state.state = AUDIO_STATE_PAUSED;
    // TODO: Implement actual pause logic

    return ESP_OK;
}

esp_err_t audio_manager_resume(void)
{
    ESP_LOGI(TAG, "Resuming playback");

    if (g_audio_state.state != AUDIO_STATE_PAUSED) {
        ESP_LOGW(TAG, "Not currently paused, ignoring resume request");
        return ESP_ERR_INVALID_STATE;
    }

    g_audio_state.state = AUDIO_STATE_PLAYING;
    // TODO: Implement actual resume logic

    return ESP_OK;
}

esp_err_t audio_manager_stop(void)
{
    ESP_LOGI(TAG, "Stopping playback");

    g_audio_state.state = AUDIO_STATE_STOPPED;
    g_audio_state.current_source = AUDIO_SOURCE_NONE;

    // TODO: Implement actual stop logic

    return ESP_OK;
}

esp_err_t audio_manager_set_volume(float volume)
{
    if (volume < 0.0f || volume > 1.0f) {
        ESP_LOGE(TAG, "Invalid volume level: %.2f (should be 0.0-1.0)", volume);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Setting volume to %.2f", volume);
    g_audio_state.volume = volume;

    // TODO: Implement actual volume control (hardware DAC or software scaling)

    return ESP_OK;
}

audio_manager_state_t* audio_manager_get_state(void)
{
    return &g_audio_state;
}

esp_err_t audio_manager_start_generation(int channel, const audio_gen_params_t* params)
{
    ESP_LOGI(TAG, "Starting audio generation on channel %d", channel);

    if (!params) {
        return ESP_ERR_INVALID_ARG;
    }

    // Start audio generation
    esp_err_t ret = audio_generator_start_channel(channel, params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start audio generation: %s", esp_err_to_name(ret));
        return ret;
    }

    // If this is the first active generator, switch to generator source
    if (g_audio_state.current_source != AUDIO_SOURCE_GENERATOR) {
        g_audio_state.current_source = AUDIO_SOURCE_GENERATOR;
        g_audio_state.state = AUDIO_STATE_PLAYING;
    }

    ESP_LOGI(TAG, "Audio generation started: freq=%.1f Hz, amp=%.2f, dur=%u ms",
             params->frequency, params->amplitude, params->duration_ms);

    return ESP_OK;
}

esp_err_t audio_manager_stop_generation(int channel)
{
    ESP_LOGI(TAG, "Stopping audio generation on channel %d", channel);

    esp_err_t ret = audio_generator_stop_channel(channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to stop audio generation: %s", esp_err_to_name(ret));
        return ret;
    }

    // Check if any generators are still active
    bool any_active = false;
    for (int i = 0; i < NUM_AUDIO_CHANNELS; i++) {
        if (audio_generator_is_active(i)) {
            any_active = true;
            break;
        }
    }

    // If no generators are active, stop audio source
    if (!any_active && g_audio_state.current_source == AUDIO_SOURCE_GENERATOR) {
        g_audio_state.current_source = AUDIO_SOURCE_NONE;
        g_audio_state.state = AUDIO_STATE_STOPPED;
        ESP_LOGI(TAG, "All audio generators stopped");
    }

    return ESP_OK;
}

bool audio_manager_is_generating(int channel)
{
    return audio_generator_is_active(channel);
}

bool audio_manager_is_channel_active(int channel)
{
    return audio_generator_is_active(channel);
}

esp_err_t audio_manager_update_generation(int channel, const audio_gen_params_t *params)
{
    if (!params) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = audio_generator_update_params(channel, params);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to update audio generation on channel %d: %s",
                 channel, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "Audio generation updated: channel=%d freq=%.1f Hz amp=%.2f",
             channel, params->frequency, params->amplitude);

    return ESP_OK;
}
