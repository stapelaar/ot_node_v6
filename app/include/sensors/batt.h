#pragma once

/* Leest de batterijspanning via ADC P1.14 (AIN7).
 * P1.15 (VBAT_EN) wordt kort ingeschakeld tijdens meting.
 * Publiceert NDxx/OUT/BATT/MV en NDxx/OUT/BATT/PCT. */
void batt_sample_and_publish(const char *root);
