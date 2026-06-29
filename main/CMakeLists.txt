idf_component_register(SRCS "esp32_audioplayer.c" "audio_manager.c" "audio_generator.c" "led_strip.c" "audio_test.c" "config_parser.c" "web_server.c" "wifi_manager.c" "led_matrix_example.c" "timing_engine.c" "lock_free_comm.c" "isr_profiling.c" "memory_pool.c" "bg_player.c" "wav_parser.c" "audio_driver.c" "audio_driver_ac101.c" "audio_driver_es8388.c" "mod_engine.c"
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
