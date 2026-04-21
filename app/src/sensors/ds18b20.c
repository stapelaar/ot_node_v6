#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ds18b20, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/w1.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "ds18b20.h"
#include "onewire_inventory.h"
#include "topic.h"
#include "transport.h"

#define CMD_MATCH_ROM     0x55
#define CMD_CONVERT_T     0x44
#define CMD_READ_SCRATCH  0xBE
#define SCRATCHPAD_SIZE   9
#define TCONV_MS          750

static const struct device *const w1_bus = DEVICE_DT_GET(DT_ALIAS(onewire0));

static struct ow_inventory s_inv;
static bool s_scanned;

static uint8_t crc8_maxim(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int j = 0; j < 8; j++) {
            uint8_t mix = (crc ^ b) & 0x01;
            crc >>= 1;
            if (mix) crc ^= 0x8C;
            b >>= 1;
        }
    }
    return crc;
}

static int match_rom_frame(const struct device *bus, const struct w1_rom *rom)
{
    uint8_t frame[1 + sizeof(*rom)];
    frame[0] = CMD_MATCH_ROM;
    memcpy(frame + 1, &rom->family, sizeof(*rom));
    return w1_write_block(bus, frame, sizeof(frame));
}

static int ds18b20_read_one(const struct device *bus,
                            const struct w1_rom *rom,
                            int32_t *temp_mC)
{
    if (w1_reset_bus(bus) <= 0)                 return -EIO;
    if (match_rom_frame(bus, rom) != 0)         return -EIO;
    if (w1_write_byte(bus, CMD_CONVERT_T) != 0) return -EIO;

    k_msleep(TCONV_MS);

    if (w1_reset_bus(bus) <= 0)                    return -EIO;
    if (match_rom_frame(bus, rom) != 0)            return -EIO;
    if (w1_write_byte(bus, CMD_READ_SCRATCH) != 0) return -EIO;

    uint8_t sp[SCRATCHPAD_SIZE];
    if (w1_read_block(bus, sp, SCRATCHPAD_SIZE) != 0) return -EIO;

    if (crc8_maxim(sp, 8) != sp[8]) return -EBADMSG;

    int16_t raw = (int16_t)((sp[1] << 8) | sp[0]);
    *temp_mC = ((int32_t)raw * 1000) / 16;
    return 0;
}

static void publish_field(const char *root, const char *sensor,
                          const char *field, int32_t value)
{
    char topic[64];
    char payload[24];

    if (topic_build(topic, sizeof(topic), root, "OUT", sensor, field) != 0) {
        return;
    }

    snprintk(payload, sizeof(payload), "%d", (int)value);
    int rc = transport_publish(topic, payload);

    if (rc < 0) {
        LOG_WRN("PUB %s failed rc=%d", topic, rc);
    } else {
        LOG_INF("PUB %s OK", topic);
    }
}

void ds18b20_sample_and_publish(const char *root)
{
    if (!s_scanned) {
        if (!device_is_ready(w1_bus)) {
            static bool warned;
            if (!warned) {
                LOG_WRN("DS18B20: 1-Wire bus not ready");
                warned = true;
            }
            return;
        }

        if (ow_inventory_scan(&s_inv, w1_bus, true, false) != 0) {
            LOG_WRN("DS18B20: inventory scan failed");
            return;
        }
        s_scanned = true;
    }

    if (s_inv.ds18b20_count == 0) {
        static bool warned;
        if (!warned) {
            LOG_WRN("DS18B20: no sensors found on bus");
            warned = true;
        }
        return;
    }

    int ds_index = 0;
    for (uint8_t i = 0; i < s_inv.count; i++) {
        if (s_inv.dev[i].family != OW_FAMILY_DS18B20) {
            continue;
        }
        ds_index++;

        int32_t temp_mC = 0;
        int rc = ds18b20_read_one(w1_bus, &s_inv.dev[i].rom, &temp_mC);

        char sensor_name[20];
        snprintk(sensor_name, sizeof(sensor_name), "DS18B20-%d", ds_index);

        if (rc == 0) {
            int32_t temp_centi = (temp_mC >= 0) ? (temp_mC + 5) / 10
                                                : (temp_mC - 5) / 10;

            LOG_INF("DS18B20-%d: T=%d.%02d C",
                    ds_index, temp_centi / 100, abs(temp_centi % 100));

            publish_field(root, sensor_name, "TEMP", temp_centi);
        } else {
            LOG_WRN("DS18B20-%d: read failed rc=%d", ds_index, rc);
        }
    }
}