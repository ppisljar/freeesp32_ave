# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# The vendored components/esp-dsp predates esp-dsp's multi-target support: its
# CMakeLists unconditionally assembles ESP32-classic LX6 sources (*_ae32.S),
# which the ESP32-S3 (LX7) toolchain cannot build. Nothing in main/ calls any
# dsps_*/dspm_* function, so the whole component is dead weight — exclude it
# rather than carry a target-split of code we never call. Delete the directory
# (it is untracked) if you are sure you will never want it back.
set(EXCLUDE_COMPONENTS esp-dsp)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_audioplayer)
