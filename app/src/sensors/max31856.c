#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(max31856, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>

#include "max31856.h"
#include "topic.h"
#include "transport.h"

/* ── MAX31856 register map ─────────────────────────────────────────────── */
#define REG_CR0         0x00
#define REG_CR1         0x01
#define REG_MASK        0x02
#define REG_CJTO        0x09
#define REG_CJTH        0x0A
#define REG_CJTL        0x0B
#define REG_LTCBH       0x0C
#define REG_LTCBM       0x0D
#define REG_LTCBL       0x0E
#define REG_SR          0x0F

#define WRITE_FLAG      0x80
#define CR0_CMODE       0x80
#define CR0_OCFAULT0    0x10
#define CR0_FAULT_CLR   0x02
#define TC_TYPE_K       0x03

#define SR_FAULT_CJ_RANGE   0x80
#define SR_FAULT_TC_RANGE   0x40
#define SR_FAULT_OV_UV      0x02
#define SR_FAULT_OPEN       0x01

/* ── CS GPIO pins — software CS ─────────────────────────────────────────── */
static const struct gpio_dt_spec cs_pins[] = {
    GPIO_DT_SPEC_GET_BY_IDX(DT_NODELABEL(spi_bb), cs_gpios, 0),  /* CS1 P1.04 */
    GPIO_DT_SPEC_GET_BY_IDX(DT_NODELABEL(spi_bb), cs_gpios, 1),  /* CS2 P1.05 */
};

/* ── Sensor struct ──────────────────────────────────────────────────────── */
struct max31856_sensor {
    const struct device *spi_dev;
    struct spi_config    spi_cfg;
    int  cs_idx;  /* index into cs_pins[] */
    bool valid;
    int  index;
};

static struct max31856_sensor sensors[] = {
#if DT_NODE_EXISTS(DT_ALIAS(max31856_0))
    {
        .spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi_bb)),
        .spi_cfg = {
            .frequency = DT_PROP(DT_ALIAS(max31856_0), spi_max_frequency),
            .operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_MODE_CPHA,
            .slave     = 0,
            .cs        = { .gpio = { 0 }, .delay = 0 },
        },
        .cs_idx = 0,
        .valid  = true,
        .index  = 1,
    },
#endif
#if DT_NODE_EXISTS(DT_ALIAS(max31856_1))
    {
        .spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi_bb)),
        .spi_cfg = {
            .frequency = DT_PROP(DT_ALIAS(max31856_1), spi_max_frequency),
            .operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_MODE_CPHA,
            .slave     = 1,
            .cs        = { .gpio = { 0 }, .delay = 0 },
        },
        .cs_idx = 1,
        .valid  = true,
        .index  = 2,
    },
#endif
#if DT_NODE_EXISTS(DT_ALIAS(max31856_2))
    {
        .spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi_bb)),
        .spi_cfg = {
            .frequency = DT_PROP(DT_ALIAS(max31856_2), spi_max_frequency),
            .operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_MODE_CPHA,
            .slave     = 2,
            .cs        = { .gpio = { 0 }, .delay = 0 },
        },
        .cs_idx = 2,
        .valid  = true,
        .index  = 3,
    },
#endif
#if DT_NODE_EXISTS(DT_ALIAS(max31856_3))
    {
        .spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi_bb)),
        .spi_cfg = {
            .frequency = DT_PROP(DT_ALIAS(max31856_3), spi_max_frequency),
            .operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_MODE_CPHA,
            .slave     = 3,
            .cs        = { .gpio = { 0 }, .delay = 0 },
        },
        .cs_idx = 3,
        .valid  = true,
        .index  = 4,
    },
#endif
};

#define NUM_SENSORS ARRAY_SIZE(sensors)

/* ── CS helpers ─────────────────────────────────────────────────────────── */
static inline void cs_assert(struct max31856_sensor *s)
{
    gpio_pin_set_dt(&cs_pins[s->cs_idx], 1);
}

static inline void cs_deassert(struct max31856_sensor *s)
{
    gpio_pin_set_dt(&cs_pins[s->cs_idx], 0);
}

/* ── SPI helpers ────────────────────────────────────────────────────────── */
static int write_reg(struct max31856_sensor *s, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { reg | WRITE_FLAG, val };
    struct spi_buf tx_buf = { .buf = tx, .len = 2 };
    struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
    cs_assert(s);
    int rc = spi_write(s->spi_dev, &s->spi_cfg, &tx_set);
    cs_deassert(s);
    return rc;
}

