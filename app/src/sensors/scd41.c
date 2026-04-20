#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(scd41, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <string.h>

#include "topic.h"
#include "transport.h"

/* ============================================================
 * DEVICE BINDING (devicetree)
 * ============================================================ */

#define SCD_NODE DT_NODELABEL(scd41_0)
#if !DT_NODE_HAS_STATUS(SCD_NODE, okay)
#error "SCD41 devicetree node 'scd41_0' ontbreekt of is niet OK"
#endif

static const struct device *const scd = DEVICE_DT_GET(SCD_NODE);

/* First stable measurement ≈ 5s after enabling periodic mode */
#define SCD41_WARMUP_MS 5000

/* ============================================================
 * HELPERS
 * ============================================================ */

static inline int32_t to_centi(const struct sensor_value *v)
{
    int64_t micro = v->val2;
    int64_t centi = ((int64_t)v->val1 * 100)
                  + (micro >= 0 ? (micro + 5000) / 10000
                                : (micro - 5000) / 10000);
    return (int32_t)centi;
}

/* ============================================================
 * PUBLISH HELPER
 * ============================================================ */

static void publish_field(const char *root,
                          const char *sensor,
                          const char *field,
                          int32_t value)
{
    char topic[64];
    char payload[24];

    if (topic_build(topic, sizeof(topic),
                    root, "OUT", sensor, field) == 0) {

        snprintk(payload, sizeof(payload), "%d", (int)value);
        transport_publish(topic, payload);
    }
}

/* ============================================================
 * MODULE STATE
 * ============================================================ */

static struct {
    bool warmup_done;
    int64_t warmup_deadline;
} scd_ctx;

/* ============================================================
 * PUBLIC API (called by app_core)
 * ============================================================ */

void scd41_sample_and_publish(const char *root)
{
    if (!device_is_ready(scd)) {
#if IS_ENABLED(CONFIG_APP_DEBUG_SENSOR)
        LOG_WRN("SCD41: device not ready (check DT/I2C)");
#endif
        return;
    }

    int64_t now = k_uptime_get();

    /* Start warm-up on first call */
    if (!scd_ctx.warmup_done && scd_ctx.warmup_deadline == 0) {
#if IS_ENABLED(CONFIG_APP_DEBUG_SENSOR)
        LOG_INF("SCD41: starting warm-up (%d ms)", SCD41_WARMUP_MS);
#endif
        scd_ctx.warmup_deadline = now + SCD41_WARMUP_MS;
    }

    /* Still warming up → dummy read */
    if (!scd_ctx.warmup_done && now < scd_ctx.warmup_deadline) {
        struct sensor_value dummy;
        sensor_sample_fetch(scd);
        sensor_channel_get(scd, SENSOR_CHAN_CO2, &dummy);
        return;
    }

    scd_ctx.warmup_done = true;

    /* Normal measurement */
    struct sensor_value co2, t, rh;

    if (sensor_sample_fetch(scd) != 0) {
#if IS_ENABLED(CONFIG_APP_DEBUG_SENSOR)
        LOG_WRN("SCD41: sample_fetch failed");
#endif
        return;
    }

    if (sensor_channel_get(scd, SENSOR_CHAN_CO2, &co2) ||
        sensor_channel_get(scd, SENSOR_CHAN_AMBIENT_TEMP, &t) ||
        sensor_channel_get(scd, SENSOR_CHAN_HUMIDITY, &rh)) {
#if IS_ENABLED(CONFIG_APP_DEBUG_SENSOR)
        LOG_WRN("SCD41: channel_get failed");
#endif
        return;
    }

    uint32_t co2_ppm  = (co2.val1 > 0) ? (uint32_t)co2.val1 : 0u;
    int32_t t_centi  = to_centi(&t);
    int32_t rh_centi = to_centi(&rh);

#if IS_ENABLED(CONFIG_APP_DEBUG_SENSOR)
    LOG_INF("SCD41 OK: CO2=%u ppm T=%d c°C RH=%d c%%",
            co2_ppm, t_centi, rh_centi);
#endif

    publish_field(root, "SCD41-1", "CO2",  co2_ppm);
    k_msleep(15);
    publish_field(root, "SCD41-1", "TEMP", t_centi);
    k_msleep(15);
    publish_field(root, "SCD41-1", "RH",   rh_centi);
}