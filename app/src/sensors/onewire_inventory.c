#include "onewire_inventory.h"

#include <zephyr/device.h>
#include <zephyr/drivers/w1.h>
#include <zephyr/logging/log.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(onewire_inventory, LOG_LEVEL_INF);

struct scan_ctx {
    struct ow_inventory *inv;
    bool en_ds;
    bool en_max;
};

static inline void ow_add_device(struct ow_inventory *inv,
                                 uint8_t fam,
                                 const struct w1_rom *rom)
{
    if (inv->count >= OW_MAX_DEVICES) {
        if (!inv->truncated) {
            inv->truncated = true;
            LOG_ERR("OW inventory overflow: count=%u max=%u -> TRUNCATE",
                    inv->count, OW_MAX_DEVICES);
        }
        return;
    }

    inv->dev[inv->count].rom = *rom;
    inv->dev[inv->count].family = fam;
    inv->count++;

    if (fam == OW_FAMILY_DS18B20) {
        inv->ds18b20_count++;
    } else if (fam == OW_FAMILY_MAX31850) {
        inv->max31850_count++;
    }
}

static void w1_search_cb(struct w1_rom rom, void *user)
{
    struct scan_ctx *ctx = (struct scan_ctx *)user;
    const uint8_t fam = rom.family;

    if (fam == OW_FAMILY_DS18B20) {
        if (!ctx->en_ds) return;
    } else if (fam == OW_FAMILY_MAX31850) {
        if (!ctx->en_max) return;
    } else {
        return;
    }

    ow_add_device(ctx->inv, fam, &rom);
}

int ow_inventory_scan(struct ow_inventory *inv,
                      const struct device *bus,
                      bool enable_ds18b20,
                      bool enable_max31850)
{
    if (!inv || !bus) {
        return -EINVAL;
    }

    memset(inv, 0, sizeof(*inv));
    inv->bus = bus;

    if (!device_is_ready(bus)) {
        LOG_ERR("1-Wire bus not ready");
        return -ENODEV;
    }

    if (w1_reset_bus(bus) <= 0) {
        LOG_WRN("1-Wire reset failed");
        return -EIO;
    }

    struct scan_ctx ctx = {
        .inv    = inv,
        .en_ds  = enable_ds18b20,
        .en_max = enable_max31850,
    };

    int rc = w1_search_bus(bus, W1_CMD_SEARCH_ROM, 0x00, w1_search_cb, &ctx);
    if (rc < 0) {
        LOG_ERR("w1_search_bus failed: %d", rc);
        return rc;
    }

    LOG_INF("w1_search_bus returned count=%d", rc);
    LOG_INF("1-Wire scan: total=%u%s (DS18B20=%u, MAX31850=%u)",
            inv->count, inv->truncated ? " (truncated)" : "",
            inv->ds18b20_count, inv->max31850_count);

    return 0;
}