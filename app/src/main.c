#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_main, LOG_LEVEL_INF);

#define APP_NAME     "ot_node"
#define APP_VERSION  "v6.1.0-ncs"
#define APP_BUILD    __DATE__ " " __TIME__

int main(void)
{
    LOG_INF("========================================");
    LOG_INF(" %s %s", APP_NAME, APP_VERSION);
    LOG_INF(" Built: %s", APP_BUILD);
    LOG_INF(" Starting node ...");
    LOG_INF("========================================");

    /* Everything starts via SYS_INIT and events. */
    return 0;
}
