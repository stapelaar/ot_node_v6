#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(shell_utils, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/net/openthread.h>
#include <zephyr/net/socket.h>

#include <openthread/instance.h>
#include <openthread/thread.h>
#include <openthread/link.h>
#include <openthread/ip6.h>
#include <openthread/dataset.h>

#if defined(CONFIG_OPENTHREAD_FTD)
#include <openthread/thread_ftd.h>
#endif

#if OPENTHREAD_CONFIG_CHANNEL_MONITOR_ENABLE
#include <openthread/channel_monitor.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "app_settings.h"
#include "coap_client.h"

#if IS_ENABLED(CONFIG_APP_USE_SCD41_SENSOR)
#include "scd41.h"
#endif

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void ip6_to_str(const otIp6Address *a, char *out, size_t n)
{
    otIp6AddressToString(a, out, (uint16_t)n);
}

static void eui64_to_str(const otExtAddress *e, char *out, size_t n)
{
    int off = 0;
    for (int i = 0; i < 8; i++) {
        off += snprintk(out + off, n - (size_t)off,
                        i ? ":%02x" : "%02x", e->m8[i]);
        if (off >= (int)n) break;
    }
}

/* ── th commands ─────────────────────────────────────────────────────────── */

static int cmd_th_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    otInstance *inst = openthread_get_default_instance();
    if (!inst) { shell_error(sh, "no OT instance"); return -ENODEV; }

    otDeviceRole role = otThreadGetDeviceRole(inst);
    const char *role_str[] = { "disabled", "detached", "child", "router", "leader" };
    shell_print(sh, "role     : %s", role_str[MIN(role, 4)]);

    uint16_t rloc = otThreadGetRloc16(inst);
    shell_print(sh, "rloc16   : 0x%04x", rloc);

    uint8_t ch = otLinkGetChannel(inst);
    shell_print(sh, "channel  : %u", ch);

    uint16_t panid = otLinkGetPanId(inst);
    shell_print(sh, "panid    : 0x%04x", panid);

    otLinkModeConfig m = otThreadGetLinkMode(inst);
    shell_print(sh, "mode     : %s%s%s",
                m.mRxOnWhenIdle ? "r" : "-",
                m.mDeviceType   ? "d" : "-",
                m.mNetworkData  ? "n" : "-");

    uint32_t poll = otLinkGetPollPeriod(inst);
    shell_print(sh, "poll     : %u ms", poll);

    return 0;
}

static int cmd_th_addrs(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    otInstance *inst = openthread_get_default_instance();
    if (!inst) { shell_error(sh, "no OT instance"); return -ENODEV; }

    const otNetifAddress *addr = otIp6GetUnicastAddresses(inst);
    int n = 0;
    while (addr) {
        char ip[64];
        ip6_to_str(&addr->mAddress, ip, sizeof(ip));
        shell_print(sh, "%s", ip);
        addr = addr->mNext;
        n++;
    }
    if (!n) shell_print(sh, "(no unicast addresses)");
    return 0;
}

static int cmd_th_parent(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    otInstance *inst = openthread_get_default_instance();
    if (!inst) { shell_error(sh, "no OT instance"); return -ENODEV; }

    otRouterInfo pinfo;
    memset(&pinfo, 0, sizeof(pinfo));
    otError err = otThreadGetParentInfo(inst, &pinfo);
    if (err != OT_ERROR_NONE) {
        shell_print(sh, "parent: unavailable (err=%d)", err);
        return 0;
    }

    otNeighborInfoIterator it = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    otNeighborInfo ni;
    bool found = false;
    while (otThreadGetNextNeighborInfo(inst, &it, &ni) == OT_ERROR_NONE) {
        if (ni.mRloc16 == pinfo.mRloc16) {
            char eui[3*8];
            eui64_to_str(&ni.mExtAddress, eui, sizeof(eui));
            int rssi = ni.mAverageRssi ? ni.mAverageRssi : ni.mLastRssi;
            shell_print(sh, "rloc16=0x%04x  lqi=%u  rssi=%d dBm  eui=%s",
                        pinfo.mRloc16, ni.mLinkQualityIn, rssi, eui);
            found = true;
            break;
        }
    }
    if (!found) {
        shell_print(sh, "rloc16=0x%04x (no neighbor details)", pinfo.mRloc16);
    }
    return 0;
}

static int cmd_th_neigh(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    otInstance *inst = openthread_get_default_instance();
    if (!inst) { shell_error(sh, "no OT instance"); return -ENODEV; }

    otNeighborInfoIterator it = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    otNeighborInfo ni;
    int count = 0;
    while (otThreadGetNextNeighborInfo(inst, &it, &ni) == OT_ERROR_NONE) {
        char eui[3*8];
        eui64_to_str(&ni.mExtAddress, eui, sizeof(eui));
        shell_print(sh, "rloc16=0x%04x  lqi=%u  rssi=%d dBm  eui=%s",
                    ni.mRloc16, ni.mLinkQualityIn,
                    ni.mAverageRssi ? ni.mAverageRssi : ni.mLastRssi, eui);
        count++;
    }
    if (!count) shell_print(sh, "(no neighbors)");
    return 0;
}

