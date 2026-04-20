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

static bool transport_initialized;

int transport_init(void)
{
    LOG_INF("transport_init()");

    int rc = coap_client_init(NULL, 0);
    if (rc == 0) {
        transport_initialized = true;
        LOG_INF("transport ready");
    } else {
        LOG_WRN("transport init failed rc=%d", rc);
    }

    return rc;
}

bool transport_is_connected(void)
{
    return coap_client_ready();
}

int transport_publish(const char *topic, const char *payload)
{
    if (!topic || !payload) {
        return -EINVAL;
    }

    if (!transport_initialized) {
        int rc = transport_init();
        if (rc != 0) {
            return -EAGAIN;
        }
    }

    return coap_client_post_mqttlike(
        "mqtt",
        topic,
        payload,
        CONFIG_APP_COAP_ACK_TIMEOUT_MS,
        CONFIG_APP_COAP_RETRIES
    );
}

int transport_subscribe(const char *topic)
{
    ARG_UNUSED(topic);
    /* CoAP has no subscribe mechanism */
    return 0;
}
