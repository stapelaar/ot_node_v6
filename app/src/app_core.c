#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "app_core.h"
#include "app_node.h"

#if IS_ENABLED(CONFIG_APP_USE_SHT41_SENSOR)
#include "sht41.h"
#endif

#if IS_ENABLED(CONFIG_APP_USE_SCD41_SENSOR)
#include "scd41.h"
#endif

#if IS_ENABLED(CONFIG_APP_USE_DS18B20_SENSOR)
#include "ds18b20.h"
#endif

#if IS_ENABLED(CONFIG_APP_USE_MAX31850_SENSOR)
#include "max31850.h"
#endif

LOG_MODULE_REGISTER(app_core, LOG_LEVEL_INF);

static bool thread_attached;

K_THREAD_STACK_DEFINE(sample_stack, 8192);
static struct k_thread sample_thread;

static void sample_thread_entry(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    /* Let Thread settle before first measurement */
    k_msleep(5000);

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

#if IS_ENABLED(CONFIG_APP_USE_DS18B20_SENSOR)
        ds18b20_sample_and_publish(app_node_name());
#endif

#if IS_ENABLED(CONFIG_APP_USE_MAX31850_SENSOR)
        max31850_sample_and_publish(app_node_name());
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