#include "isr_profiling.h"

#ifdef CONFIG_ISR_PROFILING
isr_profile_t g_isr_profiles[ISR_PROFILE_SLOT_COUNT] = {
    [0] = { .min_cycles = UINT32_MAX, .max_cycles = 0, .sum_cycles = 0, .count = 0 },
    [1] = { .min_cycles = UINT32_MAX, .max_cycles = 0, .sum_cycles = 0, .count = 0 },
};
#endif
