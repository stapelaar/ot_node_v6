#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(node_cmd, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <string.h>
#include <stdlib.h>

#include "node_cmd.h"
#include "coap_client.h"
#include "transport.h"
#include "topic.h"
#include "app_settings.h"

#if IS_ENABLED(CONFIG_APP_USE_SCD41_SENSOR)
#include "scd41.h"
#endif

/* Pending command buffer — bewaar commando als het nog niet uitgevoerd kan worden */
static char s_pending_cmd[64] = {0};

/* ── Publish ACK terug naar broker ───────────────────────────────────────── */
static void ack(const char *root, const char *result)
{
    char topic[64];
    if (topic_build(topic, sizeof(topic), root, "OUT", "CMD", "ACK") != 0) {
        return;
    }
    int rc = transport_publish(topic, result);
    if (rc < 0) {
        LOG_WRN("CMD ACK publish failed rc=%d", rc);
    } else {
        LOG_INF("CMD ACK: %s", result);
    }
}

/* ── Command dispatcher — geeft 0 als uitgevoerd, 1 als uitgesteld ────────── */
static int dispatch(const char *root, char *cmd)
{
    /* Trim trailing whitespace/newline */
    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len-1] == '\r' || cmd[len-1] == '\n' ||
                       cmd[len-1] == ' ')) {
        cmd[--len] = '\0';
    }

    if (len == 0) {
        return 0;
    }

    LOG_INF("CMD executing: '%s'", cmd);

    /* ── RESET ────────────────────────────────────────────────────────────── */
    if (strcmp(cmd, "RESET") == 0) {
        ack(root, "OK:RESET");
        k_msleep(200);
        sys_reboot(SYS_REBOOT_COLD);
        return 0;
    }

    /* ── INTERVAL:<seconden> ──────────────────────────────────────────────── */
    if (strncmp(cmd, "INTERVAL:", 9) == 0) {
        uint32_t secs = (uint32_t)strtoul(cmd + 9, NULL, 10);
        int rc = app_settings_set_interval_s(secs);
        if (rc == 0) {
            char reply[32];
            snprintk(reply, sizeof(reply), "OK:INTERVAL:%u", secs);
            ack(root, reply);
        } else {
            ack(root, "ERR:INTERVAL:INVALID");
        }
        return 0;
    }

/* ── CALIBRATE / CALIBRATE:<ppm> (SCD41 only) ───────────────────────── */
#if IS_ENABLED(CONFIG_APP_USE_SCD41_SENSOR)
    if (strcmp(cmd, "CALIBRATE") == 0 || strncmp(cmd, "CALIBRATE:", 10) == 0) {
        uint16_t ppm = 420;  /* default: buitenlucht */
        if (strncmp(cmd, "CALIBRATE:", 10) == 0) {
            ppm = (uint16_t)strtoul(cmd + 10, NULL, 10);
            if (ppm < 400 || ppm > 2000) {
                ack(root, "ERR:CALIBRATE:PPM_RANGE");
                return 0;
            }
        }
        int rc = scd41_forced_recalibration(ppm);
        if (rc == 0) {
            char reply[32];
            snprintk(reply, sizeof(reply), "OK:CALIBRATE:%u", ppm);
            ack(root, reply);
        } else if (rc == -EAGAIN) {
            /* SCD41 nog in warmup — bewaar voor volgende cyclus */
            LOG_INF("CMD deferred (SCD41 warmup): '%s'", cmd);
            strncpy(s_pending_cmd, cmd, sizeof(s_pending_cmd) - 1);
            s_pending_cmd[sizeof(s_pending_cmd) - 1] = '\0';
            return 1;
        } else {
            char reply[32];
            snprintk(reply, sizeof(reply), "ERR:CALIBRATE:%d", rc);
            ack(root, reply);
        }
        return 0;
    }
#endif

    /* ── Onbekend commando ────────────────────────────────────────────────── */
    char reply[96];
    snprintk(reply, sizeof(reply), "ERR:UNKNOWN:%s", cmd);
    ack(root, reply);
    return 0;
}

/* ── Publieke API ────────────────────────────────────────────────────────── */
void node_cmd_poll(const char *root)
{
    /* Eerst: uitgesteld commando uit vorige cyclus proberen */
    if (s_pending_cmd[0] != '\0') {
        LOG_INF("CMD retry pending: '%s'", s_pending_cmd);
        int rc = dispatch(root, s_pending_cmd);
        if (rc == 0) {
            s_pending_cmd[0] = '\0';
        }
        return;
    }

    /* Normaal: poll bridge voor nieuw commando */
    char topic[64];
    snprintk(topic, sizeof(topic), "%s/IN/CMD", root);

    char payload[64] = {0};
    LOG_DBG("CMD poll: GET %s", topic);   // ← tijdelijk
    int rc = coap_client_get(topic, payload, sizeof(payload));
	LOG_DBG("CMD poll: rc=%d payload='%s'", rc, payload);  // ← tijdelijk
	
    if (rc < 0) {
        LOG_DBG("CMD poll failed rc=%d", rc);
        return;
    }

    if (rc == 1 || payload[0] == '\0') {
        return;
    }

    dispatch(root, payload);
}