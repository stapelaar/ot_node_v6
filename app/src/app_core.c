#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>

#include "app_core.h"
#include "app_node.h"

#if IS_ENABLED(CONFIG_APP_USE_SHT41_SENSOR)
#include "sht41.h"
#endif

#if IS_ENABLED(CONFIG_APP_USE_SCD41_SENSOR)
#include "scd41.h"
#endif

#if IS_ENABLED(CONFIG_APP_USE_SEN50_SENSOR)
#include "sen50.h"
#endif

#if IS_ENABLED(CONFIG_APP_USE_DS18B20_SENSOR) || IS_ENABLED(CONFIG_APP_USE_MAX31850_SENSOR)
#define APP_HAS_ONEWIRE 1
#include "onewire_inventory.h"
#endif

#if IS_ENABLED(CONFIG_APP_USE_DS18B20_SENSOR)
#include "ds18b20.h"
#endif

#if IS_ENABLED(CONFIG_APP_USE_MAX31850_SENSOR)
#include "max31850.h"
#endif

LOG_MODULE_REGISTER(app_core, LOG_LEVEL_INF);

static bool thread_attached;

#if APP_HAS_ONEWIRE
static const struct device *const w1_bus = DEVICE_DT_GET(DT_ALIAS(onewire0));
static struct ow_inventory ow_inv;
static bool ow_scanned;
#endif

K_THREAD_STACK_DEFINE(sample_stack, 8192);
static struct k_thread sample_thread;

static void sample_thread_entry(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    /* Let Thread settle before first measurement */
    k_msleep(5000);

#if APP_HAS_ONEWIRE
    /* One-time 1-Wire bus scan - enables every configured family */
    if (device_is_ready(w1_bus)) {
        int rc = ow_inventory_scan(&ow_inv, w1_bus,
                                   IS_ENABLED(CONFIG_APP_USE_DS18B20_SENSOR),
                                   IS_ENABLED(CONFIG_APP_USE_MAX31850_SENSOR));
        ow_scanned = (rc == 0);
    } else {
        LOG_WRN("1-Wire bus not ready, skipping scan");
    }
#endif

    int counter = 0;

    while (1) {
        counter++;
        LOG_INF("Cycle %d - start", counter);

#if IS_ENABLED(CONFIG_APP_USE_SHT41_SENSOR)
        sht41_sample_and_publish(app_node_name());
#endif

#if IS_ENABLED(CONFIG_APP_USE_SCD41_SENSOR)
        scd41_sample_and_publish(app_node_name());
#endif

#if IS_ENABLED(CONFIG_APP_USE_SEN50_SENSOR)
        sen50_sample_and_publish(app_node_name());
#endif

#if IS_ENABLED(CONFIG_APP_USE_DS18B20_SENSOR)
        if (ow_scanned) {
            ds18b20_sample_and_publish(app_node_name(), &ow_inv);
        }
#endif

#if IS_ENABLED(CONFIG_APP_USE_MAX31850_SENSOR)
        if (ow_scanned) {
            max31850_sample_and_publish(app_node_name(), &ow_inv);
        }
#endif

        LOG_INF("Cycle %d - done", counter);

        k_msleep(CONFIG_APP_MEASUREMENT_PERIOD_S * 1000);
    }
}

void app_core_on_thread_state_change(bool attached, const char *root)
{
    if (attached && !thread_attached) {
        thread_attached = true;
        LOG_INF("Thread attached - starting sample loop");

        k_thread_create(&sample_thread, sample_stack,
                        K_THREAD_STACK_SIZEOF(sample_stack),
                        sample_thread_entry,
                        NULL, NULL, NULL,
                        K_PRIO_PREEMPT(5), 0, K_NO_WAIT);

        k_thread_name_set(&sample_thread, "sample_loop");
    }
}

static int app_core_init(void)
{
    LOG_INF("app_core_init()");
    thread_attached = false;
    return 0;
}

SYS_INIT(app_core_init, APPLICATION, 50);