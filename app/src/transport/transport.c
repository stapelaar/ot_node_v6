#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(transport, LOG_LEVEL_INF);

#include "transport.h"
#include "coap_client.h"

#ifndef CONFIG_APP_COAP_ACK_TIMEOUT_MS
#define CONFIG_APP_COAP_ACK_TIMEOUT_MS 800
#endif

#ifndef CONFIG_APP_COAP_RETRIES
#define CONFIG_APP_COAP_RETRIES 3
#endif

/* ============================================================
 * INITIALISATIE VAN TRANSPORTLAAG
 * ============================================================ */


static bool transport_initialized;


int transport_init(void)
{
    LOG_INF("transport_init() attempt");

    int rc = coap_client_init(NULL, 0);
    if (rc == 0) {
        transport_initialized = true;
        LOG_INF("transport ready");
    } else {
        LOG_WRN("transport init failed rc=%d", rc);
    }

    return rc;
}




/* ============================================================
 * STATUS VAN TRANSPORT
 * ============================================================ */

bool transport_is_connected(void)
{
    /* 'Connected' betekent in CoAP-context dat de socket klaar/open is */
    return coap_client_ready();
}



/* ============================================================
 * PUBLICEREN (TOPIC + PAYLOAD)
 * ============================================================ */


int transport_publish(const char *topic, const char *payload)
{
    if (!topic || !payload) {
        return -EINVAL;
    }

    if (!transport_initialized) {
        int rc = transport_init();
        if (rc != 0)
            return -EAGAIN;
    }

    return coap_client_post_mqttlike(
        "mqtt",
        topic,
        payload,
        CONFIG_APP_COAP_ACK_TIMEOUT_MS,
        CONFIG_APP_COAP_RETRIES
    );
}


/* ============================================================
 * SUBSCRIBE (NO-OP)
 * ============================================================ */

int transport_subscribe(const char *topic)
{
    ARG_UNUSED(topic);
    /* CoAP heeft geen subscribe mechanisme */
    return 0;
}