static int cmd_th_poll(const struct shell *sh, size_t argc, char **argv)
{
    otInstance *inst = openthread_get_default_instance();
    if (!inst) { shell_error(sh, "no OT instance"); return -ENODEV; }

    if (argc < 2) {
        shell_print(sh, "poll: %u ms", otLinkGetPollPeriod(inst));
        return 0;
    }

    uint32_t ms = (uint32_t)strtoul(argv[1], NULL, 10);
    otError err = otLinkSetPollPeriod(inst, ms);
    shell_print(sh, "poll set to %u ms rc=%d", ms, (int)err);
    return (int)err;
}

static int cmd_th_mode(const struct shell *sh, size_t argc, char **argv)
{
    otInstance *inst = openthread_get_default_instance();
    if (!inst) { shell_error(sh, "no OT instance"); return -ENODEV; }

    if (argc < 2) {
        otLinkModeConfig m = otThreadGetLinkMode(inst);
        shell_print(sh, "mode: %s%s%s",
                    m.mRxOnWhenIdle ? "r" : "-",
                    m.mDeviceType   ? "d" : "-",
                    m.mNetworkData  ? "n" : "-");
        return 0;
    }

    otLinkModeConfig m = {0};
    if (strcmp(argv[1], "sed") == 0) {
        m.mRxOnWhenIdle = false;
        m.mNetworkData  = true;
    } else if (strcmp(argv[1], "med") == 0) {
        m.mRxOnWhenIdle = true;
        m.mNetworkData  = true;
    } else {
        shell_error(sh, "usage: th mode [sed|med]");
        return -EINVAL;
    }

    otError err = otThreadSetLinkMode(inst, m);
    shell_print(sh, "mode set to '%s' rc=%d", argv[1], (int)err);
    return (int)err;
}

static int cmd_th_join(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    otInstance *inst = openthread_get_default_instance();
    if (!inst) { shell_error(sh, "no OT instance"); return -ENODEV; }

    /* Stop Thread first if running */
    otThreadSetEnabled(inst, false);
    otIp6SetEnabled(inst, false);

    otOperationalDataset ds = {0};

    ds.mPanId = 0x2410;
    ds.mComponents.mIsPanIdPresent = true;

    ds.mChannel = 26;
    ds.mComponents.mIsChannelPresent = true;

    strncpy(ds.mNetworkName.m8, "OT-MANNAH", OT_NETWORK_NAME_MAX_SIZE);
    ds.mComponents.mIsNetworkNamePresent = true;

    const uint8_t key[] = {
        0x6a, 0xc2, 0x56, 0xfa, 0x94, 0x4d, 0xd2, 0x8b,
        0x7f, 0x9a, 0x64, 0x1d, 0x84, 0x49, 0xed, 0xd9
    };
    memcpy(ds.mNetworkKey.m8, key, OT_NETWORK_KEY_SIZE);
    ds.mComponents.mIsNetworkKeyPresent = true;

    otError err = otDatasetSetActive(inst, &ds);
    if (err != OT_ERROR_NONE) {
        shell_error(sh, "dataset set failed: %d", err);
        return -EIO;
    }

    err = otIp6SetEnabled(inst, true);
    if (err != OT_ERROR_NONE) {
        shell_error(sh, "ifconfig up failed: %d", err);
        return -EIO;
    }

    err = otThreadSetEnabled(inst, true);
    if (err != OT_ERROR_NONE) {
        shell_error(sh, "thread start failed: %d", err);
        return -EIO;
    }

    shell_print(sh, "Joining OT-MANNAH (ch26, panid=0x2410)...");
    return 0;
}

static int cmd_th_scan(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    otInstance *inst = openthread_get_default_instance();
    if (!inst) { shell_error(sh, "no OT instance"); return -ENODEV; }

    uint32_t mask = 0;
    for (int ch = 11; ch <= 26; ch++) mask |= (1u << ch);

    otError err = otLinkEnergyScan(inst, mask, 100, NULL, NULL);
    shell_print(sh, "energy scan started rc=%d", (int)err);
    return (int)err;
}

static int cmd_th_mon(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
#if OPENTHREAD_CONFIG_CHANNEL_MONITOR_ENABLE
    otInstance *inst = openthread_get_default_instance();
    if (!inst) { shell_error(sh, "no OT instance"); return -ENODEV; }

    shell_print(sh, "Channel occupancy:");
    for (uint8_t ch = 11; ch <= 26; ch++) {
        uint16_t occ = otChannelMonitorGetChannelOccupancy(inst, ch);
        uint32_t pct = ((uint32_t)occ * 100u + 32767u) / 65535u;
        shell_print(sh, "  ch %2u: ~%u%% busy", ch, pct);
    }
    return 0;
#else
    shell_print(sh, "Channel monitor disabled (CONFIG_OPENTHREAD_CHANNEL_MONITOR=y)");
    return -ENOTSUP;
#endif
}

