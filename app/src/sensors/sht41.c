#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(sht41, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>

#include "topic.h"
#include "transport.h"

#define SHT_NODE DT_NODELABEL(sht4x0)
#if !DT_NODE_HAS_STATUS(SHT_NODE, okay)
#error "SHT41 devicetree node 'sht4x0' missing or not OK"
#endif

static const struct device *const sht = DEVICE_DT_GET(SHT_NODE);

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

void sht41_sample_and_publish(const char *root)
{
    if (!device_is_ready(sht)) {
        static bool warned = false;
        if (!warned) {
            LOG_WRN("SHT41: device NOT ready (check DT, I2C, power)");
            warned = true;
        }
        return;
    }

    static bool ready_logged = false;
    if (!ready_logged) {
        LOG_INF("SHT41: device is ready");
        ready_logged = true;
    }

    if (sensor_sample_fetch(sht) != 0) {
        LOG_WRN("SHT41: sample_fetch failed");
        return;
    }

    struct sensor_value t, rh;
    if (sensor_channel_get(sht, SENSOR_CHAN_AMBIENT_TEMP, &t) ||
        sensor_channel_get(sht, SENSOR_CHAN_HUMIDITY, &rh)) {
        LOG_WRN("SHT41: channel_get failed");
        return;
    }

    int32_t t_centi  = to_centi(&t);
    int32_t rh_centi = to_centi(&rh);

    LOG_INF("SHT41: T=%d.%02d C, RH=%d.%02d %%",
            t_centi / 100, abs(t_centi % 100),
            rh_centi / 100, abs(rh_centi % 100));

    publish_field(root, "SHT41-1", "TEMP", t_centi);
    publish_field(root, "SHT41-1", "RH",   rh_centi);
}
