#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(max31850, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/w1.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

#include "max31850.h"
#include "onewire_inventory.h"
#include "topic.h"
#include "transport.h"

#define CMD_MATCH_ROM     0x55
#define CMD_CONVERT_T     0x44
#define CMD_READ_SCRATCH  0xBE
#define SCRATCHPAD_SIZE   9
#define TCONV_MS          100

#define FAULT_OC  0x01
#define FAULT_SCG 0x02
#define FAULT_SCV 0x04

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

struct max31850_reading {
    int32_t tc_centi;
    int32_t cj_centi;
    uint8_t fault_bits;
};

static int max31850_read_one(const struct device *bus,
                             const struct w1_rom *rom,
                             struct max31850_reading *out)
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

    /* Thermocouple: 14-bit signed, 0.25 C/LSB */
    int16_t tc_raw = (int16_t)((sp[1] << 8) | sp[0]);
    tc_raw >>= 2;
    out->tc_centi = (int32_t)tc_raw * 25;

    /* Cold junction: 12-bit signed, 0.0625 C/LSB */
    int16_t cj_raw = (int16_t)((sp[3] << 8) | sp[2]);
    cj_raw >>= 4;
    out->cj_centi = ((int32_t)cj_raw * 625) / 100;

    out->fault_bits = 0;
    if (sp[0] & 0x01) {
        if (sp[2] & FAULT_OC)  out->fault_bits |= FAULT_OC;
        if (sp[2] & FAULT_SCG) out->fault_bits |= FAULT_SCG;
        if (sp[2] & FAULT_SCV) out->fault_bits |= FAULT_SCV;
    }

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

static void publish_fault(const char *root, const char *sensor, uint8_t bits)
{
    char topic[64];
    char payload[16];

    if (topic_build(topic, sizeof(topic), root, "OUT", sensor, "FAULT") != 0) {
        return;
    }

    if (bits == 0) {
        strncpy(payload, "NONE", sizeof(payload));
    } else {
        int w = 0;
        if (bits & FAULT_OC)  w += snprintk(payload + w, sizeof(payload) - w,
                                            "%sOC",  w ? "|" : "");
        if (bits & FAULT_SCG) w += snprintk(payload + w, sizeof(payload) - w,
                                            "%sSCG", w ? "|" : "");
        if (bits & FAULT_SCV) w += snprintk(payload + w, sizeof(payload) - w,
                                            "%sSCV", w ? "|" : "");
    }

    int rc = transport_publish(topic, payload);
    if (rc < 0) {
        LOG_WRN("PUB %s failed rc=%d", topic, rc);
    } else {
        LOG_INF("PUB %s = %s", topic, payload);
    }
}

void max31850_sample_and_publish(const char *root)
{
    if (!s_scanned) {
        if (!device_is_ready(w1_bus)) {
            static bool warned;
            if (!warned) {
                LOG_WRN("MAX31850: 1-Wire bus not ready");
                warned = true;
            }
            return;
        }

        if (ow_inventory_scan(&s_inv, w1_bus, false, true) != 0) {
            LOG_WRN("MAX31850: inventory scan failed");
            return;
        }
        s_scanned = true;
    }

    if (s_inv.max31850_count == 0) {
        static bool warned;
        if (!warned) {
            LOG_WRN("MAX31850: no sensors found on bus");
            warned = true;
        }
        return;
    }

    int idx = 0;
    for (uint8_t i = 0; i < s_inv.count; i++) {
        if (s_inv.dev[i].family != OW_FAMILY_MAX31850) {
            continue;
        }
        idx++;

        struct max31850_reading r = {0};
        int rc = max31850_read_one(w1_bus, &s_inv.dev[i].rom, &r);

        char sensor_name[20];
        snprintk(sensor_name, sizeof(sensor_name), "MAX31850-%d", idx);

        if (rc == 0) {
            LOG_INF("MAX31850-%d: TC=%d.%02d C, CJ=%d.%02d C, fault=0x%02x",
                    idx,
                    r.tc_centi / 100, abs(r.tc_centi % 100),
                    r.cj_centi / 100, abs(r.cj_centi % 100),
                    r.fault_bits);

            publish_field(root, sensor_name, "TEMP", r.tc_centi);
            publish_field(root, sensor_name, "CJ",   r.cj_centi);
            publish_fault(root, sensor_name, r.fault_bits);
        } else {
            LOG_WRN("MAX31850-%d: read failed rc=%d", idx, rc);
        }
    }
}