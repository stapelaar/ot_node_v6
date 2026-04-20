#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(coap_client, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/coap.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <errno.h>

#include "coap_client.h"

#define COAP_BUF_SIZE  192
#define PAYLOAD_SIZE    96

static char s_target[INET6_ADDRSTRLEN] = "fd84:3760:18a6:cec7:b222:7aff:fee7:5602";
static uint16_t s_port = 5684;

static int s_sock = -1;

/* ── Get the Thread network interface index ─────────────────────────────── */

static int get_ot_iface_index(void)
{
    /* OpenThread L2 interface is always the first (and usually only)
       IEEE 802.15.4 interface. Walk all interfaces and find it. */
    struct net_if *iface = net_if_get_default();
    if (iface) {
        return net_if_get_by_iface(iface);
    }
    return 1; /* fallback: interface 1 */
}

/* ── Socket open ────────────────────────────────────────────────────────── */

static int coap_open(void)
{
    if (s_sock >= 0) {
        return 0;
    }

    s_sock = zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        LOG_ERR("socket() failed: %d", errno);
        return -errno;
    }

    LOG_INF("CoAP UDP socket opened");
    return 0;
}

int coap_client_init(const char *target, uint16_t port)
{
    if (target && *target) strncpy(s_target, target, sizeof(s_target) - 1);
    if (port) s_port = port;

    int rc = coap_open();
    if (rc == 0) {
        LOG_INF("CoAP ready (Zephyr socket) -> target %s:%d", s_target, s_port);
    }
    return rc;
}

bool coap_client_ready(void) { return s_sock >= 0; }

/* ── CoAP POST ──────────────────────────────────────────────────────────── */

int coap_client_post_mqttlike(const char *uri_path, const char *topic, const char *value,
                              uint32_t ack_timeout_ms, int retries)
{
    ARG_UNUSED(ack_timeout_ms);
    ARG_UNUSED(retries);

    if (s_sock < 0) {
        if (coap_client_init(NULL, 0) != 0) return -EIO;
    }

    /* Build payload and CoAP packet */
    char payload[PAYLOAD_SIZE];
    snprintk(payload, sizeof(payload), "topic=%s\nvalue=%s",
             topic ? topic : "unknown", value ? value : "null");

    uint8_t buf[COAP_BUF_SIZE];
    struct coap_packet req;
    uint8_t token[2];
    uint16_t msg_id;

    sys_rand_get(token, sizeof(token));
    msg_id = (uint16_t)sys_rand32_get();

    int r = coap_packet_init(&req, buf, sizeof(buf), 1,
                             COAP_TYPE_NON_CON,
                             sizeof(token), token, COAP_METHOD_POST, msg_id);
    if (r < 0) {
        LOG_ERR("coap_packet_init failed: %d", r);
        return r;
    }

    coap_packet_append_option(&req, COAP_OPTION_URI_PATH, uri_path, strlen(uri_path));

    uint16_t cf = COAP_CONTENT_FORMAT_TEXT_PLAIN;
    coap_packet_append_option(&req, COAP_OPTION_CONTENT_FORMAT,
                              (uint8_t *)&cf, sizeof(cf));

    coap_packet_append_payload_marker(&req);
    coap_packet_append_payload(&req, payload, strlen(payload));

    /* Set destination with scope_id pointing to the Thread interface.
       Without this, Zephyr cannot route to addresses on foreign prefixes. */
    struct sockaddr_in6 dst = {
        .sin6_family   = AF_INET6,
        .sin6_port     = htons(s_port),
        .sin6_scope_id = (uint32_t)get_ot_iface_index(),
    };

    if (zsock_inet_pton(AF_INET6, s_target, &dst.sin6_addr) != 1) {
        LOG_ERR("Invalid target address: %s", s_target);
        return -EINVAL;
    }

    ssize_t sent = zsock_sendto(s_sock, req.data, req.offset, 0,
                                (struct sockaddr *)&dst, sizeof(dst));
    if (sent > 0) {
        LOG_INF("CoAP POST OK -> %s (%d bytes)", uri_path, (int)sent);
        return 0;
    }

    int err = errno;
    LOG_ERR("sendto() failed: %d", err);

    /* Close socket so next call reopens it cleanly */
    zsock_close(s_sock);
    s_sock = -1;
    return -err;
}
