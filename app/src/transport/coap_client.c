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

#define COAP_BUF_SIZE   192
#define PAYLOAD_SIZE     96
#define GET_BUF_SIZE    192
#define GET_TIMEOUT_MS 2000

#ifndef CONFIG_APP_BROKER_IPV4
#define CONFIG_APP_BROKER_IPV4 "192.168.1.5"
#endif

#ifndef CONFIG_APP_BROKER_PORT
#define CONFIG_APP_BROKER_PORT 5684
#endif

/* Single UDP socket for both POST and GET.
 * The GET drains stale data before sending to avoid picking up
 * unrelated traffic. Same socket ensures the response takes the
 * same NAT64 path as the POST — critical for mesh routing. */
static int s_sock = -1;

static char s_target_str[INET6_ADDRSTRLEN] = {0};

/* ── Helpers ────────────────────────────────────────────────────────────── */

static int get_ot_iface_index(void)
{
    struct net_if *iface = net_if_get_default();
    return iface ? net_if_get_by_iface(iface) : 1;
}

static int synthesize_broker_ipv6(char *out, size_t out_sz)
{
    otIp4Address ip4 = {0};
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
        LOG_ERR("otNat64SynthesizeIp6Address failed: %d", (int)err);
        return -ENETUNREACH;
    }

    if (zsock_inet_ntop(AF_INET6, &ip6, out, out_sz) == NULL) {
        LOG_ERR("inet_ntop failed");
        return -EINVAL;
    }

    return 0;
}

static int coap_open(void)
{
    if (s_sock >= 0) return 0;

    s_sock = zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        LOG_ERR("socket() failed: %d", errno);
        return -errno;
    }

    LOG_INF("CoAP UDP socket opened");
    return 0;
}

/* ── Public init ────────────────────────────────────────────────────────── */

int coap_client_init(const char *target, uint16_t port)
{
    ARG_UNUSED(target);
    ARG_UNUSED(port);

    int rc = coap_open();
    if (rc != 0) return rc;

    rc = synthesize_broker_ipv6(s_target_str, sizeof(s_target_str));
    if (rc == 0) {
        LOG_INF("CoAP ready -> broker IPv4 %s synthesized to [%s]:%d",
                CONFIG_APP_BROKER_IPV4, s_target_str, CONFIG_APP_BROKER_PORT);
    } else {
        LOG_WRN("CoAP socket open, NAT64 not ready yet (rc=%d)", rc);
    }

    return 0;
}

bool coap_client_ready(void) { return s_sock >= 0; }

int coap_client_get_server(char *addr, size_t len, uint16_t *port)
{
    if (!addr || len == 0 || !port) return -EINVAL;
    if (s_target_str[0] == '\0') return -ENODATA;
    strncpy(addr, s_target_str, len - 1);
    addr[len - 1] = '\0';
    *port = CONFIG_APP_BROKER_PORT;
    return 0;
}

/* ── CoAP POST (NON_CON, fire-and-forget) ───────────────────────────────── */

int coap_client_post_mqttlike(const char *uri_path, const char *topic,
                              const char *value,
                              uint32_t ack_timeout_ms, int retries)
{
    ARG_UNUSED(ack_timeout_ms);
    ARG_UNUSED(retries);

    if (s_sock < 0) {
        if (coap_client_init(NULL, 0) != 0) return -EIO;
    }

    int rc = synthesize_broker_ipv6(s_target_str, sizeof(s_target_str));
    if (rc < 0) return rc;

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
                             sizeof(token), token,
                             COAP_METHOD_POST, msg_id);
    if (r < 0) { LOG_ERR("POST packet_init failed: %d", r); return r; }

    coap_packet_append_option(&req, COAP_OPTION_URI_PATH,
                              uri_path, strlen(uri_path));

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
        LOG_ERR("Invalid address: %s", s_target_str);
        return -EINVAL;
    }

    ssize_t sent = zsock_sendto(s_sock, req.data, req.offset, 0,
                                (struct sockaddr *)&dst, sizeof(dst));
    if (sent > 0) {
        LOG_INF("CoAP POST OK -> %s (%d bytes)", uri_path, (int)sent);
        return 0;
    }

    int err = errno;
    LOG_ERR("POST sendto() failed: %d", err);
    zsock_close(s_sock);
    s_sock = -1;
    return -err;
}

/* ── CoAP GET (CON, same socket as POST) ────────────────────────────────── */

