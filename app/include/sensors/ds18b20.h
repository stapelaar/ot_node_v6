#pragma once

#include "onewire_inventory.h"

void ds18b20_sample_and_publish(const char *root, const struct ow_inventory *inv);