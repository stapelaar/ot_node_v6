#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(dsmr, LOG_LEVEL_INF);

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

#include "dsmr.h"

#define DSMR_UART_NODE  DT_NODELABEL(uart21)
static const struct device *const uart = DEVICE_DT_GET(DSMR_UART_NODE);

#define DSMR_RX_RING_SIZE  2048
static uint8_t dsmr_ring_storage[DSMR_RX_RING_SIZE];
static struct ring_buf dsmr_ring;

static void dsmr_uart_isr(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);

    while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
        uint8_t byte;
        int n = uart_fifo_read(dev, &byte, 1);
        if (n == 1) {
            (void)ring_buf_put(&dsmr_ring, &byte, 1);
        }
    }
}

static bool uart_initialized;

void dsmr_sample_and_publish(const char *root)
{
    ARG_UNUSED(root);

    if (!device_is_ready(uart)) {
        LOG_WRN("DSMR: UART not ready");
        return;
    }

    if (!uart_initialized) {
        ring_buf_init(&dsmr_ring, sizeof(dsmr_ring_storage), dsmr_ring_storage);

        uart_irq_rx_disable(uart);
        uart_irq_callback_set(uart, dsmr_uart_isr);
        uart_irq_rx_enable(uart);

        uart_initialized = true;
        LOG_INF("DSMR-DIAG: UART21 listening on P2.07 @ 115200-8N1");
    }

    /* Drain everything in the ring buffer and show it */
    uint32_t total = 0;
    while (1) {
        uint8_t chunk[64];
        uint32_t n = ring_buf_get(&dsmr_ring, chunk, sizeof(chunk));
        if (n == 0) break;

        total += n;

        /* Print in two forms: printable ASCII + hex dump for non-printable */
        char line[220];
        int w = 0;
        for (uint32_t i = 0; i < n && w < (int)sizeof(line) - 6; i++) {
            uint8_t b = chunk[i];
            if (b >= 0x20 && b <= 0x7E) {
                line[w++] = (char)b;
            } else if (b == '\n') {
                line[w++] = '\\';
                line[w++] = 'n';
            } else if (b == '\r') {
                line[w++] = '\\';
                line[w++] = 'r';
            } else {
                w += snprintk(line + w, sizeof(line) - w, "<%02x>", b);
            }
        }
        line[w] = '\0';
        LOG_INF("RX(%u): %s", (unsigned)n, line);
    }

    if (total == 0) {
        LOG_INF("DSMR-DIAG: 0 bytes in last 60s on UART21 RX");
    } else {
        LOG_INF("DSMR-DIAG: %u bytes received total this cycle", (unsigned)total);
    }
}