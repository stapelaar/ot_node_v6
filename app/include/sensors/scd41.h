#pragma once
#include <stdint.h>

void scd41_sample_and_publish(const char *root);

/* Forced Recalibration (FRC) — stopt periodieke meting, kalibreert op
 * opgegeven CO2 referentie (ppm), herstart meting.
 * Typische referentiewaarden: 400 ppm (buitenlucht), 1000 ppm (gekalibreerd gas).
 * Geeft 0 bij succes, negatief bij fout. */
int scd41_forced_recalibration(uint16_t reference_ppm);
