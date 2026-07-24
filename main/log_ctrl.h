/**
 * @file log_ctrl.h
 * @brief Runtime-controllable, categorized logging.
 *
 * The firmware groups the many ESP_LOG TAG strings ("audio_generator",
 * "bg_player", "web_server", ...) into a small set of logical CATEGORIES
 * ("audio", "bg", "web", ...). A category's level is applied by calling
 * esp_log_level_set(tag, level) for each of its member tags, so raising e.g.
 * "bg" to DEBUG turns on every ESP_LOGD in bg_player.c / wav_parser.c /
 * mp3_decoder.c at once.
 *
 * DEBUG/VERBOSE lines are only *compiled in* when CONFIG_LOG_MAXIMUM_LEVEL
 * permits them (raised to DEBUG in sdkconfig); CONFIG_LOG_DEFAULT_LEVEL stays
 * at INFO so nothing debug-level runs until a category is explicitly raised.
 *
 * Per-category levels are persisted in NVS (namespace "devcfg", a dedicated
 * key independent of the settings blob, so there is no SETTINGS_VERSION churn)
 * and re-applied on boot.
 */
#ifndef LOG_CTRL_H
#define LOG_CTRL_H

#include "esp_log.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize the log-control subsystem: load persisted per-category levels from
 * NVS (if any) and apply them via esp_log_level_set(). Safe to call once, after
 * NVS is initialized. Categories with no persisted override are left at the
 * compile-time default (INFO).
 */
void log_ctrl_init(void);

/**
 * Set a single category's level and apply it to all of the category's tags.
 * @param category  category name, or "*"/"all" for the global default.
 * @param level     one of esp_log_level_t.
 * @return true if the category name was recognized, false otherwise.
 * Persists the change to NVS.
 */
bool log_ctrl_set(const char *category, esp_log_level_t level);

/**
 * Apply a JSON object of {"<category>":"<level>", ...} updates. Unknown
 * categories / levels are skipped with a warning. Persists once at the end.
 * @return number of categories successfully applied (>=0), or -1 on parse error.
 */
int log_ctrl_apply_json(const char *body, int len);

/**
 * Serialize the current state as JSON into @p buf:
 *   {"categories":[{"name","level","tags":[...]}...],
 *    "levels":["none","error","warn","info","debug","verbose"]}
 * @return number of bytes written (excluding NUL), or -1 on error / overflow.
 */
int log_ctrl_to_json(char *buf, int cap);

/** Map an esp_log_level_t to its lowercase string ("none".."verbose"). */
const char *log_ctrl_level_str(esp_log_level_t level);

/** Map a level string ("none".."verbose", case-insensitive) to esp_log_level_t.
 *  Returns true on success. Also accepts numeric "0".."5". */
bool log_ctrl_level_parse(const char *s, esp_log_level_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LOG_CTRL_H */
