#pragma once

#include <zephyr/device.h>
#include <zephyr/drivers/w1.h>
#include <stdint.h>
#include <stdbool.h>

#define OW_MAX_DEVICES 10

/* 1-Wire family codes */
#define OW_FAMILY_DS18B20   0x28
#define OW_FAMILY_MAX31850  0x3B

struct ow_entry {
    struct w1_rom rom;
    uint8_t family;
};

struct ow_inventory {
    const struct device *bus;

    struct ow_entry dev[OW_MAX_DEVICES];
    uint8_t count;
    bool truncated;

    uint8_t ds18b20_count;
    uint8_t max31850_count;
};

/* Scan bus once. Filters out families that are not enabled. */
int ow_inventory_scan(struct ow_inventory *inv,
                      const struct device *bus,
                      bool enable_ds18b20,
                      bool enable_max31850);