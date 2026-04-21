#pragma once

#include "onewire_inventory.h"

void max31850_sample_and_publish(const char *root, const struct ow_inventory *inv);