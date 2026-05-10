#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_settings, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/fs/zms.h>
#include <zephyr/storage/flash_map.h>

#include "app_settings.h"

/* ZMS NVS ID's — voeg hier nieuwe keys toe als nodig */
#define ZMS_ID_INTERVAL_S   1

#define ZMS_PARTITION        storage_partition
#define ZMS_PARTITION_DEVICE FIXED_PARTITION_DEVICE(ZMS_PARTITION)
#define ZMS_PARTITION_OFFSET FIXED_PARTITION_OFFSET(ZMS_PARTITION)
#define ZMS_PARTITION_SIZE   FIXED_PARTITION_SIZE(ZMS_PARTITION)

static struct zms_fs zms;
static uint32_t s_interval_s = CONFIG_APP_MEASUREMENT_PERIOD_S;

int app_settings_init(void)
{
    struct flash_pages_info info;
    const struct device *flash_dev = ZMS_PARTITION_DEVICE;

    if (!device_is_ready(flash_dev)) {
        LOG_ERR("Flash device not ready");
        return -ENODEV;
    }

    int rc = flash_get_page_info_by_offs(flash_dev, ZMS_PARTITION_OFFSET, &info);
    if (rc < 0) {
        LOG_ERR("flash_get_page_info failed: %d", rc);
        return rc;
    }

    zms.flash_device = flash_dev;
    zms.offset       = ZMS_PARTITION_OFFSET;
    zms.sector_size  = info.size;
    zms.sector_count = 2U;

    rc = zms_mount(&zms);
    if (rc < 0) {
        LOG_ERR("zms_mount failed: %d", rc);
        return rc;
    }

    /* Laad opgeslagen interval, gebruik Kconfig default als niet aanwezig */
    uint32_t stored = 0;
    ssize_t  n      = zms_read(&zms, ZMS_ID_INTERVAL_S, &stored, sizeof(stored));
    if (n == sizeof(stored) && stored >= 1 && stored <= 86400) {
        s_interval_s = stored;
        LOG_INF("app_settings: interval=%u s (from ZMS)", s_interval_s);
    } else {
        s_interval_s = CONFIG_APP_MEASUREMENT_PERIOD_S;
        LOG_INF("app_settings: interval=%u s (Kconfig default)", s_interval_s);
    }

    return 0;
}

uint32_t app_settings_get_interval_s(void)
{
    return s_interval_s;
}

int app_settings_set_interval_s(uint32_t seconds)
{
    if (seconds < 1 || seconds > 86400) {
        LOG_ERR("app_settings: interval %u out of range (1..86400)", seconds);
        return -EINVAL;
    }

    s_interval_s = seconds;

    ssize_t rc = zms_write(&zms, ZMS_ID_INTERVAL_S, &seconds, sizeof(seconds));
    if (rc < 0) {
        LOG_ERR("app_settings: zms_write failed: %d", (int)rc);
        return (int)rc;
    }

    LOG_INF("app_settings: interval set to %u s (persisted)", seconds);
    return 0;
}

SYS_INIT(app_settings_init, APPLICATION, 40);
