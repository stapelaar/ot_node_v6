#pragma once
#include <zephyr/kernel.h>

static inline const char *app_node_name(void)
{
    static char name[8];

    if (name[0] == '\0') {
        snprintk(name, sizeof(name), "ND%s", CONFIG_APP_NODE_ID);
    }

    return name;
}