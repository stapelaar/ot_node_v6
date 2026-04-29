#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(watermeter, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>

#include "watermeter.h"
#include "topic.h"
#include "transport.h"

/* GPIO from devicetree alias */
#define WATER_GPIO_NODE  DT_ALIAS(watermeter_pulse)
#if !DT_NODE_EXISTS(WATER_GPIO_NODE)
#error "Missing devicetree alias 'watermeter_pulse'"
#endif

static const struct gpio_dt_spec pulse_gpio = GPIO_DT_SPEC_GET(WATER_GPIO_NODE, gpios);

static struct gpio_callback pulse_cb;
static atomic_t pulse_count;            /* total pulses since boot */
static int64_t last_edge_time_ms;       /* for software debounce */

/* Debounce window in milliseconds. The LJ12A3 inductive sensor is fairly
 * clean, but the optocoupler edge has finite rise/fall and may chatter
 * slightly when water flow is borderline. 50 ms is safe up to ~20 Hz which
 * is far above any realistic flow rate (max ~1 Hz at 60 L/min). */
#define DEBOUNCE_MS  50

static void pulse_isr(const struct device *port,
                      struct gpio_callback *cb,
                      gpio_port_pins_t pins)
{
    ARG_UNUSED(port);
    ARG_UNUSED(cb);
    ARG_UNUSED(pins);

    int64_t now = k_uptime_get();
    if (now - last_edge_time_ms < DEBOUNCE_MS) {
        return;
    }
    last_edge_time_ms = now;

    atomic_inc(&pulse_count);
}

static int watermeter_init_gpio(void)
{
    if (!gpio_is_ready_dt(&pulse_gpio)) {
        LOG_ERR("Watermeter GPIO not ready");
        return -ENODEV;
    }

    /* Active LOW: optocoupler conducts (sensor detects metal) -> input pulled LOW.
     * Configure with internal pull-up for safety even though we have an
     * external 20kΩ pullup; doesn't hurt. */
    int rc = gpio_pin_configure_dt(&pulse_gpio, GPIO_INPUT | GPIO_PULL_UP);
    if (rc < 0) {
        LOG_ERR("gpio_pin_configure_dt failed rc=%d", rc);
        return rc;
    }

    /* Trigger on falling edge (HIGH -> LOW = water pulse) */
    rc = gpio_pin_interrupt_configure_dt(&pulse_gpio, GPIO_INT_EDGE_TO_INACTIVE);
    if (rc < 0) {
        LOG_ERR("gpio_pin_interrupt_configure_dt failed rc=%d", rc);
        return rc;
    }

    gpio_init_callback(&pulse_cb, pulse_isr, BIT(pulse_gpio.pin));
    rc = gpio_add_callback(pulse_gpio.port, &pulse_cb);
    if (rc < 0) {
        LOG_ERR("gpio_add_callback failed rc=%d", rc);
        return rc;
    }

    LOG_INF("Watermeter: GPIO pulse counter armed on D3 (P1.07), active LOW");
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
        LOG_INF("PUB %s = %s", topic, payload);
    }
}

static bool initialized;
static atomic_t last_count;

void watermeter_sample_and_publish(const char *root)
{
    if (!initialized) {
        if (watermeter_init_gpio() != 0) {
            return;
        }
        initialized = true;
        atomic_set(&pulse_count, 0);
        atomic_set(&last_count, 0);
    }

    /* Snapshot current cumulative count, compute delta vs last cycle */
    atomic_t now_count = atomic_get(&pulse_count);
    atomic_t prev = atomic_get(&last_count);
    int32_t delta = (int32_t)(now_count - prev);
    atomic_set(&last_count, now_count);

    LOG_INF("Watermeter: %d pulses this cycle (total since boot=%d)",
            delta, (int)now_count);

    publish_field(root, "WATER-1", "LITERS_DELTA", delta);
}