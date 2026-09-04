#include "sd_card.h"
#include "pin_config.h"

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "SD_CARD";
static const char *MOUNT_POINT = "/sdcard";

// Conservative SPI clock for reliability on jumper/breadboard wiring. 20MHz (the
// SDSPI default) often times out on long leads; 4MHz is rock-solid and still
// ~0.5MB/s. Raise toward 20000 once the SD is on a short/clean PCB trace.
#define SD_FREQ_KHZ 4000

static sdmmc_card_t *s_card = NULL;
static sdmmc_host_t s_host = SDSPI_HOST_DEFAULT();
static bool s_mounted = false;
static bool s_bus_initialized = false;

esp_err_t sd_card_mount(void)
{
    if (s_mounted) return ESP_OK;

    // SPI bus config — only initialize once
    if (!s_bus_initialized) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = PIN_SD_MOSI,
            .miso_io_num = PIN_SD_MISO,
            .sclk_io_num = PIN_SD_CLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 4096,
        };

        s_host.slot = SD_SPI_HOST;

        esp_err_t ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        s_bus_initialized = true;
    }

    // Conservative clock for jumper wiring (see SD_FREQ_KHZ above).
    s_host.max_freq_khz = SD_FREQ_KHZ;

    // Internal pull-ups on the data lines — breadboard/jumper setups usually lack
    // the external pull-ups SPI-SD wants, a common cause of ESP_ERR_TIMEOUT.
    gpio_set_pull_mode(PIN_SD_MISO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_SD_CS,   GPIO_PULLUP_ONLY);

    // SD SPI device config
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_SD_CS;
    slot_config.host_id = SD_SPI_HOST;

    // FATFS mount config
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    // SD init can be flaky on the first try (card power-up timing) — retry a few.
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 1; attempt <= 3; attempt++) {
        ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &s_host, &slot_config,
                                      &mount_config, &s_card);
        if (ret == ESP_OK) break;
        ESP_LOGW(TAG, "SD mount attempt %d/3 failed: %s", attempt, esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Mount failed: %s (CS=GPIO%d, MOSI=GPIO%d, CLK=GPIO%d, MISO=GPIO%d) "
                      "@ %dkHz — check card seated + FAT32, and add 10k pull-ups on MISO/CS if it persists",
                 esp_err_to_name(ret), PIN_SD_CS, PIN_SD_MOSI, PIN_SD_CLK, PIN_SD_MISO, SD_FREQ_KHZ);
        s_card = NULL;
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

void sd_card_unmount(void)
{
    if (!s_mounted) return;

    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    spi_bus_free(SD_SPI_HOST);
    s_card = NULL;
    s_mounted = false;
    s_bus_initialized = false;
    ESP_LOGI(TAG, "SD card unmounted");
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}
