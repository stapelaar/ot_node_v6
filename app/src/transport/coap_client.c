#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(coap_client, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/coap.h>
#include <zephyr/net/openthread.h>
#include <zephyr/random/random.h>
#include <openthread/nat64.h>
#include <openthread/instance.h>
#include <string.h>
#include <errno.h>

#include "coap_client.h"

#define COAP_BUF_SIZE  192
#define PAYLOAD_SIZE    96

/* Broker IPv4 address comes from Kconfig at build time.
 * Default in Kconfig is 192.168.1.5 - override per node if needed. */
#ifndef CONFIG_APP_BROKER_IPV4
#define CONFIG_APP_BROKER_IPV4 "192.168.1.5"
#endif

#ifndef CONFIG_APP_BROKER_PORT
#define CONFIG_APP_BROKER_PORT 5684
#endif

static int s_sock = -1;

/* Cached synthesized IPv6 - re-resolved if a publish fails or socket is reopened. */
static char s_target_str[INET6_ADDRSTRLEN] = {0};

/* ── Get the Thread network interface index ─────────────────────────────── */
static int get_ot_iface_index(void)
{
    struct net_if *iface = net_if_get_default();
    if (iface) {
        return net_if_get_by_iface(iface);
    }
    return 1;
}

/* ── Synthesize IPv6 address for the configured broker IPv4 ──────────────
 * Calls OpenThread's NAT64 helper which uses the prefix advertised by the
 * Border Router. This way an OTBR reboot (with a new prefix) is handled
 * transparently - we just re-resolve. */
static int synthesize_broker_ipv6(char *out, size_t out_sz)
{
    otIp4Address ip4 = {0};

    /* Parse "a.b.c.d" into otIp4Address bytes */
    int a, b, c, d;
    if (sscanf(CONFIG_APP_BROKER_IPV4, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) {
        LOG_ERR("Bad APP_BROKER_IPV4 '%s'", CONFIG_APP_BROKER_IPV4);
        return -EINVAL;
    }
    ip4.mFields.m8[0] = (uint8_t)a;
    ip4.mFields.m8[1] = (uint8_t)b;
    ip4.mFields.m8[2] = (uint8_t)c;
    ip4.mFields.m8[3] = (uint8_t)d;

    otInstance *inst = openthread_get_default_instance();
    if (!inst) {
        LOG_ERR("OpenThread instance not available");
        return -ENODEV;
    }

    otIp6Address ip6 = {0};
    otError err = otNat64SynthesizeIp6Address(inst, &ip4, &ip6);
    if (err != OT_ERROR_NONE) {
        LOG_ERR("otNat64SynthesizeIp6Address failed: %d (NAT64 prefix not yet known?)",
                (int)err);
        return -ENETUNREACH;
    }

    /* Convert to printable form for sockaddr_in6 inet_pton round trip */
    if (zsock_inet_ntop(AF_INET6, &ip6, out, out_sz) == NULL) {
        LOG_ERR("inet_ntop failed");
        return -EINVAL;
    }

    return 0;
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
    /* target/port from caller are ignored - we always use Kconfig + NAT64 */
    ARG_UNUSED(target);
    ARG_UNUSED(port);

    int rc = coap_open();
    if (rc != 0) {
        return rc;
    }

    /* Try to resolve broker IPv6 once at init for an early visibility log */
    rc = synthesize_broker_ipv6(s_target_str, sizeof(s_target_str));
    if (rc == 0) {
        LOG_INF("CoAP ready -> broker IPv4 %s synthesized to [%s]:%d",
                CONFIG_APP_BROKER_IPV4, s_target_str, CONFIG_APP_BROKER_PORT);
    } else {
        LOG_WRN("CoAP socket open, but NAT64 synthesis not ready yet (rc=%d)", rc);
        /* That's OK - we'll try again on first publish */
    }
    return 0;
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

    /* Always re-resolve target on every publish. NAT64 prefix may have
     * changed if the OTBR rebooted; the cost is one cheap function call. */
    int rc = synthesize_broker_ipv6(s_target_str, sizeof(s_target_str));
    if (rc < 0) {
        return rc;
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

    struct sockaddr_in6 dst = {
        .sin6_family   = AF_INET6,
        .sin6_port     = htons(CONFIG_APP_BROKER_PORT),
        .sin6_scope_id = (uint32_t)get_ot_iface_index(),
    };

    if (zsock_inet_pton(AF_INET6, s_target_str, &dst.sin6_addr) != 1) {
        LOG_ERR("Invalid synthesized address: %s", s_target_str);
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