static int read_regs(struct max31856_sensor *s, uint8_t reg,
                     uint8_t *buf, size_t len)
{
    uint8_t tx_buf[8] = { reg & 0x7F, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t rx_buf[8] = { 0 };
    struct spi_buf tx = { .buf = tx_buf, .len = 1 + len };
    struct spi_buf rx = { .buf = rx_buf, .len = 1 + len };
    struct spi_buf_set tx_set = { .buffers = &tx, .count = 1 };
    struct spi_buf_set rx_set = { .buffers = &rx, .count = 1 };
    cs_assert(s);
    int rc = spi_transceive(s->spi_dev, &s->spi_cfg, &tx_set, &rx_set);
    cs_deassert(s);
    if (rc == 0) {
        memcpy(buf, &rx_buf[1], len);
    }
    return rc;
}

/* ── Init ───────────────────────────────────────────────────────────────── */
static int sensor_init(struct max31856_sensor *s)
{
    /* Configure CS pin as output inactive (high) */
    int rc = gpio_pin_configure_dt(&cs_pins[s->cs_idx], GPIO_OUTPUT_INACTIVE);
    if (rc) { LOG_ERR("MAX31856-%d: CS config failed %d", s->index, rc); return rc; }

    if (!device_is_ready(s->spi_dev)) {
        LOG_ERR("MAX31856-%d: SPI not ready", s->index);
        return -ENODEV;
    }

    rc = write_reg(s, REG_CR0, CR0_FAULT_CLR);
    if (rc) { LOG_ERR("MAX31856-%d: fault clear failed %d", s->index, rc); return rc; }
    k_msleep(2);

    rc = write_reg(s, REG_CR1, (0x03 << 4) | TC_TYPE_K);
    if (rc) { LOG_ERR("MAX31856-%d: write CR1 failed %d", s->index, rc); return rc; }

    rc = write_reg(s, REG_MASK, 0x00);
    if (rc) { LOG_ERR("MAX31856-%d: write MASK failed %d", s->index, rc); return rc; }

    rc = write_reg(s, REG_CR0, CR0_CMODE | CR0_OCFAULT0);
    if (rc) { LOG_ERR("MAX31856-%d: write CR0 failed %d", s->index, rc); return rc; }

    LOG_INF("MAX31856-%d: initialized (K-type, continuous mode)", s->index);
    return 0;
}

/* ── Read ───────────────────────────────────────────────────────────────── */
static int sensor_read(struct max31856_sensor *s,
                       int32_t *tc_temp_cdeg,
                       int32_t *cj_temp_cdeg,
                       uint8_t *fault_reg)
{
    uint8_t buf[6];
    int rc = read_regs(s, REG_CJTH, buf, sizeof(buf));
    if (rc) { return rc; }

    int16_t cj_raw = (int16_t)(buf[0] << 8 | buf[1]);
    *cj_temp_cdeg = (int32_t)cj_raw * 100 / 256;

    int32_t tc_raw = (int32_t)((uint32_t)buf[2] << 16 |
                                (uint32_t)buf[3] << 8  |
                                (uint32_t)buf[4]);
    if (tc_raw & BIT(18)) {
        tc_raw |= ~((1 << 19) - 1);
    }
    tc_raw >>= 5;
    *tc_temp_cdeg = tc_raw * 100 / 128;
    *fault_reg = buf[5];
    return 0;
}

/* ── Fault string ───────────────────────────────────────────────────────── */
static const char *fault_str(uint8_t sr)
{
    if (sr == 0)                return "NONE";
    if (sr & SR_FAULT_OPEN)     return "OC";
    if (sr & SR_FAULT_OV_UV)    return "OV";
    if (sr & SR_FAULT_TC_RANGE) return "TC_RANGE";
    if (sr & SR_FAULT_CJ_RANGE) return "CJ_RANGE";
    return "FAULT";
}

/* ── Publish helpers ────────────────────────────────────────────────────── */
static void publish_int(const char *root, const char *sensor_name,
                        const char *field, int32_t value)
{
    char topic[80];
    char payload[16];
    if (topic_build(topic, sizeof(topic), root, "OUT", sensor_name, field) != 0) return;
    snprintk(payload, sizeof(payload), "%d", (int)value);
    int rc = transport_publish(topic, payload);
    if (rc < 0) { LOG_WRN("PUB %s failed rc=%d", topic, rc); }
    else        { LOG_INF("PUB %s = %s", topic, payload); }
}

static void publish_str(const char *root, const char *sensor_name,
                        const char *field, const char *value)
{
    char topic[80];
    if (topic_build(topic, sizeof(topic), root, "OUT", sensor_name, field) != 0) return;
    int rc = transport_publish(topic, value);
    if (rc < 0) { LOG_WRN("PUB %s failed rc=%d", topic, rc); }
    else        { LOG_INF("PUB %s = %s", topic, value); }
}

/* ── Public API ─────────────────────────────────────────────────────────── */
static bool initialized;

void max31856_sample_and_publish(const char *root)
{
    if (!initialized) {
        for (size_t i = 0; i < NUM_SENSORS; i++) {
            if (sensors[i].valid) {
                sensor_init(&sensors[i]);
            }
        }
        k_msleep(500);
        initialized = true;
    }

    for (size_t i = 0; i < NUM_SENSORS; i++) {
        struct max31856_sensor *s = &sensors[i];
        if (!s->valid) continue;

        char sensor_name[20];
        snprintk(sensor_name, sizeof(sensor_name), "MAX31856-%d", s->index);

        int32_t tc_temp, cj_temp;
        uint8_t fault;

        int rc = sensor_read(s, &tc_temp, &cj_temp, &fault);
        if (rc) {
            LOG_WRN("%s: read failed rc=%d", sensor_name, rc);
            continue;
        }

        int tc_abs = tc_temp < 0 ? -tc_temp : tc_temp;
        int cj_abs = cj_temp < 0 ? -cj_temp : cj_temp;
        LOG_INF("%s: TC=%s%d.%02d C, CJ=%s%d.%02d C, fault=0x%02x",
                sensor_name,
                tc_temp < 0 ? "-" : "", tc_abs / 100, tc_abs % 100,
                cj_temp < 0 ? "-" : "", cj_abs / 100, cj_abs % 100,
                fault);

        publish_int(root, sensor_name, "TEMP",  tc_temp);
        publish_int(root, sensor_name, "CJ",    cj_temp);
        publish_str(root, sensor_name, "FAULT", fault_str(fault));
    }
}
