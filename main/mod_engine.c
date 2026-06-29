/*
 * mod_engine — periodic modulation runtime for .ledc timeline files.
 *
 * Maintains a small slot table of active modulations; a FreeRTOS task
 * polls at ~100 Hz, evaluates each wave's current value, and pushes
 * updates via the LED matrix / audio_generator APIs.
 *
 * Scope notes:
 * - LED brightness modulation is fully supported.
 * - LED freq/duty/R/G/B modulation is parsed but not yet driven, because
 *   the LED matrix API lacks per-field setters that preserve "other" fields
 *   (update_flicker_params_masked overwrites all three of freq+duty+bright;
 *   set_flicker_color_masked overwrites all of RGB). When per-field setters
 *   land, the engine just gains a few extra cases in dispatch_led_slot().
 * - Audio modulation is a stub — audio_generator lacks a per-parameter
 *   update API, so audio_mod entries log a warning and return ESP_OK.
 */

#include "mod_engine.h"
#include "led_matrix_example.h"
#include "audio_generator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

#define TAG "mod_engine"

#define MAX_SLOTS          32
#define POLL_PERIOD_MS     10   /* 100 Hz update rate */
#define POLL_TASK_STACK    4096
#define POLL_TASK_PRIO     5    /* below audio (23) / LED ISR (24), above default */

typedef enum {
    SLOT_KIND_FREE = 0,
    SLOT_KIND_LED,
    SLOT_KIND_AUDIO,
} slot_kind_t;

typedef struct {
    slot_kind_t kind;
    uint8_t     mask_or_channel;
    uint8_t     field;          /* mod_led_field_t or mod_audio_field_t */
    mod_wave_t  wave;
    float       start;
    float       end;
    uint32_t    period_ms;
    uint64_t    start_time_us;  /* absolute time the modulation began */
} slot_t;

static slot_t s_slots[MAX_SLOTS];
static bool s_initialized = false;
static TaskHandle_t s_poll_task = NULL;
static portMUX_TYPE s_slots_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---------------------------------------------------------- wave evaluator -- */

/* Compute the current modulation value for a slot at absolute time `now_us`.
 * Returns the modulated value (in the field's natural units). */
static float evaluate(const slot_t *s, uint64_t now_us)
{
    if (s->period_ms == 0) return s->start;          /* divide-by-zero guard */
    uint64_t elapsed_ms = (now_us - s->start_time_us) / 1000ULL;
    uint32_t t_in_cycle = (uint32_t)(elapsed_ms % s->period_ms);
    float phase = (float)t_in_cycle / (float)s->period_ms;  /* 0..1 */
    float shape;
    switch (s->wave) {
        case MOD_WAVE_TRIANGLE:
            shape = (phase < 0.5f) ? (phase * 2.0f) : (2.0f - phase * 2.0f);
            break;
        case MOD_WAVE_SINE:
            /* cosine-shifted sine: starts at 0, peaks at 1 mid-period, back to 0 */
            shape = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * phase);
            break;
        case MOD_WAVE_SAW_UP:
            shape = phase;                                /* 0 → 1, then jumps */
            break;
        case MOD_WAVE_SAW_DOWN:
            shape = 1.0f - phase;                         /* 1 → 0, then jumps */
            break;
        case MOD_WAVE_SQUARE:
            shape = (phase < 0.5f) ? 0.0f : 1.0f;
            break;
        default:
            shape = 0.0f;
            break;
    }
    return s->start + (s->end - s->start) * shape;
}

/* -------------------------------------------------------- dispatch helpers -- */

