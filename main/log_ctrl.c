/**
 * @file log_ctrl.c
 * @brief Implementation of runtime categorized logging (see log_ctrl.h).
 */

#include "log_ctrl.h"

#include "esp_log.h"
#include "nvs.h"
#include "cJSON.h"

#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <stdlib.h>

static const char *TAG = "log_ctrl";

/* Persist per-category levels in the existing device-config namespace under a
 * dedicated key. This is independent of the settings blob (settings.c), so
 * there is no SETTINGS_VERSION churn — a schema change here can't corrupt the
 * settings struct and vice versa. */
#define LOGCTRL_NS   "devcfg"
#define LOGCTRL_KEY  "loglvls"

/* ---------------------------------------------------------------------------
 * Category registry — a category maps a logical name to a set of ESP_LOG TAGs.
 * The tag strings MUST match the `static const char *TAG = "..."` in each .c.
 * A "*" tag means the global default (esp_log_level_set("*", ...)).
 * --------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *tags[6];   /* NULL-terminated (<=5 tags + terminator) */
} log_category_t;

static const log_category_t s_categories[] = {
    { "audio",    { "audio_generator", "audio_manager", "audio_driver", "audio_test", "es8388", NULL } },
    { "led",      { "led_matrix", "led_strip", NULL } },
    { "wifi",     { "wifi_manager", NULL } },
    { "bg",       { "bg_player", "wav_parser", NULL } },
    { "web",      { "web_server", NULL } },
    { "config",   { "config_parser", NULL } },
    { "timing",   { "timing_engine", "mod_engine", "memory_pool", "lock_free_comm", NULL } },
    { "codec",    { "ac101", "audio_driver", NULL } },
    { "diag",     { "diag", NULL } },
    { "settings", { "settings", NULL } },
    { "system",   { "main", NULL } },
    /* "global" is a pseudo-category that sets the wildcard default for any tag
     * not otherwise overridden (esp_log_level_set("*", ...)). */
    { "global",   { "*", NULL } },
};
#define NUM_CATEGORIES (sizeof(s_categories) / sizeof(s_categories[0]))

/* Current level per category (index-aligned with s_categories). Seeded to the
 * compile-time default so a fresh GET reflects reality before any override. */
static esp_log_level_t s_levels[NUM_CATEGORIES];

/* ---------------------------------------------------------------------------
 * Level <-> string mapping
 * --------------------------------------------------------------------------- */

const char *log_ctrl_level_str(esp_log_level_t level)
{
    switch (level) {
        case ESP_LOG_NONE:    return "none";
        case ESP_LOG_ERROR:   return "error";
        case ESP_LOG_WARN:    return "warn";
        case ESP_LOG_INFO:    return "info";
        case ESP_LOG_DEBUG:   return "debug";
        case ESP_LOG_VERBOSE: return "verbose";
        default:              return "info";
    }
}

