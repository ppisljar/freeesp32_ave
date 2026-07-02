/**
 * @file mp3_decoder.c
 * @brief minimp3 implementation translation unit for BG MP3 playback.
 *
 * This file exists solely to compile the minimp3 decoder implementation into
 * exactly one object file (the classic single-header "IMPLEMENTATION" pattern).
 * All other code that needs the decoder just `#include "minimp3.h"` for the
 * prototypes and types; the actual function bodies live here.
 *
 * Gated by CONFIG_BG_SUPPORT_MP3 (Kconfig, default y). When the option is
 * disabled the whole decoder is compiled out and the BG player rejects MP3
 * streams — see bg_player.c. A dummy symbol keeps the translation unit legal
 * (non-empty) in that configuration.
 *
 * minimp3 upstream: https://github.com/lieff/minimp3 (public domain / CC0).
 */

#include "sdkconfig.h"

#if CONFIG_BG_SUPPORT_MP3

/* Emit the decoder implementation. mp3d_sample_t defaults to int16_t (we do NOT
 * define MINIMP3_FLOAT_OUTPUT), so decode_frame yields 16-bit PCM — the exact
 * format the BG ring's int16->float conversion tail already expects.
 * MINIMP3_ONLY_MP3 drops the MPEG-1/2 Layer I/II tables we never use. The ESP32
 * is Xtensa, so minimp3 auto-selects its portable (no-SIMD) code path. */
/* Place minimp3's ~16 KB decode scratch in PSRAM (.ext_ram.bss) instead of the
 * stack or scarce internal DRAM. Requires CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_
 * MEMORY=y; without it EXT_RAM_BSS_ATTR is a no-op and it falls back to internal
 * .bss. See the LOCAL PATCH note in minimp3.h. */
#include "esp_attr.h"
#define MINIMP3_SCRATCH_ATTR EXT_RAM_BSS_ATTR

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"

#else  /* !CONFIG_BG_SUPPORT_MP3 */

/* Keep this a legal, non-empty translation unit when MP3 support is disabled. */
typedef int mp3_decoder_disabled_translation_unit_t;

#endif /* CONFIG_BG_SUPPORT_MP3 */
