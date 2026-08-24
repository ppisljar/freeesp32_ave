#ifndef MOD_ENGINE_H
#define MOD_ENGINE_H

/*
 * mod_engine — periodic modulation of LED (and audio) parameters.
 *
 * Implements the runtime side of the .ledc modulation prefixes:
 *   ^start:end:period  — triangle wave (linear up then down)
 *   ~start:end:period  — sine wave (smooth)
 *   /start:end:period  — sawtooth (ramp from start to end, jump back)
 *   \start:end:period  — reverse sawtooth (typically end < start)
 *   _start:end:period  — square wave (half period at start, half at end)
 *
 * The engine maintains a small slot table of active modulations. A FreeRTOS
 * task wakes at ~100 Hz, walks the active slots, computes the current value
 * for each using the wave-shape evaluator, and pushes it via the appropriate
 * LED matrix / audio_generator update API.
 *
 * Modulation is preempted whenever a new timeline entry for the same
 * (channel/mask, field) arrives — the executor calls mod_engine_stop_*
 * before applying the new entry, then mod_engine_start_* again if the new
 * entry is itself a modulation.
 */

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "config_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOD_WAVE_TRIANGLE = 0,  // ^
    MOD_WAVE_SINE,          // ~
    MOD_WAVE_SAW_UP,        // /
    MOD_WAVE_SAW_DOWN,      // \ ;  end is typically less than start
    MOD_WAVE_SQUARE,        // _
    MOD_WAVE_COUNT
} mod_wave_t;

typedef enum {
    MOD_LED_FREQ = 0,
    MOD_LED_DUTY,
    MOD_LED_BRIGHT,
    MOD_LED_R,
    MOD_LED_G,
    MOD_LED_B,
    MOD_LED_PHASE,
    MOD_LED_ATTACK,
    MOD_LED_FIELD_COUNT
} mod_led_field_t;

typedef enum {
    MOD_AUDIO_FREQ = 0,
    MOD_AUDIO_PAN,
    MOD_AUDIO_VOLUME,
    MOD_AUDIO_MOD,
    /* Isochronic pulse shape — compound cells in .ledc, so they modulate too. */
    MOD_AUDIO_ISO_DUTY,
    MOD_AUDIO_ISO_PHASE,
    MOD_AUDIO_ISO_ATTACK,
    MOD_AUDIO_FIELD_COUNT
} mod_audio_field_t;

/* Bring up the engine: allocate the slot table, spawn the polling task.
 * Idempotent — second call is a no-op. */
esp_err_t mod_engine_init(void);

/* Bind / replace a modulation on LED channel(s).
 *
 * @param channel_mask  Bitmask of LED channels to modulate together
 *                      (bit N = channel N+1; typically 0xFF for all-channels).
 * @param field         Which LED parameter to modulate (brightness, duty, etc.)
 * @param wave          Wave shape (triangle/sine/saw/square)
 * @param start         Wave's "start" value (where t=0 sits)
 * @param end           Wave's "end" value (the other extreme)
 * @param period_ms     Full cycle time (start -> end -> start = one period)
 *
 * If a modulation is already active for the same (mask, field), it is
 * replaced cleanly. The phase resets to t=0 (start value) on replacement. */
esp_err_t mod_engine_start_led(uint8_t channel_mask, mod_led_field_t field,
                               mod_wave_t wave, float start, float end,
                               uint32_t period_ms);

/* Stop any active modulation on (mask, field). No-op if none active.
 * The LED parameter is LEFT at whatever value it had at the moment of stop —
 * the caller is expected to push a new value (e.g. via a timeline entry's
 * step semantic) immediately after. */
esp_err_t mod_engine_stop_led(uint8_t channel_mask, mod_led_field_t field);

/* Audio modulation: same idea as LED. Currently NOT fully implemented;
 * the call logs a warning and returns ESP_OK. Audio support requires a
 * per-parameter update API on audio_generator that doesn't exist yet. */
esp_err_t mod_engine_start_audio(uint8_t channel, mod_audio_field_t field,
                                 mod_wave_t wave, float start, float end,
                                 uint32_t period_ms);
esp_err_t mod_engine_stop_audio(uint8_t channel, mod_audio_field_t field);

/* Stop ALL active modulations on every channel/field. Used at timeline
 * stop / abort to clean up. */
void mod_engine_stop_all(void);

/* Convert a config_interpolation_t (parser-layer enum) to mod_wave_t
 * (engine-layer enum). Returns -1 if the input is not a modulation type. */
int mod_wave_from_interp(config_interpolation_t interp);

#ifdef __cplusplus
}
#endif

#endif // MOD_ENGINE_H