int coap_client_get(const char *topic,
                    char *payload_out,
                    size_t payload_out_len)
{
    if (!topic || !payload_out || payload_out_len == 0) return -EINVAL;

    payload_out[0] = '\0';

    if (s_sock < 0) {
        if (coap_client_init(NULL, 0) != 0) return -EIO;
    }

    int rc = synthesize_broker_ipv6(s_target_str, sizeof(s_target_str));
    if (rc < 0) return rc;

    /* Drain stale data from socket before sending GET.
     * Wait 50ms first so any 2.04 Changed ACK from the last POST
     * has time to arrive before we drain. */
    k_msleep(50);
    {
        struct zsock_pollfd drain = { .fd = s_sock, .events = ZSOCK_POLLIN };
        int drained = 0;
        while (zsock_poll(&drain, 1, 200) > 0) {
            uint8_t tmp[64];
            zsock_recv(s_sock, tmp, sizeof(tmp), ZSOCK_MSG_DONTWAIT);
            drained++;
        }
        if (drained > 0) {
            LOG_DBG("GET: drained %d stale packet(s)", drained);
        }
    }

    uint8_t buf[GET_BUF_SIZE];
    struct coap_packet req;
    uint8_t token[2];
    uint16_t msg_id;

    sys_rand_get(token, sizeof(token));
    msg_id = (uint16_t)sys_rand32_get();

    rc = coap_packet_init(&req, buf, sizeof(buf), 1,
                          COAP_TYPE_CON,
                          sizeof(token), token,
                          COAP_METHOD_GET, msg_id);
    if (rc < 0) { LOG_ERR("GET packet_init failed: %d", rc); return rc; }

    coap_packet_append_option(&req, COAP_OPTION_URI_PATH, "cmd", 3);

    char query[80];
    snprintk(query, sizeof(query), "topic=%s", topic);
    coap_packet_append_option(&req, COAP_OPTION_URI_QUERY,
                              query, strlen(query));

    struct sockaddr_in6 dst = {
        .sin6_family   = AF_INET6,
        .sin6_port     = htons(CONFIG_APP_BROKER_PORT),
        .sin6_scope_id = (uint32_t)get_ot_iface_index(),
    };

    if (zsock_inet_pton(AF_INET6, s_target_str, &dst.sin6_addr) != 1) {
        LOG_ERR("Invalid address: %s", s_target_str);
        return -EINVAL;
    }

    ssize_t sent = zsock_sendto(s_sock, req.data, req.offset, 0,
                                (struct sockaddr *)&dst, sizeof(dst));
    if (sent < 0) {
        int err = errno;
        LOG_ERR("GET sendto() failed: %d", err);
        zsock_close(s_sock);
        s_sock = -1;
        return -err;
    }

    struct zsock_pollfd pfd = { .fd = s_sock, .events = ZSOCK_POLLIN };
    int poll_rc = zsock_poll(&pfd, 1, GET_TIMEOUT_MS);
    if (poll_rc <= 0) {
        LOG_DBG("CoAP GET timeout (no CMD pending)");
        return 1;
    }

    uint8_t resp_buf[GET_BUF_SIZE];
    struct sockaddr_in6 from = {0};
    socklen_t from_len = sizeof(from);

    ssize_t recvd = zsock_recvfrom(s_sock, resp_buf, sizeof(resp_buf), 0,
                                   (struct sockaddr *)&from, &from_len);
    if (recvd < 0) {
        LOG_ERR("GET recvfrom failed: %d", errno);
        return -errno;
    }

    struct coap_packet resp;
    rc = coap_packet_parse(&resp, resp_buf, recvd, NULL, 0);
    if (rc < 0) {
        LOG_ERR("GET response parse failed: %d", rc);
        return rc;
    }

    uint8_t resp_code = coap_header_get_code(&resp);
    
    LOG_DBG("GET resp_code=0x%02x recvd=%d", resp_code, (int)recvd);
    
    if (resp_code != COAP_RESPONSE_CODE_CONTENT) {
        LOG_DBG("CoAP GET: ignoring 0x%02x (not 2.05)", resp_code);
        return 1;
    }

    uint16_t plen;
    const uint8_t *pdata = coap_packet_get_payload(&resp, &plen);
    if (pdata && plen > 0) {
        size_t copy = MIN(plen, payload_out_len - 1);
        memcpy(payload_out, pdata, copy);
        payload_out[copy] = '\0';
        LOG_INF("CoAP GET cmd: '%s'", payload_out);
        return 0;
    }

    return 1;
}