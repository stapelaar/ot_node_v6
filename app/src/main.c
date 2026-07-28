#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <app_version.h>

LOG_MODULE_REGISTER(app_main, LOG_LEVEL_INF);

#define APP_NAME "ot_node"

int main(void)
{
    LOG_INF("========================================");
    LOG_INF(" %s v%s+%s", APP_NAME, APP_VERSION_STRING, GIT_HASH);
    LOG_INF(" Built: %s %s", __DATE__, __TIME__);
    LOG_INF(" Starting node ...");
    LOG_INF("========================================");

    /* Everything starts via SYS_INIT and events. */
    return 0;
}