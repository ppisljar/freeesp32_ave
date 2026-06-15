#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include "esp_err.h"
#include "config_parser.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Static memory pools for long-lived allocations whose size is bounded at
 * compile time.  Goal is to eliminate heap fragmentation accumulating over
 * multi-hour sessions where users upload / start / stop .led configs many
 * times.
 *
 * Real-time ISR paths already contain zero dynamic allocations (verified by
 * Phase 4.1 recon — see reports/003_phase4_1_memory_pools.md).  The pools
 * here therefore target the long-lived per-play allocations (the persistent
 * timeline copy held for the lifetime of a playing config).
 *
 * Pools are claimed/released, not freelist-allocated: a single static array
 * sized at CONFIG_PARSER_MAX_ENTRIES serves the at-most-one persistent
 * timeline that exists at a time.  Claim/release is atomic so the
 * accounting matches the lock-free pattern used elsewhere in this project.
 */

typedef struct {
    uint32_t claim_count;       // total successful claims (lifetime)
    uint32_t release_count;     // total releases (lifetime)
    uint32_t failed_claims;     // claims attempted while pool was already in use
    size_t   peak_entries_used; // largest count parked in the pool so far
    bool     in_use;            // current claim state (snapshot, not guaranteed
                                // stable across multi-step reads)
} memory_pool_stats_t;

esp_err_t memory_pool_init(void);

/* Claim the timeline entry pool.  On success, *out_entries points to a
 * buffer of CONFIG_PARSER_MAX_ENTRIES config_entry_t slots and the slot
 * remains valid until memory_pool_timeline_release() is called.  Returns
 * ESP_ERR_NO_MEM if the pool is already claimed.
 */
esp_err_t memory_pool_timeline_claim(config_entry_t **out_entries,
                                     size_t *out_capacity);

/* Mark the buffer previously returned by memory_pool_timeline_claim() as
 * available.  Pass `entries_used` so peak tracking stays accurate; pass 0
 * if not known.  Safe to call when not claimed (no-op + warning).
 */
void memory_pool_timeline_release(size_t entries_used);

/* Snapshot pool statistics.  Cheap; safe from task context (not ISR).
 */
void memory_pool_get_stats(memory_pool_stats_t *out_stats);

#endif /* MEMORY_POOL_H */
