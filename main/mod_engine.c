/*
 * mod_engine — periodic modulation dispatcher for .ledc timeline files.
 *
 * After the refactor to in-consumer evaluation, this module is now a thin
 * adapter between the timeline executor (which speaks in mod_wave_t /
 * mod_led_field_t / mod_audio_field_t) and the LED matrix / audio_generator
 * layers (which now own the per-channel modulation slots and evaluate the
 * wave shapes in their own consumer loops):
 *
 *   - LED:   wave evaluation runs INSIDE the LED ISR's cycle-boundary block
 *            in led_matrix_example.c, alongside the sweep interpolator. The
 *            mod params live in led_flicker_state[].mod_X slots, set via
 *            led_matrix_set_mod_*_masked().
 *
 *   - Audio: wave evaluation runs at the start of each fill_buffer call in
 *            audio_generator.c. The mod params live in audio_gen_channel.mods[]
 *            slots, set via audio_generator_set_mod().
 *
 * No polling task, no separate slot table, no mutex contention with the
 * LED ISR or fill_buffer beyond the brief one-time write when the executor
 * dispatches an entry. Modulation now costs the same as a sweep in terms of
 * ongoing CPU and timing perturbation: effectively zero.
 */

#include "mod_engine.h"
#include "led_matrix_example.h"
#include "audio_generator.h"
#include "esp_log.h"

#define TAG "mod_engine"

static bool s_initialized = false;

esp_err_t mod_engine_init(void)
{
    if (s_initialized) return ESP_OK;
    s_initialized = true;
    ESP_LOGI(TAG, "mod_engine initialized (in-consumer evaluation, no polling task)");
    return ESP_OK;
}

/* ---------------------------------------------------------------- LED ------ */

esp_err_t mod_engine_start_led(uint8_t channel_mask, mod_led_field_t field,
                               mod_wave_t wave, float start, float end,
                               uint32_t period_ms)
{
    if (field >= MOD_LED_FIELD_COUNT || wave >= MOD_WAVE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    switch (field) {
        case MOD_LED_FREQ:
            return led_matrix_set_mod_freq_masked(channel_mask, (uint8_t)wave,
                                                  start, end, period_ms);
        case MOD_LED_DUTY: {
            uint8_t s = (start < 0) ? 0 : ((start > 100) ? 100 : (uint8_t)start);
            uint8_t e = (end   < 0) ? 0 : ((end   > 100) ? 100 : (uint8_t)end);
            return led_matrix_set_mod_duty_masked(channel_mask, (uint8_t)wave, s, e, period_ms);
        }
        case MOD_LED_BRIGHT: {
            uint8_t s = (start < 0) ? 0 : ((start > 100) ? 100 : (uint8_t)start);
            uint8_t e = (end   < 0) ? 0 : ((end   > 100) ? 100 : (uint8_t)end);
            return led_matrix_set_mod_brightness_masked(channel_mask, (uint8_t)wave, s, e, period_ms);
        }
        case MOD_LED_R: case MOD_LED_G: case MOD_LED_B: {
            uint8_t s = (start < 0) ? 0 : ((start > 255) ? 255 : (uint8_t)start);
            uint8_t e = (end   < 0) ? 0 : ((end   > 255) ? 255 : (uint8_t)end);
            char comp = (field == MOD_LED_R) ? 'R' : (field == MOD_LED_G) ? 'G' : 'B';
            return led_matrix_set_mod_color_masked(channel_mask, (uint8_t)wave,
                                                   comp, s, e, period_ms);
        }
        default:
            return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t mod_engine_stop_led(uint8_t channel_mask, mod_led_field_t field)
{
    if (field >= MOD_LED_FIELD_COUNT) return ESP_ERR_INVALID_ARG;
    return led_matrix_clear_mod_masked(channel_mask, (uint8_t)field);
}

/* -------------------------------------------------------------- audio ------ */

esp_err_t mod_engine_start_audio(uint8_t channel, mod_audio_field_t field,
                                 mod_wave_t wave, float start, float end,
                                 uint32_t period_ms)
{
    if (field >= MOD_AUDIO_FIELD_COUNT || wave >= MOD_WAVE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    audio_param_t p;
    float s = start, e = end;
    switch (field) {
        case MOD_AUDIO_FREQ:   p = AUDIO_PARAM_FREQUENCY; break;
        case MOD_AUDIO_PAN:    p = AUDIO_PARAM_PAN;       s /= 100.0f; e /= 100.0f; break;
        case MOD_AUDIO_VOLUME: p = AUDIO_PARAM_AMPLITUDE; s /= 100.0f; e /= 100.0f; break;
        case MOD_AUDIO_MOD:    p = AUDIO_PARAM_MOD_FREQ;  break;
        default: return ESP_ERR_INVALID_ARG;
    }
    /* IMPORTANT: use the _locked variant. mod_engine is called from the
     * timeline dispatch path in config_parser, which holds audio_gen_mutex
     * across the entire batch. The plain audio_generator_set_mod() would
     * try to take that same non-recursive mutex on the same task and
     * deadlock forever — no panic, no watchdog, total silent freeze. */
    return audio_generator_set_mod_locked(channel, p, (uint8_t)wave, s, e, period_ms);
}

esp_err_t mod_engine_stop_audio(uint8_t channel, mod_audio_field_t field)
{
    audio_param_t p;
    switch (field) {
        case MOD_AUDIO_FREQ:   p = AUDIO_PARAM_FREQUENCY; break;
        case MOD_AUDIO_PAN:    p = AUDIO_PARAM_PAN;       break;
        case MOD_AUDIO_VOLUME: p = AUDIO_PARAM_AMPLITUDE; break;
        case MOD_AUDIO_MOD:    p = AUDIO_PARAM_MOD_FREQ;  break;
        default: return ESP_ERR_INVALID_ARG;
    }
    /* _locked: caller (timeline executor) already holds audio_gen_mutex. */
    return audio_generator_clear_mod_locked(channel, p);
}

void mod_engine_stop_all(void)
{
    /* Iterate every (channel, field) combination and clear. Cheap enough
     * to do unconditionally — handful of function calls. */
    const uint8_t mask = 0xFF;
    for (uint8_t f = 0; f < MOD_LED_FIELD_COUNT; f++) {
        led_matrix_clear_mod_masked(mask, f);
    }
    for (uint8_t ch = 0; ch < NUM_AUDIO_CHANNELS; ch++) {
        for (uint8_t f = 0; f < MOD_AUDIO_FIELD_COUNT; f++) {
            audio_param_t p = (f == MOD_AUDIO_FREQ)   ? AUDIO_PARAM_FREQUENCY
                            : (f == MOD_AUDIO_PAN)    ? AUDIO_PARAM_PAN
                            : (f == MOD_AUDIO_VOLUME) ? AUDIO_PARAM_AMPLITUDE
                            :                            AUDIO_PARAM_MOD_FREQ;
            /* _locked: caller already holds audio_gen_mutex. See note above. */
            audio_generator_clear_mod_locked(ch, p);
        }
    }
}

int mod_wave_from_interp(config_interpolation_t interp)
{
    switch (interp) {
        case CONFIG_INTERP_TRIANGLE: return MOD_WAVE_TRIANGLE;
        case CONFIG_INTERP_SINE:     return MOD_WAVE_SINE;
        case CONFIG_INTERP_SAW_UP:   return MOD_WAVE_SAW_UP;
        case CONFIG_INTERP_SAW_DOWN: return MOD_WAVE_SAW_DOWN;
        case CONFIG_INTERP_SQUARE:   return MOD_WAVE_SQUARE;
        default:                     return -1;
    }
}
