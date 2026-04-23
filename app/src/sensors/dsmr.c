#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(dsmr, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <errno.h>

#include "dsmr.h"
#include "topic.h"
#include "transport.h"

/* Use uart21 on the XIAO (TX=P2.08 D6, RX=P2.07 D7).
 * P1 -> inverter transistor -> RX */
#define DSMR_UART_NODE  DT_NODELABEL(uart21)
#if !DT_NODE_HAS_STATUS(DSMR_UART_NODE, okay)
#error "uart21 must be enabled for DSMR"
#endif

static const struct device *const uart = DEVICE_DT_GET(DSMR_UART_NODE);

/* Ring buffer sized for >1 full telegram worth of bytes.
 * DSMR 5.0 telegrams are typically 500-1000 bytes; use 2 KiB for headroom. */
#define DSMR_RX_RING_SIZE  2048
static uint8_t dsmr_ring_storage[DSMR_RX_RING_SIZE];
static struct ring_buf dsmr_ring;

/* A full telegram buffer for parsing. */
#define DSMR_TELEGRAM_MAX  1400
static char telegram[DSMR_TELEGRAM_MAX];

/* ── UART interrupt callback: drain hardware FIFO into ring buffer ─────── */
static void dsmr_uart_isr(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        uint8_t byte;
        int n = uart_fifo_read(dev, &byte, 1);
        if (n == 1) {
            /* Drop byte silently if ring is full - better than blocking in ISR */
            (void)ring_buf_put(&dsmr_ring, &byte, 1);
        }
    }
}

/* ── Telegram collector ──────────────────────────────────────────────────
 * Reads from the ring buffer until a complete telegram is assembled.
 * A DSMR telegram: starts with '/', ends with '!<CRC>\r\n'. */
static int collect_telegram(int timeout_ms)
{
    const int64_t deadline = k_uptime_get() + timeout_ms;
    size_t w = 0;
    bool started = false;
    bool seen_bang = false;

    while (k_uptime_get() < deadline) {
        uint8_t b;
        uint32_t got = ring_buf_get(&dsmr_ring, &b, 1);
        if (got == 0) {
            k_msleep(5);
            continue;
        }

        if (!started) {
            if (b == '/') {
                started = true;
                w = 0;
                telegram[w++] = '/';
            }
            continue;
        }

        if (w < sizeof(telegram) - 1) {
            telegram[w++] = (char)b;
        }

        if (b == '!') {
            seen_bang = true;
            continue;
        }
        if (seen_bang && b == '\n') {
            telegram[w] = '\0';
            return (int)w;
        }
    }
    return -ETIMEDOUT;
}

/* ── OBIS helpers ───────────────────────────────────────────────────────── */

/* Parse the decimal number between '(' after `tag` and the next ')' or non-digit.
 * Returns as exact string (no rounding) plus split into integer/fraction. */
static bool parse_obis_decimal(const char *tag, long *a, long *b, int *b_digits,
                               char *out, size_t out_sz)
{
    const char *p = strstr(telegram, tag);
    if (!p) return false;
    p = strchr(p, '(');
    if (!p) return false;
    p++;

    const char *q = p;
    while (*q && (isdigit((unsigned char)*q) || *q == '.')) q++;

    size_t n = (size_t)(q - p);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, p, n);
    out[n] = '\0';

    const char *dot = strchr(out, '.');
    if (!dot) {
        *a = strtol(out, NULL, 10);
        *b = 0;
        *b_digits = 0;
    } else {
        char ibuf[24], fbuf[24];
        size_t ilen = (size_t)(dot - out);
        size_t flen = strlen(dot + 1);
        if (ilen >= sizeof(ibuf)) ilen = sizeof(ibuf) - 1;
        if (flen >= sizeof(fbuf)) flen = sizeof(fbuf) - 1;
        memcpy(ibuf, out, ilen); ibuf[ilen] = '\0';
        memcpy(fbuf, dot + 1, flen); fbuf[flen] = '\0';
        *a = strtol(ibuf, NULL, 10);
        *b = strtol(fbuf, NULL, 10);
        *b_digits = (int)flen;
    }
    return true;
}

/* Special case: gas reading is formatted as 0-1:24.2.1(TIMESTAMP)(VALUE*m3) */
static bool parse_gas(char *out, size_t out_sz, long *a, long *b)
{
    const char *g = strstr(telegram, "0-1:24.2.1");
    if (!g) return false;

    /* Skip the timestamp block - find the SECOND '(' */
    const char *p = strchr(g, '(');
    if (!p) return false;
    p = strchr(p + 1, '(');
    if (!p) return false;
    p++;

    if (sscanf(p, "%ld.%ld", a, b) != 2) {
        return false;
    }

    snprintk(out, out_sz, "%ld.%03ld", *a, *b);
    return true;
}

