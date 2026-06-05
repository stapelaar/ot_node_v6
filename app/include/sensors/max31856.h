#pragma once

/* MAX31856 SPI thermocouple amplifier driver.
 * Publiceert per sensor:
 *   NDxx/OUT/MAX31856-N/TEMP   (°C * 100, integer)
 *   NDxx/OUT/MAX31856-N/CJ     (cold junction °C * 100)
 *   NDxx/OUT/MAX31856-N/FAULT  (NONE / OC / OV / SHORT / TC_RANGE / CJ_RANGE)
 */

void max31856_sample_and_publish(const char *root);