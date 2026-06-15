#include "audio_manager.h"
#include "audio_config.h"
#include "audio_led_sync.h"
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

    // I2S configuration
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);

    esp_err_t ret = i2s_new_channel(&chan_cfg, &tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // I2S standard configuration for audio output
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_I2S_BCK_GPIO,
            .ws = AUDIO_I2S_WS_GPIO,
            .dout = AUDIO_I2S_DATA_GPIO,
            .din = I2S_GPIO_UNUSED,
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

    // Initialize audio-LED synchronization BEFORE enabling I2S channel
    ret = audio_led_sync_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize audio-LED sync: %s", esp_err_to_name(ret));
        // Continue initialization - sync is optional
    } else {
        // Register I2S callback for sample-accurate synchronization
        // MUST be done before i2s_channel_enable() in ESP-IDF v5.5.2
        ret = audio_led_sync_register_i2s_callback(tx_handle);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to register I2S callback for LED sync: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Audio-LED synchronization enabled with I2S DMA callbacks");
        }
    }

    // Enable I2S channel AFTER callback registration
    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable I2S channel: %s", esp_err_to_name(ret));
        return ret;
    }

    // Initialize audio generator
    ret = audio_generator_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize audio generator: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Audio Manager initialized successfully");
    ESP_LOGI(TAG, "I2S pins - BCK: %d, WS: %d, DATA: %d", AUDIO_I2S_BCK_GPIO, AUDIO_I2S_WS_GPIO, AUDIO_I2S_DATA_GPIO);
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
    for (int i = 0; i < 8; i++) { // Check up to 8 channels
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
