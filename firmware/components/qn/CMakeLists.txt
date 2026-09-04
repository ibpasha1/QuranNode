# The 'qn' component compiles the portable core (../../../core), the ESP32 HAL
# (../../../hal/esp32) and the reused DSP-Mini drivers (drivers/) as ONE component,
# which avoids circular inter-component dependencies (the drivers need the core's
# canvas.h, and the HAL needs both). It requires only the ESP-IDF components the
# drivers pull in.
set(QN_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../..)

file(GLOB QN_CORE
    ${QN_ROOT}/core/*.c
    ${QN_ROOT}/core/gfx/*.c
    ${QN_ROOT}/core/arabic/*.c
    ${QN_ROOT}/core/quran/*.c
    ${QN_ROOT}/core/audio/*.c
    ${QN_ROOT}/core/scene/*.c)
file(GLOB QN_HAL ${QN_ROOT}/hal/esp32/*.c)
file(GLOB QN_DRV ${CMAKE_CURRENT_LIST_DIR}/drivers/*.c)

# Al-Fatihah bundled in flash for on-bench testing without an SD card (glyph pack,
# timings, 7 ayah MP3s). The files are embedded via `board_build.embed_files` in
# platformio.ini (PlatformIO's native embed — EMBED_FILES here doesn't generate
# the .S stubs under pio); hal_fs_slurp serves them from flash and falls back to SD.
idf_component_register(
    SRCS ${QN_CORE} ${QN_HAL} ${QN_DRV}
    INCLUDE_DIRS
        ${QN_ROOT}/core
        ${QN_ROOT}/core/gfx
        ${QN_ROOT}/core/arabic
        ${QN_ROOT}/core/quran
        ${QN_ROOT}/core/audio
        ${QN_ROOT}/core/scene
        ${QN_ROOT}/hal
        drivers
    REQUIRES
        driver fatfs sdmmc vfs esp_timer nvs_flash esp_psram
        esp_wifi esp_event esp_netif esp_http_server app_update
        esp_http_client esp_https_ota esp-tls mbedtls
)

# The portable core + reused DSP-Mini drivers are warning-clean in logic but trip
# IDF's aggressive -Werror defaults (printf format widths on xtensa, snprintf
# truncation bounds, indentation style). Keep the warnings visible but don't fail
# the build on them for this component.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format
    -Wno-error=format-truncation
    -Wno-error=misleading-indentation)