static inline uint8_t clamp_u8_pct(float val)   /* 0..100 */
{
    int v = (int)(val + 0.5f);
    return (uint8_t)(v < 0 ? 0 : (v > 100 ? 100 : v));
}
static inline uint8_t clamp_u8_full(float val)  /* 0..255 */
{
    int v = (int)(val + 0.5f);
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void dispatch_led_slot(const slot_t *s, uint64_t now_us)
{
    float val = evaluate(s, now_us);
    switch ((mod_led_field_t)s->field) {
        case MOD_LED_BRIGHT:
            led_matrix_update_brightness_masked(s->mask_or_channel, clamp_u8_pct(val));
            break;
        case MOD_LED_FREQ:
            /* val is in Hz (timeline native units) */
            if (val > 0.01f) led_matrix_update_frequency_masked(s->mask_or_channel, val);
            break;
        case MOD_LED_DUTY:
            led_matrix_update_duty_masked(s->mask_or_channel, clamp_u8_pct(val));
            break;
        case MOD_LED_R:
            led_matrix_update_color_r_masked(s->mask_or_channel, clamp_u8_full(val));
            break;
        case MOD_LED_G:
            led_matrix_update_color_g_masked(s->mask_or_channel, clamp_u8_full(val));
            break;
        case MOD_LED_B:
            led_matrix_update_color_b_masked(s->mask_or_channel, clamp_u8_full(val));
            break;
        default:
            break;
    }
}

static void dispatch_audio_slot(const slot_t *s, uint64_t now_us)
{
    float val = evaluate(s, now_us);
    audio_param_t p;
    /* Volume in the timeline is 0-100; audio_generator amplitude is 0.0-1.0. */
    switch ((mod_audio_field_t)s->field) {
        case MOD_AUDIO_FREQ:   p = AUDIO_PARAM_FREQUENCY;                          break;
        case MOD_AUDIO_PAN:    p = AUDIO_PARAM_PAN;       val = val / 100.0f;      break;
        case MOD_AUDIO_VOLUME: p = AUDIO_PARAM_AMPLITUDE; val = val / 100.0f;      break;
        case MOD_AUDIO_MOD:    p = AUDIO_PARAM_MOD_FREQ;                           break;
        default: return;
    }
    audio_generator_set_param(s->mask_or_channel, p, val);
}

/* ---------------------------------------------------------------- task ----- */

static void poll_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(POLL_PERIOD_MS);
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last_wake, period);
        uint64_t now_us = esp_timer_get_time();
        /* Snapshot active slots under the lock, then dispatch outside it
         * to avoid holding the spinlock during LED matrix calls. */
        slot_t local[MAX_SLOTS];
        portENTER_CRITICAL(&s_slots_lock);
        memcpy(local, s_slots, sizeof(local));
        portEXIT_CRITICAL(&s_slots_lock);

        for (int i = 0; i < MAX_SLOTS; i++) {
            if (local[i].kind == SLOT_KIND_LED) {
                dispatch_led_slot(&local[i], now_us);
            } else if (local[i].kind == SLOT_KIND_AUDIO) {
                dispatch_audio_slot(&local[i], now_us);
            }
        }
    }
}

/* ---------------------------------------------------------- slot table ----- */

/* Find an existing slot matching the key, or an empty slot if none. Returns
 * -1 if both no match and no empty slot. */
static int find_or_alloc_slot(slot_kind_t kind, uint8_t mask_or_channel, uint8_t field)
{
    int first_free = -1;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (s_slots[i].kind == kind
            && s_slots[i].mask_or_channel == mask_or_channel
            && s_slots[i].field == field) {
            return i;
        }
        if (s_slots[i].kind == SLOT_KIND_FREE && first_free < 0) {
            first_free = i;
        }
    }
    return first_free;
}

/* ----------------------------------------------------------- public API ---- */

esp_err_t mod_engine_init(void)
{
    if (s_initialized) return ESP_OK;
    memset(s_slots, 0, sizeof(s_slots));
    BaseType_t ok = xTaskCreatePinnedToCore(poll_task, "mod_engine",
                                            POLL_TASK_STACK, NULL,
                                            POLL_TASK_PRIO, &s_poll_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create poll task");
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "mod_engine initialized (slots=%d, poll=%dms)",
             MAX_SLOTS, POLL_PERIOD_MS);
    return ESP_OK;
}

