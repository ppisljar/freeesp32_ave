idf_component_register(SRCS "esp32_audioplayer.c" "audio_manager.c" "audio_generator.c" "led_strip.c" "audio_test.c" "config_parser.c" "web_server.c" "wifi_manager.c" "led_matrix_example.c" "timing_engine.c" "lock_free_comm.c" "isr_profiling.c" "memory_pool.c" "bg_player.c" "wav_parser.c" "mp3_decoder.c" "audio_driver.c" "audio_driver_ac101.c" "audio_driver_es8388.c" "mod_engine.c" "settings.c"
                    INCLUDE_DIRS ".")

# Build the web UI SPIFFS image from ../web/data (the gzipped output of the
# esbuild frontend build) and flash it into the "storage" partition alongside
# the app (idf.py flash). web_server.c serves these from /spiffs with gzip
# Content-Encoding. IMPORTANT: run `npm install && npm run build` in ../web
# first to (re)generate ../web/data — the firmware build does not run npm.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../web/data/index.html.gz)
    message(WARNING "web/data/index.html.gz missing — run 'npm run build' in the web/ directory. The web UI will be empty.")
endif()
spiffs_create_partition_image(storage ../web/data FLASH_IN_PROJECT)

# Seed the "cfgfs" partition with the built-in session library
# (../sessions/library/*.ledc). configstore (web_server.c) lists configs from
# cfgfs, so packing the library here makes the starter sessions appear in the
# web UI config list out of the box. The directory contains ONLY .ledc files
# (plans/ and guidelines live one level up, excluded). NOTE: FLASH_IN_PROJECT
# means a full `idf.py flash` / flash_all.sh OVERWRITES cfgfs — user-saved
# configs are replaced by the shipped library. flash_web.sh (storage-only) and
# OTA (ota_0 app only) do NOT touch cfgfs, so routine updates preserve them.
spiffs_create_partition_image(cfgfs ../sessions/library FLASH_IN_PROJECT)
