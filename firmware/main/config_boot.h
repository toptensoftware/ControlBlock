#pragma once

// Standalone mode at boot: load the stored config (the `config` flash
// partition) and show its Home. Device layer only: the work is in
// config_blob (validate, relocate) and config_apply (Home to the layout),
// both host tested. Spec: docs/standalone.md.
//
// A missing, damaged or invalid config is logged and the device stays on
// its empty Home. Neither function can fail the boot.

#include <stddef.h>
#include <stdint.h>

// Read the blob from flash into PSRAM, check its CRC, validate it and
// relocate it. Call before model_init() (so the image store's capacity
// leaves room for the blob) and before screen_init() (the flash read and
// the checks touch PSRAM heavily, which the display fill must not compete
// with).
void config_boot_load(void);

// Put the loaded config's Home on the layout. Call after screen_init(). Does
// nothing if there is no valid config.
void config_boot_apply(void);

// The loaded blob (NULL if none), for the later parts of standalone mode.
// Its pointers are relocated: valid until restart.
const uint8_t *config_blob(void);
size_t          config_blob_size(void);