esp_err_t mod_engine_start_led(uint8_t channel_mask, mod_led_field_t field,
                               mod_wave_t wave, float start, float end,
                               uint32_t period_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (field >= MOD_LED_FIELD_COUNT || wave >= MOD_WAVE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_slots_lock);
    int idx = find_or_alloc_slot(SLOT_KIND_LED, channel_mask, (uint8_t)field);
    if (idx < 0) {
        portEXIT_CRITICAL(&s_slots_lock);
        ESP_LOGW(TAG, "no free slot for LED mod (mask=0x%02X field=%d)",
                 channel_mask, (int)field);
        return ESP_ERR_NO_MEM;
    }
    s_slots[idx] = (slot_t){
        .kind = SLOT_KIND_LED,
        .mask_or_channel = channel_mask,
        .field = (uint8_t)field,
        .wave = wave,
        .start = start,
        .end = end,
        .period_ms = period_ms,
        .start_time_us = (uint64_t)esp_timer_get_time(),
    };
    portEXIT_CRITICAL(&s_slots_lock);
    /* Push the start value immediately so the LED doesn't wait up to 10 ms
     * before the first poll cycle. */
    dispatch_led_slot(&s_slots[idx], s_slots[idx].start_time_us);
    return ESP_OK;
}

esp_err_t mod_engine_stop_led(uint8_t channel_mask, mod_led_field_t field)
{
    if (!s_initialized) return ESP_OK;
    portENTER_CRITICAL(&s_slots_lock);
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (s_slots[i].kind == SLOT_KIND_LED
            && s_slots[i].mask_or_channel == channel_mask
            && s_slots[i].field == (uint8_t)field) {
            s_slots[i].kind = SLOT_KIND_FREE;
        }
    }
    portEXIT_CRITICAL(&s_slots_lock);
    return ESP_OK;
}

esp_err_t mod_engine_start_audio(uint8_t channel, mod_audio_field_t field,
                                 mod_wave_t wave, float start, float end,
                                 uint32_t period_ms)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (field >= MOD_AUDIO_FIELD_COUNT || wave >= MOD_WAVE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_slots_lock);
    int idx = find_or_alloc_slot(SLOT_KIND_AUDIO, channel, (uint8_t)field);
    if (idx < 0) {
        portEXIT_CRITICAL(&s_slots_lock);
        ESP_LOGW(TAG, "no free slot for audio mod (ch=%u field=%d)",
                 channel, (int)field);
        return ESP_ERR_NO_MEM;
    }
    s_slots[idx] = (slot_t){
        .kind = SLOT_KIND_AUDIO,
        .mask_or_channel = channel,
        .field = (uint8_t)field,
        .wave = wave,
        .start = start,
        .end = end,
        .period_ms = period_ms,
        .start_time_us = (uint64_t)esp_timer_get_time(),
    };
    portEXIT_CRITICAL(&s_slots_lock);
    /* Push start value immediately so audio doesn't wait up to 10 ms. */
    dispatch_audio_slot(&s_slots[idx], s_slots[idx].start_time_us);
    return ESP_OK;
}

esp_err_t mod_engine_stop_audio(uint8_t channel, mod_audio_field_t field)
{
    if (!s_initialized) return ESP_OK;
    portENTER_CRITICAL(&s_slots_lock);
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (s_slots[i].kind == SLOT_KIND_AUDIO
            && s_slots[i].mask_or_channel == channel
            && s_slots[i].field == (uint8_t)field) {
            s_slots[i].kind = SLOT_KIND_FREE;
        }
    }
    portEXIT_CRITICAL(&s_slots_lock);
    return ESP_OK;
}

void mod_engine_stop_all(void)
{
    if (!s_initialized) return;
    portENTER_CRITICAL(&s_slots_lock);
    memset(s_slots, 0, sizeof(s_slots));
    portEXIT_CRITICAL(&s_slots_lock);
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