bool log_ctrl_level_parse(const char *s, esp_log_level_t *out)
{
    if (!s || !out) return false;
    if (!strcasecmp(s, "none"))         { *out = ESP_LOG_NONE;    return true; }
    if (!strcasecmp(s, "error"))        { *out = ESP_LOG_ERROR;   return true; }
    if (!strcasecmp(s, "warn") ||
        !strcasecmp(s, "warning"))      { *out = ESP_LOG_WARN;    return true; }
    if (!strcasecmp(s, "info"))         { *out = ESP_LOG_INFO;    return true; }
    if (!strcasecmp(s, "debug"))        { *out = ESP_LOG_DEBUG;   return true; }
    if (!strcasecmp(s, "verbose"))      { *out = ESP_LOG_VERBOSE; return true; }
    /* Accept numeric 0..5 as a fallback. */
    if (s[0] >= '0' && s[0] <= '5' && s[1] == '\0') {
        *out = (esp_log_level_t)(s[0] - '0');
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Apply
 * --------------------------------------------------------------------------- */

static int find_category(const char *name)
{
    if (!name) return -1;
    /* "*"/"all" alias the global wildcard category. */
    if (!strcmp(name, "*") || !strcasecmp(name, "all")) {
        name = "global";
    }
    for (size_t i = 0; i < NUM_CATEGORIES; i++) {
        if (!strcasecmp(s_categories[i].name, name)) return (int)i;
    }
    return -1;
}

/* Apply a category's level to every one of its tags (no persist). */
static void apply_category(int idx, esp_log_level_t level)
{
    s_levels[idx] = level;
    for (const char *const *t = s_categories[idx].tags; *t; t++) {
        esp_log_level_set(*t, level);
    }
}

/* ---------------------------------------------------------------------------
 * NVS persistence — a compact blob of NUM_CATEGORIES bytes, one level each.
 * --------------------------------------------------------------------------- */

static void persist(void)
{
    nvs_handle_t h;
    if (nvs_open(LOGCTRL_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t blob[NUM_CATEGORIES];
    for (size_t i = 0; i < NUM_CATEGORIES; i++) blob[i] = (uint8_t)s_levels[i];
    if (nvs_set_blob(h, LOGCTRL_KEY, blob, sizeof(blob)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

void log_ctrl_init(void)
{
    /* Seed to the compile-time default level so the reported state is accurate
     * before any persisted override is loaded. */
    esp_log_level_t dflt = (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL;
    for (size_t i = 0; i < NUM_CATEGORIES; i++) s_levels[i] = dflt;

    nvs_handle_t h;
    if (nvs_open(LOGCTRL_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t blob[NUM_CATEGORIES];
        size_t len = sizeof(blob);
        if (nvs_get_blob(h, LOGCTRL_KEY, blob, &len) == ESP_OK && len == sizeof(blob)) {
            for (size_t i = 0; i < NUM_CATEGORIES; i++) {
                esp_log_level_t lv = (esp_log_level_t)blob[i];
                if (lv > ESP_LOG_VERBOSE) lv = dflt;   /* guard against garbage */
                apply_category((int)i, lv);
            }
            ESP_LOGI(TAG, "restored %u persisted category log levels", (unsigned)NUM_CATEGORIES);
        }
        nvs_close(h);
    }
}

bool log_ctrl_set(const char *category, esp_log_level_t level)
{
    int idx = find_category(category);
    if (idx < 0) return false;
    apply_category(idx, level);
    persist();
    ESP_LOGI(TAG, "category '%s' -> %s", s_categories[idx].name, log_ctrl_level_str(level));
    return true;
}

int log_ctrl_apply_json(const char *body, int len)
{
    if (!body || len <= 0) return -1;
    cJSON *root = cJSON_ParseWithLength(body, (size_t)len);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        ESP_LOGW(TAG, "apply_json: parse error / not an object");
        return -1;
    }

    int applied = 0;
    for (cJSON *it = root->child; it; it = it->next) {
        if (!cJSON_IsString(it) || !it->valuestring || !it->string) continue;
        int idx = find_category(it->string);
        if (idx < 0) {
            ESP_LOGW(TAG, "unknown category '%s' — ignoring", it->string);
            continue;
        }
        esp_log_level_t lv;
        if (!log_ctrl_level_parse(it->valuestring, &lv)) {
            ESP_LOGW(TAG, "unknown level '%s' for '%s' — ignoring", it->valuestring, it->string);
            continue;
        }
        apply_category(idx, lv);
        ESP_LOGI(TAG, "category '%s' -> %s", s_categories[idx].name, log_ctrl_level_str(lv));
        applied++;
    }
    cJSON_Delete(root);

    if (applied > 0) persist();
    return applied;
}

int log_ctrl_to_json(char *buf, int cap)
{
    if (!buf || cap <= 0) return -1;
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;

    cJSON *cats = cJSON_AddArrayToObject(root, "categories");
    if (cats) {
        for (size_t i = 0; i < NUM_CATEGORIES; i++) {
            cJSON *c = cJSON_CreateObject();
            if (!c) continue;
            cJSON_AddStringToObject(c, "name", s_categories[i].name);
            cJSON_AddStringToObject(c, "level", log_ctrl_level_str(s_levels[i]));
            cJSON *tags = cJSON_AddArrayToObject(c, "tags");
            if (tags) {
                for (const char *const *t = s_categories[i].tags; *t; t++) {
                    cJSON_AddItemToArray(tags, cJSON_CreateString(*t));
                }
            }
            cJSON_AddItemToArray(cats, c);
        }
    }

    cJSON *levels = cJSON_AddArrayToObject(root, "levels");
    if (levels) {
        static const char *names[] = { "none", "error", "warn", "info", "debug", "verbose" };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            cJSON_AddItemToArray(levels, cJSON_CreateString(names[i]));
        }
    }

    bool ok = cJSON_PrintPreallocated(root, buf, cap, false);
    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGW(TAG, "to_json: buffer too small (cap=%d)", cap);
        return -1;
    }
    return (int)strlen(buf);
}