static int cmd_th_cheat(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    shell_print(sh, "Thread cheatsheet:");
    shell_print(sh, "  th status              — rol, rloc, kanaal, mode, poll");
    shell_print(sh, "  th addrs               — eigen IPv6 adressen");
    shell_print(sh, "  th parent              — parent rloc/lqi/rssi");
    shell_print(sh, "  th neigh               — neighbors");
    shell_print(sh, "  th poll [ms]           — poll periode tonen/zetten");
    shell_print(sh, "  th mode [sed|med]      — link mode tonen/zetten");
    shell_print(sh, "  th join                — join OT-MANNAH (1 commando)");
    shell_print(sh, "  th scan                — energy scan kanalen 11-26");
    shell_print(sh, "  th mon                 — channel monitor occupancy");
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_th,
    SHELL_CMD(status, NULL, "Thread status overzicht",           cmd_th_status),
    SHELL_CMD(addrs,  NULL, "Eigen IPv6 adressen",               cmd_th_addrs),
    SHELL_CMD(parent, NULL, "Parent info",                       cmd_th_parent),
    SHELL_CMD(neigh,  NULL, "Neighbors",                         cmd_th_neigh),
    SHELL_CMD_ARG(poll, NULL, "poll [ms]",                       cmd_th_poll, 1, 1),
    SHELL_CMD_ARG(mode, NULL, "mode [sed|med]",                  cmd_th_mode, 1, 1),
    SHELL_CMD(join,   NULL, "Join OT-MANNAH netwerk",            cmd_th_join),
    SHELL_CMD(scan,   NULL, "Energy scan kanalen 11-26",         cmd_th_scan),
    SHELL_CMD(mon,    NULL, "Channel monitor occupancy",         cmd_th_mon),
    SHELL_CMD(cheat,  NULL, "Cheatsheet",                        cmd_th_cheat),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(th, &sub_th, "Thread tools", NULL);

/* ── node commands ───────────────────────────────────────────────────────── */

static int cmd_node_reset(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);
    shell_print(sh, "Rebooting...");
    k_msleep(100);
    sys_reboot(SYS_REBOOT_COLD);
    return 0;
}

static int cmd_node_interval(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_print(sh, "interval: %u s", app_settings_get_interval_s());
        return 0;
    }

    uint32_t secs = (uint32_t)strtoul(argv[1], NULL, 10);
    int rc = app_settings_set_interval_s(secs);
    if (rc == 0) {
        shell_print(sh, "interval set to %u s (persisted)", secs);
    } else {
        shell_error(sh, "failed: %d", rc);
    }
    return rc;
}

#if IS_ENABLED(CONFIG_APP_USE_SCD41_SENSOR)
static int cmd_node_calibrate(const struct shell *sh, size_t argc, char **argv)
{
    uint16_t ppm = 420;
    if (argc >= 2) {
        ppm = (uint16_t)strtoul(argv[1], NULL, 10);
    }

    shell_print(sh, "SCD41 FRC met referentie %u ppm...", ppm);
    int rc = scd41_forced_recalibration(ppm);
    if (rc == 0) {
        shell_print(sh, "Kalibratie OK");
    } else {
        shell_error(sh, "Kalibratie mislukt: %d", rc);
    }
    return rc;
}
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(sub_node,
    SHELL_CMD(reset,    NULL, "Reboot de node",                          cmd_node_reset),
    SHELL_CMD_ARG(interval, NULL, "interval [s] — toon/stel meetinterval", cmd_node_interval, 1, 1),
#if IS_ENABLED(CONFIG_APP_USE_SCD41_SENSOR)
    SHELL_CMD_ARG(calibrate, NULL, "calibrate [ppm] — SCD41 FRC (default 420)", cmd_node_calibrate, 1, 1),
#endif
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(node, &sub_node, "Node commando's", NULL);

/* ── coap commands ───────────────────────────────────────────────────────── */

static int cmd_coap_status(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc); ARG_UNUSED(argv);

    char addr[INET6_ADDRSTRLEN];
    uint16_t port;
    int rc = coap_client_get_server(addr, sizeof(addr), &port);
    if (rc == 0) {
        shell_print(sh, "broker: [%s]:%u", addr, port);
        shell_print(sh, "ready : %s", coap_client_ready() ? "yes" : "no");
    } else {
        shell_print(sh, "broker: not resolved (rc=%d)", rc);
        shell_print(sh, "ready : %s", coap_client_ready() ? "yes" : "no");
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_coap,
    SHELL_CMD(status, NULL, "Broker adres en status", cmd_coap_status),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(coap, &sub_coap, "CoAP transport status", NULL);

void shell_utils_init(void)
{
    LOG_INF("shell_utils initialized");
}