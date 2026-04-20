#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(scd41, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>

#include "topic.h"
#include "transport.h"

#define SCD_NODE DT_NODELABEL(scd41_0)
#if !DT_NODE_HAS_STATUS(SCD_NODE, okay)
#error "SCD41 devicetree node 'scd41_0' missing or not OK"
#endif

static const struct device *const scd = DEVICE_DT_GET(SCD_NODE);

#define SCD41_WARMUP_MS 5000

static inline int32_t to_centi(const struct sensor_value *v)
{
    int64_t micro = v->val2;
    int64_t centi = ((int64_t)v->val1 * 100)
                  + (micro >= 0 ? (micro + 5000) / 10000
                                : (micro - 5000) / 10000);
    return (int32_t)centi;
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

static struct {
    bool    warmup_done;
    int64_t warmup_deadline;
} scd_ctx;

void scd41_sample_and_publish(const char *root)
{
    if (!device_is_ready(scd)) {
        static bool warned = false;
        if (!warned) {
            LOG_WRN("SCD41: device NOT ready (check DT, I2C, power)");
            warned = true;
        }
        return;
    }

    int64_t now = k_uptime_get();

    if (!scd_ctx.warmup_done && scd_ctx.warmup_deadline == 0) {
        LOG_INF("SCD41: starting warm-up (%d ms)", SCD41_WARMUP_MS);
        scd_ctx.warmup_deadline = now + SCD41_WARMUP_MS;
    }

    if (!scd_ctx.warmup_done && now < scd_ctx.warmup_deadline) {
        struct sensor_value dummy;
        sensor_sample_fetch(scd);
        sensor_channel_get(scd, SENSOR_CHAN_CO2, &dummy);
        return;
    }

    scd_ctx.warmup_done = true;

    if (sensor_sample_fetch(scd) != 0) {
        LOG_WRN("SCD41: sample_fetch failed");
        return;
    }

    struct sensor_value co2, t, rh;
    if (sensor_channel_get(scd, SENSOR_CHAN_CO2, &co2)        ||
        sensor_channel_get(scd, SENSOR_CHAN_AMBIENT_TEMP, &t) ||
        sensor_channel_get(scd, SENSOR_CHAN_HUMIDITY, &rh)) {
        LOG_WRN("SCD41: channel_get failed");
        return;
    }

    uint32_t co2_ppm  = (co2.val1 > 0) ? (uint32_t)co2.val1 : 0u;
    int32_t  t_centi  = to_centi(&t);
    int32_t  rh_centi = to_centi(&rh);

    LOG_INF("SCD41: CO2=%u ppm, T=%d.%02d C, RH=%d.%02d %%",
            co2_ppm,
            t_centi / 100,  abs(t_centi % 100),
            rh_centi / 100, abs(rh_centi % 100));

    publish_field(root, "SCD41-1", "CO2",  (int32_t)co2_ppm);
    publish_field(root, "SCD41-1", "TEMP", t_centi);
    publish_field(root, "SCD41-1", "RH",   rh_centi);
}
