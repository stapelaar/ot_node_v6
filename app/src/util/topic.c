#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(topic, LOG_LEVEL_INF);

#include <zephyr/sys/printk.h>
#include <string.h>
#include "topic.h"

/* ============================================================
 * BUILD ROOT TOPIC (e.g. "ND12")
 * ============================================================ */

int topic_root(char *out, size_t out_len, const char *node_id)
{
    if (!out || out_len == 0 || !node_id || !*node_id) {
        return -EINVAL;
    }

    int n = snprintk(out, out_len, "ND%s", node_id);

    /* snprintk():
       - returns number of chars that *would* have been written
       - if >= out_len → truncated → error
    */
    if (n <= 0 || n >= (int)out_len) {
        return -ENOSPC;
    }

    return 0;
}

/* ============================================================
 * BUILD FULL TOPIC
 * Example: root="ND12", chan="OUT", device="SHT41-1", field="TEMP"
 * Output: "ND12/OUT/SHT41-1/TEMP"
 * ============================================================ */

int topic_build(char *out, size_t out_len,
                const char *root,
                const char *chan,
                const char *device,
                const char *field)
{
    if (!out || out_len == 0 ||
        !root || !chan || !device || !field)
    {
        return -EINVAL;
    }

    int n = snprintk(out, out_len,
                     "%s/%s/%s/%s",
                     root, chan, device, field);

    if (n <= 0 || n >= (int)out_len) {
        return -ENOSPC;
    }

    return 0;
}