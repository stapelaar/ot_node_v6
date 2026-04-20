#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_antenna, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>

/* RF switch nodes (board DTS xiao_nrf54l15_nrf54l15_cpuapp.dts):
 *   rfsw_pwr  GPIO2.3 ACTIVE_HIGH  — powers the RF switch IC
 *   rfsw_ctl  GPIO2.5 ACTIVE_HIGH  — LOW = internal, HIGH = external
 */
#define RFSW_PWR_NODE  DT_NODELABEL(rfsw_pwr)
#define RFSW_CTL_NODE  DT_NODELABEL(rfsw_ctl)

static const struct gpio_dt_spec rfsw_pwr = {
    .port     = DEVICE_DT_GET(DT_GPIO_CTLR(RFSW_PWR_NODE, enable_gpios)),
    .pin      = DT_GPIO_PIN(RFSW_PWR_NODE, enable_gpios),
    .dt_flags = DT_GPIO_FLAGS(RFSW_PWR_NODE, enable_gpios),
};

static const struct gpio_dt_spec rfsw_ctl = {
    .port     = DEVICE_DT_GET(DT_GPIO_CTLR(RFSW_CTL_NODE, enable_gpios)),
    .pin      = DT_GPIO_PIN(RFSW_CTL_NODE, enable_gpios),
    .dt_flags = DT_GPIO_FLAGS(RFSW_CTL_NODE, enable_gpios),
};

static int antenna_init(void)
{
    int ret;

    if (!gpio_is_ready_dt(&rfsw_pwr) || !gpio_is_ready_dt(&rfsw_ctl)) {
        LOG_ERR("RF switch GPIOs not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&rfsw_pwr, GPIO_OUTPUT_ACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure rfsw_pwr: %d", ret);
        return ret;
    }

    ret = gpio_pin_configure_dt(&rfsw_ctl, GPIO_OUTPUT_INACTIVE);
    if (ret < 0) {
        LOG_ERR("Failed to configure rfsw_ctl: %d", ret);
        return ret;
    }

    ret = gpio_pin_set_dt(&rfsw_ctl, 0);
    if (ret < 0) {
        LOG_ERR("Failed to set rfsw_ctl: %d", ret);
        return ret;
    }

    LOG_INF("External antenna selected");
    return 0;
}

SYS_INIT(antenna_init, APPLICATION, 10);
