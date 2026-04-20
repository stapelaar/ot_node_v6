#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(thread_mgr, LOG_LEVEL_INF);

#include <zephyr/net/openthread.h>
#include <openthread/thread.h>
#include <openthread/instance.h>
#include <openthread/dataset.h>
#include <openthread/ip6.h>

#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>

#include "thread_mgr.h"
#include "app_core.h"
#include "app_node.h"
#include "transport.h"

/* ============================================================
 * INTERNAL STATE
 * ============================================================ */

static volatile enum thread_mgr_role current_role = THREAD_MGR_DETACHED;

void app_core_on_thread_state_change(bool attached, const char *root);

/* ============================================================
 * OPENTHREAD STATE CALLBACK
 * ============================================================ */

static void ot_state_changed(otChangedFlags flags, void *ctx)
{
    ARG_UNUSED(ctx);

    otInstance *inst = openthread_get_default_instance();
    if (!inst) {
        current_role = THREAD_MGR_DETACHED;
        return;
    }

    if (flags & OT_CHANGED_THREAD_ROLE) {
        otDeviceRole r = otThreadGetDeviceRole(inst);
        bool attached = false;

        switch (r) {
        case OT_DEVICE_ROLE_CHILD:
            current_role = THREAD_MGR_CHILD;
            attached = true;
            LOG_INF("Thread role = CHILD");
            break;

        case OT_DEVICE_ROLE_ROUTER:
            current_role = THREAD_MGR_ROUTER;
            attached = true;
            LOG_INF("Thread role = ROUTER");
            break;

        case OT_DEVICE_ROLE_LEADER:
            current_role = THREAD_MGR_LEADER;
            attached = true;
            LOG_INF("Thread role = LEADER");
            break;

        default:
            current_role = THREAD_MGR_DETACHED;
            attached = false;
            LOG_WRN("Thread role = DETACHED");
            break;
        }

        /* Notify app_core exactly once per role change */
        app_core_on_thread_state_change(attached, app_node_name());
    }
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

int thread_mgr_init(void)
{
    LOG_INF("thread_mgr_init()");

    otInstance *inst = openthread_get_default_instance();
    if (!inst) {
        LOG_WRN("No OpenThread instance available");
        current_role = THREAD_MGR_DETACHED;
        return 0;
    }

    /* Register OpenThread state callback. */
    otSetStateChangedCallback(inst, ot_state_changed, NULL);

    /* OPENTHREAD_MANUAL_START=y is forced on when the OT shell is enabled.
       The L2 layer will NOT auto-start Thread in this case, so we do it
       ourselves if a dataset is already commissioned (normal boot after
       first-time setup). The shell still works for manual commissioning
       on a fresh/erased device. */
    if (otDatasetIsCommissioned(inst)) {
        LOG_INF("Active dataset present, auto-starting Thread");
        otIp6SetEnabled(inst, true);
        otThreadSetEnabled(inst, true);
    }

    /* Snapshot initial role — L2 may have already started Thread */
    otDeviceRole r = otThreadGetDeviceRole(inst);
    current_role =
        (r == OT_DEVICE_ROLE_CHILD)  ? THREAD_MGR_CHILD  :
        (r == OT_DEVICE_ROLE_ROUTER) ? THREAD_MGR_ROUTER :
        (r == OT_DEVICE_ROLE_LEADER) ? THREAD_MGR_LEADER :
        THREAD_MGR_DETACHED;

    LOG_INF("Initial Thread role = %d", current_role);

    /* If already attached at init time, notify app_core */
    if (thread_mgr_is_attached()) {
        app_core_on_thread_state_change(true, app_node_name());
    }

    return 0;
}

void thread_mgr_poll(void)
{
    /* No-op */
}

bool thread_mgr_is_attached(void)
{
    return (current_role == THREAD_MGR_CHILD  ||
            current_role == THREAD_MGR_ROUTER ||
            current_role == THREAD_MGR_LEADER);
}

enum thread_mgr_role thread_mgr_get_role(void)
{
    return current_role;
}

SYS_INIT(thread_mgr_init, APPLICATION, 90);
