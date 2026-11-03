#pragma once

// Put a validated config blob's Home on the key layout. Platform independent
// and tested; the device layer (config_boot.cpp) supplies the blob in memory.
// Spec: docs/standalone.md ("Image IDs").

#include <stdint.h>
#include "config_blob.h"
#include "image_store.h"
#include "key_layout.h"

// Register each distinct Home image with `store` as an external image (ID =
// its offset in the blob, pointer = blob + offset) and set page 0's keys. A
// key with no images is empty (inactive). The layout and store are expected
// to be empty (boot, or after a hosted takeover is released: Clear() the
// layout and Reset() the store first). `blob` must stay put until the store
// is reset.
//
// All or nothing: on a failure (an image the store refuses) the store and
// layout are as they were, and false is returned.
bool ConfigApplyHome(const ConfigSummary &config, const uint8_t *blob, ImageStore *store, KeyLayout *layout);
