/**
 * @file marker.h
 * @brief Stimulus markers — timestamped experiment events, fanned out to a
 *        configurable list of sinks.
 *
 * WHY THIS EXISTS
 * ---------------
 * While the device runs a session, something else (an EEG) is recording. That
 * recorder has no idea when the stimulus started, stopped or changed rate. A
 * marker is the announcement: "at this instant, THIS happened".
 *
 * THREE TIMESTAMPS, NEVER CONFLATED
 * ---------------------------------
 * The whole value of this module is the distinction between:
 *
 *   t_sched_us       when the .ledc INTENDED the event (time_ms vs transport origin)
 *   t_disp_us        when the firmware PROCESSED it (esp_timer, stamped once)
 *   t_onset_pred_us  when the signal is EXPECTED at the output
 *
 * The third is PREDICTED, NOT MEASURED, and is named that way everywhere it
 * surfaces. It is t_disp_us plus the modality's pipeline latency, which the A/V
 * sync work measured:
 *
 *   audio : AUDIO_DMA_PIPELINE_LAG_US (46.439 ms) — 8 x 256-frame DMA ring
 *   LED   : the same 46.439 ms anchor PLUS the strip write, which is
 *           pixel_index x 30.0 us (1.25 us/bit x 24 bits per pixel), i.e.
 *           0 .. 2.220 ms across a 74-pixel strip. A flat constant is WRONG
 *           here, which is why marker_pipeline_lag_us() takes a modality.
 *
 * A marker timestamp is not a verified physical onset. Anyone doing timing
 * science with this needs the GPIO sink validated against a capture (see
 * tools/avmeasure) rather than trusting the prediction.
 *
 * REAL-TIME SAFETY
 * ----------------
 * marker_emit() is called from the timeline dispatcher. It does NO I/O: it
 * stamps the record and pushes it into a preallocated ring, which a
 * low-priority task drains into the SD and UDP sinks. Overflow increments a
 * counter rather than blocking or silently vanishing — a dropped marker that
 * nobody knows about would poison an experiment.
 *
 * The GPIO sink is deliberately the exception: it arms an esp_timer, because
 * its entire reason to exist is timing precision.
 */

#ifndef MARKER_H
#define MARKER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MARKER_LABEL_MAX   32   /**< incl. NUL; [A-Z0-9_] */
#define MARKER_KV_MAX       4   /**< key=value pairs per marker */
#define MARKER_KEY_MAX     16   /**< incl. NUL */
#define MARKER_VAL_MAX     32   /**< incl. NUL */
#define MARKER_RING_SLOTS  64   /**< preallocated; overflow is counted, not blocked */

/** Which output pipeline a marker is announcing, for onset prediction. */
typedef enum {
    MARKER_MODALITY_NONE  = 0,  /**< bookkeeping only — onset == dispatch */
    MARKER_MODALITY_AUDIO = 1,
    MARKER_MODALITY_LED   = 2,
    MARKER_MODALITY_BOTH  = 3,  /**< uses the LATER of the two lags */
} marker_modality_t;

typedef struct {
    char key[MARKER_KEY_MAX];
    char val[MARKER_VAL_MAX];
} marker_kv_t;

typedef struct {
    char              label[MARKER_LABEL_MAX];
    marker_kv_t       kv[MARKER_KV_MAX];
    uint8_t           kv_count;
    marker_modality_t modality;

    /* The three times. See the file comment — do not collapse them. */
    int64_t t_sched_us;       /**< session-relative: .ledc time_ms * 1000 */
    int64_t t_disp_us;        /**< session-relative dispatch */
    int64_t t_onset_pred_us;  /**< session-relative PREDICTED output onset */

    int64_t t_disp_mono_us;   /**< raw esp_timer_get_time() at dispatch, for the
                                   GPIO sink's absolute scheduling and for the
                                   bridge's clock mapping */
    uint32_t seq;             /**< monotonic, so a sink can detect a gap */
} marker_record_t;

/** Start the flusher task and arm the configured sinks. Idempotent. */
esp_err_t marker_init(void);

/**
 * @brief Stamp and queue a marker. Safe from the timeline dispatcher; does no I/O.
 *
 * @param label      [A-Z0-9_]; truncated to MARKER_LABEL_MAX-1.
 * @param kv         may be NULL when kv_count == 0.
 * @param modality   selects the pipeline lag for the onset PREDICTION.
 * @param t_sched_us session-relative intended time.
 * @param t_disp_mono_us  esp_timer_get_time() taken ONCE at BATCH start, so a
 *                   marker shares its dispatch reference with the audio/LED
 *                   entries beside it. Pass 0 to stamp now.
 */
esp_err_t marker_emit(const char *label,
                      const marker_kv_t *kv, uint8_t kv_count,
                      marker_modality_t modality,
                      int64_t t_sched_us,
                      int64_t t_disp_mono_us);

/**
 * @brief Open a logging session: resets counters, writes the SD provenance
 *        header, and fixes the monotonic origin that every session-relative
 *        timestamp is measured against.
 *
 * @param session_name   label for the log file / records.
 * @param session_sha256 hex digest of the .ledc source, or NULL.
 * @param origin_mono_us the session's transport origin (esp_timer domain).
 */
esp_err_t marker_session_begin(const char *session_name,
                               const char *session_sha256,
                               int64_t origin_mono_us);

/** Close the logging session: footer record with counters, flush, close files. */
esp_err_t marker_session_end(void);

/** Predicted output lag for a modality, in microseconds. */
int64_t marker_pipeline_lag_us(marker_modality_t modality);

/** Counters for /api/state and the log footer. */
typedef struct {
    uint32_t emitted;
    uint32_t dropped;          /**< ring overflow — MUST surface, never be silent */
    uint32_t sink_errors;
    int64_t  max_sched_disp_delta_us;
    bool     session_open;
} marker_stats_t;

void marker_get_stats(marker_stats_t *out);

/** Most recent records, newest last, for the UI's live tail. Returns the count. */
size_t marker_get_recent(marker_record_t *out, size_t max);

/** Re-read the sink list from settings (called after a settings POST). */
esp_err_t marker_reconfigure(void);

#ifdef __cplusplus
}
#endif

#endif /* MARKER_H */
