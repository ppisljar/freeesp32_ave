#include "memory_pool.h"
#include "esp_log.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "memory_pool";

static config_entry_t g_timeline_entry_pool[CONFIG_PARSER_MAX_ENTRIES];

static atomic_bool   g_timeline_in_use      = ATOMIC_VAR_INIT(false);
static atomic_uint   g_claim_count          = ATOMIC_VAR_INIT(0);
static atomic_uint   g_release_count        = ATOMIC_VAR_INIT(0);
static atomic_uint   g_failed_claims        = ATOMIC_VAR_INIT(0);
static atomic_size_t g_peak_entries_used    = ATOMIC_VAR_INIT(0);
static bool          g_pool_initialized     = false;

esp_err_t memory_pool_init(void)
{
    if (g_pool_initialized) {
        return ESP_OK;
    }

    memset(g_timeline_entry_pool, 0, sizeof(g_timeline_entry_pool));

    config_entry_t *probe = NULL;
    size_t cap = 0;
    esp_err_t ret = memory_pool_timeline_claim(&probe, &cap);
    if (ret != ESP_OK || probe != g_timeline_entry_pool ||
        cap != CONFIG_PARSER_MAX_ENTRIES) {
        ESP_LOGE(TAG, "Pool self-test failed: ret=%d probe=%p cap=%zu",
                 ret, probe, cap);
        return ESP_FAIL;
    }
    memory_pool_timeline_release(0);
    if (atomic_load(&g_timeline_in_use)) {
        ESP_LOGE(TAG, "Pool self-test: release did not clear in_use");
        return ESP_FAIL;
    }
    atomic_store(&g_claim_count, 0);
    atomic_store(&g_release_count, 0);
    atomic_store(&g_failed_claims, 0);
    atomic_store(&g_peak_entries_used, 0);

    g_pool_initialized = true;
    ESP_LOGI(TAG, "Memory pool initialized: timeline_entry_pool = %zu entries, %zu bytes",
             (size_t)CONFIG_PARSER_MAX_ENTRIES, sizeof(g_timeline_entry_pool));
    return ESP_OK;
}

esp_err_t memory_pool_timeline_claim(config_entry_t **out_entries,
                                     size_t *out_capacity)
{
    if (!out_entries || !out_capacity) {
        return ESP_ERR_INVALID_ARG;
    }

    bool expected = false;
    if (!atomic_compare_exchange_strong(&g_timeline_in_use, &expected, true)) {
        atomic_fetch_add(&g_failed_claims, 1);
        ESP_LOGW(TAG, "Timeline pool already claimed");
        return ESP_ERR_NO_MEM;
    }

    atomic_fetch_add(&g_claim_count, 1);
    *out_entries  = g_timeline_entry_pool;
    *out_capacity = CONFIG_PARSER_MAX_ENTRIES;
    return ESP_OK;
}

void memory_pool_timeline_release(size_t entries_used)
{
    bool was_in_use = atomic_exchange(&g_timeline_in_use, false);
    if (!was_in_use) {
        ESP_LOGW(TAG, "Timeline pool release called while not claimed");
        return;
    }

    size_t prev_peak = atomic_load(&g_peak_entries_used);
    while (entries_used > prev_peak &&
           !atomic_compare_exchange_weak(&g_peak_entries_used, &prev_peak,
                                         entries_used)) {
        /* retry */
    }
    atomic_fetch_add(&g_release_count, 1);
}

void memory_pool_get_stats(memory_pool_stats_t *out_stats)
{
    if (!out_stats) return;
    out_stats->claim_count       = atomic_load(&g_claim_count);
    out_stats->release_count     = atomic_load(&g_release_count);
    out_stats->failed_claims     = atomic_load(&g_failed_claims);
    out_stats->peak_entries_used = atomic_load(&g_peak_entries_used);
    out_stats->in_use            = atomic_load(&g_timeline_in_use);
}
