#pragma once
#include <stdint.h>

/* Initialiseert ZMS en laadt opgeslagen waarden.
 * Wordt automatisch aangeroepen via SYS_INIT. */
int app_settings_init(void);

/* Meetinterval in seconden. Geeft Kconfig default terug als geen
 * persistente waarde opgeslagen is. */
uint32_t app_settings_get_interval_s(void);

/* Sla nieuw meetinterval op in ZMS. Overleeft reboot. */
int app_settings_set_interval_s(uint32_t seconds);
