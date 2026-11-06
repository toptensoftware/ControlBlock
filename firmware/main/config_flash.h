#pragma once

// Device side of the config store: the `config` flash partition
// (firmware/partitions.csv) as a ConfigFlash. Spec: docs/standalone.md.

#include "config_store.h"

#define CONFIG_PARTITION_NAME     "config"
#define CONFIG_PARTITION_SUBTYPE  0x40

// Fills in capacity, erase, write, read and ctx; the caller supplies `busy`.
// Without the partition (flashed with an old partition table) the capacity is
// 0, so the store refuses every upload.
void config_flash_open(ConfigFlash *flash);
