#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(bmp388, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <stdlib.h>

#include "bmp388.h"
#include "topic.h"
#include "transport.h"

/* Grab the first bosch,bmp388 node from devicetree */
#define BMP388_NODE  DT_COMPAT_GET_ANY_STATUS_OKAY(bosch_bmp388)

#if !DT_NODE_EXISTS(BMP388_NODE)
#error "No enabled bosch,bmp388 node in devicetree"
#endif

static const struct device *const bmp388 = DEVICE_DT_GET(BMP388_NODE);

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

void bmp388_sample_and_publish(const char *root)
{
    if (!device_is_ready(bmp388)) {
        static bool warned;
        if (!warned) {
            LOG_WRN("BMP388: device not ready");
            warned = true;
        }
        return;
    }

    int rc = sensor_sample_fetch(bmp388);
    if (rc < 0) {
        LOG_WRN("BMP388: sample_fetch failed rc=%d", rc);
        return;
    }

    struct sensor_value press_kpa;
    struct sensor_value temp_c;

    rc = sensor_channel_get(bmp388, SENSOR_CHAN_PRESS, &press_kpa);
    if (rc < 0) {
        LOG_WRN("BMP388: channel_get(PRESS) failed rc=%d", rc);
        return;
    }

    rc = sensor_channel_get(bmp388, SENSOR_CHAN_AMBIENT_TEMP, &temp_c);
    if (rc < 0) {
        LOG_WRN("BMP388: channel_get(TEMP) failed rc=%d", rc);
        return;
    }

    /* sensor_value: val1 = integer part, val2 = millionths
     * press is in kPa - convert to centi-hPa: 1 kPa = 10 hPa = 1000 centi-hPa
     * temp is in degC - convert to centi-C: val1 * 100 + val2 / 10000
     *
     * Normal atmospheric pressure ~1013 hPa = 101300 centi-hPa, fits easily
     * in int32_t, so no int64_t math needed. */
    int32_t press_chpa = press_kpa.val1 * 1000 + press_kpa.val2 / 1000;
    int32_t temp_centi = temp_c.val1 * 100 + temp_c.val2 / 10000;

    LOG_INF("BMP388: P=%d.%02d hPa, T=%d.%02d C",
            press_chpa / 100, abs(press_chpa % 100),
            temp_centi / 100, abs(temp_centi % 100));

    publish_field(root, "BMP388-1", "PRESSURE", press_chpa);
    publish_field(root, "BMP388-1", "TEMP",     temp_centi);
}