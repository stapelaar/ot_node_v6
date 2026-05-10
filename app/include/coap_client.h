#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

int  coap_client_init(const char *server_addr, uint16_t port);
int  coap_client_set_server(const char *server_addr, uint16_t port);
int  coap_client_get_server(char *addr, size_t len, uint16_t *port);
bool coap_client_ready(void);

int coap_client_post(const char *uri_path,
                     const char *payload,
                     uint32_t ack_timeout_ms,
                     int retries);

int coap_client_post_mqttlike(const char *uri_path,
                              const char *topic,
                              const char *value,
                              uint32_t ack_timeout_ms,
                              int retries);

/* GET een waarde van de broker via topic als URI query parameter.
 * Vult payload_out met de ontvangen waarde (null-terminated).
 * Geeft 0 terug als er een payload is, 1 als leeg/geen commando, negatief bij fout. */
int coap_client_get(const char *topic,
                    char *payload_out,
                    size_t payload_out_len);