/* ── Publish helper ─────────────────────────────────────────────────────── */
static void publish_field(const char *root, const char *sensor,
                          const char *field, const char *value)
{
    char topic[64];

    if (topic_build(topic, sizeof(topic), root, "OUT", sensor, field) != 0) {
        return;
    }

    int rc = transport_publish(topic, value);
    if (rc < 0) {
        LOG_WRN("PUB %s failed rc=%d", topic, rc);
    } else {
        LOG_INF("PUB %s = %s", topic, value);
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

static bool uart_initialized;

void dsmr_sample_and_publish(const char *root)
{
    if (!device_is_ready(uart)) {
        static bool warned;
        if (!warned) {
            LOG_WRN("DSMR: UART not ready");
            warned = true;
        }
        return;
    }

    /* One-time UART setup */
    if (!uart_initialized) {
        ring_buf_init(&dsmr_ring, sizeof(dsmr_ring_storage), dsmr_ring_storage);

        uart_irq_rx_disable(uart);
        uart_irq_callback_set(uart, dsmr_uart_isr);
        uart_irq_rx_enable(uart);

        uart_initialized = true;
        LOG_INF("DSMR: UART21 ready (115200 8N1, RX on P2.07)");

        /* Drain any residual bytes - skip first cycle so we get a clean telegram */
        ring_buf_reset(&dsmr_ring);
        LOG_INF("DSMR: priming, skipping first publish");
        return;
    }

    /* Read one complete telegram. Timeout generous enough for slow meters. */
    int n = collect_telegram(15000);
    if (n < 0) {
        LOG_WRN("DSMR: no telegram received within timeout");
        return;
    }

    LOG_INF("DSMR: telegram received (%d bytes)", n);

    /* Parse fields.
     * Exact strings are preserved for values with fractional parts to avoid
     * rounding loss. These strings get published as-is. */
    char t1[24] = "", t2[24] = "";
    char p_in[24] = "", p_out[24] = "";
    char p_net[24] = "";
    char gas[24] = "";
    long a, b; int d;

    bool has_t1  = parse_obis_decimal("1-0:1.8.1(", &a, &b, &d, t1, sizeof(t1));
    bool has_t2  = parse_obis_decimal("1-0:1.8.2(", &a, &b, &d, t2, sizeof(t2));

    long in_a = 0, in_b = 0, out_a = 0, out_b = 0;
    int  in_d = 0, out_d = 0;
    bool has_pin  = parse_obis_decimal("1-0:1.7.0(", &in_a,  &in_b,  &in_d,  p_in,  sizeof(p_in));
    bool has_pout = parse_obis_decimal("1-0:2.7.0(", &out_a, &out_b, &out_d, p_out, sizeof(p_out));

    bool has_gas = parse_gas(gas, sizeof(gas), &a, &b);

    /* Net power in milli-kW, exact regardless of meter's decimals */
    if (has_pin && has_pout) {
        /* Normalize both fractional parts to exactly 3 digits */
        long in_frac  = in_b;
        long out_frac = out_b;
        for (int i = in_d;  i < 3; i++) in_frac  *= 10;
        for (int i = out_d; i < 3; i++) out_frac *= 10;
        for (int i = 3; i < in_d;  i++) in_frac  /= 10;
        for (int i = 3; i < out_d; i++) out_frac /= 10;

        long in_m  = in_a  * 1000 + in_frac;
        long out_m = out_a * 1000 + out_frac;
        long net_m = in_m - out_m;
        long abs_m = net_m < 0 ? -net_m : net_m;

        snprintk(p_net, sizeof(p_net), "%s%ld.%03ld",
                 net_m < 0 ? "-" : "",
                 abs_m / 1000, abs_m % 1000);
    }

    /* Publish whatever we found */
    if (has_t1)  publish_field(root, "DSMR-1", "T1_KWH",  t1);
    if (has_t2)  publish_field(root, "DSMR-1", "T2_KWH",  t2);
    if (has_pin) publish_field(root, "DSMR-1", "P_IN_KW", p_in);
    if (has_pout) publish_field(root, "DSMR-1", "P_OUT_KW", p_out);
    if (has_pin && has_pout) publish_field(root, "DSMR-1", "P_NET_KW", p_net);
    if (has_gas) publish_field(root, "DSMR-1", "GAS_M3",  gas);

    if (!has_t1 && !has_t2 && !has_pin && !has_pout && !has_gas) {
        LOG_WRN("DSMR: telegram had no recognized OBIS codes");
    }
}