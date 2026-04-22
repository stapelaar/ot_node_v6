#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sen50, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <string.h>
#include <errno.h>

#include "sen50.h"
#include "topic.h"
#include "transport.h"

/* SEN50 lives on I2C alias i2c0 at address 0x69 */
#define SEN50_I2C_ADDR  0x69

static const struct device *const i2c_bus = DEVICE_DT_GET(DT_ALIAS(i2c0));

/* SEN5x command set (big-endian 2-byte command codes) */
#define CMD_START_MEAS          0x0021
#define CMD_STOP_MEAS           0x0104
#define CMD_READ_DATA_READY     0x0202
#define CMD_READ_VALUES         0x03C4
#define CMD_RESET               0xD304

/* Timing per datasheet (ms) */
#define T_AFTER_START_MEAS      50
#define T_AFTER_STOP_MEAS       200
#define T_AFTER_RESET           100
#define T_BEFORE_READ_VALUES    20

/* First reliable data appears ~8 seconds after start of periodic measurement */
#define WARMUP_MS               8000

static bool s_started;
static int64_t s_warmup_deadline;

/* ── Sensirion CRC-8 (poly 0x31, init 0xFF) ────────────────────────────── */
static uint8_t sensirion_crc(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* ── Low-level I2C helpers ─────────────────────────────────────────────── */
static int sen50_write_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    return i2c_write(i2c_bus, buf, sizeof(buf), SEN50_I2C_ADDR);
}

/* Send command, then read N 2-byte words (each followed by CRC-8).
 * Returns 0 on success with word values written to `words[]`. */
static int sen50_read_words(uint16_t cmd, uint16_t *words, size_t word_count,
                            uint32_t delay_ms)
{
    int rc = sen50_write_cmd(cmd);
    if (rc < 0) {
        return rc;
    }

    if (delay_ms) {
        k_msleep(delay_ms);
    }

    uint8_t rx[30];
    size_t rx_len = word_count * 3;
    if (rx_len > sizeof(rx)) {
        return -EINVAL;
    }

    rc = i2c_read(i2c_bus, rx, rx_len, SEN50_I2C_ADDR);
    if (rc < 0) {
        return rc;
    }

    for (size_t i = 0; i < word_count; i++) {
        uint8_t *p = &rx[i * 3];
        if (sensirion_crc(p, 2) != p[2]) {
            LOG_WRN("CRC mismatch on word %u", (unsigned)i);
            return -EBADMSG;
        }
        words[i] = ((uint16_t)p[0] << 8) | p[1];
    }
    return 0;
}

/* ── High-level sensor ops ─────────────────────────────────────────────── */

static int sen50_start_measurement(void)
{
    int rc = sen50_write_cmd(CMD_START_MEAS);
    if (rc < 0) return rc;
    k_msleep(T_AFTER_START_MEAS);
    return 0;
}

/* Returns 1 if ready, 0 if not, negative on error */
static int sen50_is_ready(void)
{
    uint16_t w;
    int rc = sen50_read_words(CMD_READ_DATA_READY, &w, 1, T_BEFORE_READ_VALUES);
    if (rc < 0) return rc;
    return (w & 0x0001) ? 1 : 0;
}

struct sen50_values {
    uint16_t pm1p0_raw;
    uint16_t pm2p5_raw;
    uint16_t pm4p0_raw;
    uint16_t pm10_raw;
};

static int sen50_read_values(struct sen50_values *out)
{
    /* Data layout per datasheet: 8 words × (2 bytes + CRC)
     *   [0] PM1.0  (scale 0.1 µg/m³)
     *   [1] PM2.5  (scale 0.1 µg/m³)
     *   [2] PM4.0  (scale 0.1 µg/m³)
     *   [3] PM10   (scale 0.1 µg/m³)
     *   [4..7] humidity/temp/VOC/NOx - unused on SEN50
     */
    uint16_t w[8];
    int rc = sen50_read_words(CMD_READ_VALUES, w, 8, T_BEFORE_READ_VALUES);
    if (rc < 0) return rc;

    out->pm1p0_raw = w[0];
    out->pm2p5_raw = w[1];
    out->pm4p0_raw = w[2];
    out->pm10_raw  = w[3];
    return 0;
}

/* ── Publish helper ────────────────────────────────────────────────────── */

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

/* ── Public API ────────────────────────────────────────────────────────── */

void sen50_sample_and_publish(const char *root)
{
    if (!device_is_ready(i2c_bus)) {
        static bool warned;
        if (!warned) {
            LOG_WRN("SEN50: I2C bus not ready");
            warned = true;
        }
        return;
    }

    /* One-time: start periodic measurement mode */
    if (!s_started) {
        int rc = sen50_start_measurement();
        if (rc < 0) {
            static bool warned;
            if (!warned) {
                LOG_WRN("SEN50: start_measurement failed rc=%d", rc);
                warned = true;
            }
            return;
        }
        s_warmup_deadline = k_uptime_get() + WARMUP_MS;
        s_started = true;
        LOG_INF("SEN50: measurement started, warm-up %d ms", WARMUP_MS);
    }

    /* During warm-up just consume samples to keep the sensor active */
    if (k_uptime_get() < s_warmup_deadline) {
        LOG_INF("SEN50: warm-up in progress, skipping publish");
        return;
    }

    int ready = sen50_is_ready();
    if (ready < 0) {
        LOG_WRN("SEN50: data_ready failed rc=%d", ready);
        return;
    }
    if (ready == 0) {
        LOG_INF("SEN50: data not ready yet");
        return;
    }

    struct sen50_values v;
    int rc = sen50_read_values(&v);
    if (rc < 0) {
        LOG_WRN("SEN50: read_values failed rc=%d", rc);
        return;
    }

    /* Raw values are in units of 0.1 µg/m³.
     * Publish as deci-µg/m³ (same scale as raw), consumer divides by 10. */
    LOG_INF("SEN50: PM1.0=%u.%u, PM2.5=%u.%u, PM4.0=%u.%u, PM10=%u.%u µg/m³",
            v.pm1p0_raw / 10, v.pm1p0_raw % 10,
            v.pm2p5_raw / 10, v.pm2p5_raw % 10,
            v.pm4p0_raw / 10, v.pm4p0_raw % 10,
            v.pm10_raw  / 10, v.pm10_raw  % 10);

    publish_field(root, "SEN50-1", "PM1P0", (int32_t)v.pm1p0_raw);
    publish_field(root, "SEN50-1", "PM2P5", (int32_t)v.pm2p5_raw);
    publish_field(root, "SEN50-1", "PM4P0", (int32_t)v.pm4p0_raw);
    publish_field(root, "SEN50-1", "PM10",  (int32_t)v.pm10_raw);
}