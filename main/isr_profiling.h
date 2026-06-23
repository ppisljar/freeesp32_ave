#pragma once

#include "sdkconfig.h"
#include <stdint.h>
#include <limits.h>

#ifdef CONFIG_ISR_PROFILING

#include "esp_cpu.h"
#include "esp_log.h"

typedef struct {
    volatile uint32_t min_cycles;
    volatile uint32_t max_cycles;
    volatile uint64_t sum_cycles;
    volatile uint32_t count;
} isr_profile_t;

// One slot per ISR: 0=timing_engine_alarm, 1=led_flicker
// (slot for the former audio_led_sync I2S callback was removed with the
// VU pipeline; downstream slots shifted down by one.)
#define ISR_PROFILE_SLOT_COUNT 2
extern isr_profile_t g_isr_profiles[ISR_PROFILE_SLOT_COUNT];

#define ISR_PROFILE_BEGIN(slot) \
    uint32_t _isr_cyc_start = esp_cpu_get_cycle_count()

#define ISR_PROFILE_END(slot) \
    do { \
        uint32_t _isr_cyc_end = esp_cpu_get_cycle_count(); \
        uint32_t _isr_cyc_elapsed = _isr_cyc_end - _isr_cyc_start; \
        isr_profile_t *_p = &g_isr_profiles[(slot)]; \
        if (_isr_cyc_elapsed < _p->min_cycles) _p->min_cycles = _isr_cyc_elapsed; \
        if (_isr_cyc_elapsed > _p->max_cycles) _p->max_cycles = _isr_cyc_elapsed; \
        _p->sum_cycles += _isr_cyc_elapsed; \
        _p->count++; \
    } while (0)

// Call from task context only (uses ESP_LOGI).
static inline void isr_profiling_report(void) {
    static const char *TAG = "isr_profile";
    static const char *names[ISR_PROFILE_SLOT_COUNT] = {
        "timing_engine_alarm_callback",
        "led_flicker_timer_callback",
    };
    for (int i = 0; i < ISR_PROFILE_SLOT_COUNT; i++) {
        isr_profile_t *p = &g_isr_profiles[i];
        uint32_t cnt = p->count;
        if (cnt == 0) {
            ESP_LOGI(TAG, "[%s] no samples yet", names[i]);
            continue;
        }
        uint32_t avg = (uint32_t)(p->sum_cycles / cnt);
        ESP_LOGI(TAG, "[%s] count=%lu min=%lu max=%lu avg=%lu cycles",
                 names[i], (unsigned long)cnt,
                 (unsigned long)p->min_cycles,
                 (unsigned long)p->max_cycles,
                 (unsigned long)avg);
    }
}

#else  /* CONFIG_ISR_PROFILING not set */

#define ISR_PROFILE_BEGIN(slot)  do {} while (0)
#define ISR_PROFILE_END(slot)    do {} while (0)

static inline void isr_profiling_report(void) {}

#endif /* CONFIG_ISR_PROFILING */
