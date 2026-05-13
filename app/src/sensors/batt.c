#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(batt, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>

#include "batt.h"
#include "topic.h"
#include "transport.h"

/* ── Hardware constants (from schematic) ─────────────────────────────────
 * P1.14 = AIN7  — battery voltage via 10K/10K divider
 * P1.15 = GPIO  — VBAT_EN: enable pin for TPS22916 load switch
 * Divider ratio: Vbat = Vadc * 2.0
 * ADC ref: 900mV internal, gain 1/3 → max input = 2700mV → max Vbat = 5400mV
 * ──────────────────────────────────────────────────────────────────────── */

#define BATT_ADC_CHANNEL    7        /* AIN7 */
#define BATT_VBAT_EN_PORT   1        /* P1.xx */
#define BATT_VBAT_EN_PIN    15       /* P1.15 */
#define BATT_DIVIDER_FACTOR 2        /* Vbat = Vadc * 2 */
#define BATT_ADC_RESOLUTION 12
#define BATT_ADC_OVERSAMPLING 4
#define BATT_SETTLE_MS      5        /* settle time after VBAT_EN */

/* ── ADC device ──────────────────────────────────────────────────────────── */

static const struct device *adc_dev = DEVICE_DT_GET(DT_NODELABEL(adc));

static const struct adc_channel_cfg chan_cfg = {
    .gain             = ADC_GAIN_1_3,
    .reference        = ADC_REF_INTERNAL,
    .acquisition_time = ADC_ACQ_TIME_DEFAULT,
    .channel_id       = BATT_ADC_CHANNEL,
    .input_positive   = 7,   /* AIN7 = P1.14, 0-indexed */
};

static int16_t  adc_buf;
static struct adc_sequence adc_seq = {
    .channels     = BIT(BATT_ADC_CHANNEL),
    .buffer       = &adc_buf,
    .buffer_size  = sizeof(adc_buf),
    .resolution   = BATT_ADC_RESOLUTION,
    .oversampling = BATT_ADC_OVERSAMPLING,
};

/* ── VBAT_EN GPIO ────────────────────────────────────────────────────────── */

static const struct gpio_dt_spec vbat_en = {
    .port     = DEVICE_DT_GET(DT_NODELABEL(gpio1)),
    .pin      = BATT_VBAT_EN_PIN,
    .dt_flags = GPIO_ACTIVE_HIGH,
};

/* ── LiPo discharge curve: mV → % ───────────────────────────────────────── */

static const struct { uint16_t mv; uint8_t pct; } lipo_curve[] = {
    { 4200, 100 },
    { 4100,  90 },
    { 4000,  80 },
    { 3900,  70 },
    { 3800,  60 },
    { 3700,  50 },
    { 3600,  40 },
    { 3500,  30 },
    { 3400,  20 },
    { 3300,  10 },
    { 3000,   0 },
};

static uint8_t mv_to_pct(uint16_t mv)
{
    if (mv >= lipo_curve[0].mv) return 100;
    if (mv <= lipo_curve[ARRAY_SIZE(lipo_curve)-1].mv) return 0;

    for (size_t i = 0; i < ARRAY_SIZE(lipo_curve) - 1; i++) {
        if (mv >= lipo_curve[i+1].mv) {
            uint16_t range_mv  = lipo_curve[i].mv - lipo_curve[i+1].mv;
            uint8_t  range_pct = lipo_curve[i].pct - lipo_curve[i+1].pct;
            uint16_t offset_mv = mv - lipo_curve[i+1].mv;
            return lipo_curve[i+1].pct +
                   (uint8_t)((offset_mv * range_pct) / range_mv);
        }
    }
    return 0;
}

/* ── Init (lazy, once) ───────────────────────────────────────────────────── */

static bool initialized;

static int batt_init(void)
{
    if (initialized) return 0;

    if (!device_is_ready(adc_dev)) {
        LOG_ERR("BATT: ADC device not ready");
        return -ENODEV;
    }

    int rc = adc_channel_setup(adc_dev, &chan_cfg);
    if (rc < 0) {
        LOG_ERR("BATT: adc_channel_setup failed: %d", rc);
        return rc;
    }

    if (!gpio_is_ready_dt(&vbat_en)) {
        LOG_ERR("BATT: VBAT_EN GPIO not ready");
        return -ENODEV;
    }

    rc = gpio_pin_configure_dt(&vbat_en, GPIO_OUTPUT_INACTIVE);
    if (rc < 0) {
        LOG_ERR("BATT: VBAT_EN configure failed: %d", rc);
        return rc;
    }

    initialized = true;
    LOG_INF("BATT: initialized (AIN7/P1.14, VBAT_EN/P1.15)");
    return 0;
}

/* ── Publish helper ──────────────────────────────────────────────────────── */

static void publish_field(const char *root, const char *field, int32_t value)
{
    char topic[64];
    char payload[16];

    if (topic_build(topic, sizeof(topic), root, "OUT", "BATT", field) != 0) {
        return;
    }
    snprintk(payload, sizeof(payload), "%d", (int)value);
    int rc = transport_publish(topic, payload);
    if (rc < 0) {
        LOG_WRN("PUB %s failed rc=%d", topic, rc);
    } else {
        LOG_INF("PUB %s = %s", topic, payload);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void batt_sample_and_publish(const char *root)
{
    if (batt_init() != 0) {
        return;
    }

    /* Enable voltage divider */
    gpio_pin_set_dt(&vbat_en, 1);
    k_msleep(BATT_SETTLE_MS);

    int rc = adc_read(adc_dev, &adc_seq);

    /* Disable voltage divider immediately */
    gpio_pin_set_dt(&vbat_en, 0);

    if (rc < 0) {
        LOG_WRN("BATT: adc_read failed: %d", rc);
        return;
    }

    /* Convert raw to mV:
     * ref = 900mV internal, gain = 1/3, resolution = 12 bit
     * Vadc_mv = raw * (900 * 3) / 4096 = raw * 2700 / 4096 */
    int32_t vadc_mv = (int32_t)adc_buf * 2700 / 4096;

    /* Apply voltage divider correction */
    uint16_t vbat_mv = (uint16_t)(vadc_mv * BATT_DIVIDER_FACTOR);
    uint8_t  pct     = mv_to_pct(vbat_mv);

    LOG_INF("BATT: raw=%d vadc=%dmV vbat=%umV pct=%u%%",
            adc_buf, (int)vadc_mv, vbat_mv, pct);

    publish_field(root, "MV",  (int32_t)vbat_mv);
    publish_field(root, "PCT", (int32_t)pct);
}
