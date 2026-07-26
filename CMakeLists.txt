cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "v2-phase4d.4-dev")

# ccache is disabled deliberately (2026-07-26). DO NOT re-enable without
# re-testing the finding below.
#
# With ccache enabled, a CLEAN build fails with an internal compiler error
# (segmentation fault) in ESP-IDF's own
# components/esp_lcd/rgb/esp_lcd_panel_rgb.c at line 700, under
# xtensa-esp-elf GCC 14.2.0 with ESP-IDF v5.5.3. ccache compiles
# preprocessed source rather than the original translation unit, which is a
# different path through the compiler, and this toolchain crashes on that
# path for this file. Clearing the cache does not help - the crash is
# deterministic, not a corrupt cache entry.
#
# This was latent here: incremental builds reused cached objects and
# succeeded, so it only surfaced when the build directory was removed. The
# identical defect was diagnosed and fixed the same way in
# ArgusControl_PumpHMI-Rotary-V1; see its CMakeLists.txt and
# docs/hardware/BUILD_ENVIRONMENT.md for the full reproduction.
#
# This controller has no display and never uses esp_lcd, but ESP-IDF's
# default component set compiles every discovered component, so the file is
# built regardless. Cost of the workaround is slower rebuilds; the benefit
# is that a clean, reproducible build works at all.
set(CCACHE_ENABLE 0)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ArgusControl_PumpController-V2)
