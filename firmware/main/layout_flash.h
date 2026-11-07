#pragma once

// Device side of the layout store: the `layout` flash partition
// (firmware/partitions.csv) as a LayoutFlash. Spec: docs/standalone.md.

#include "layout_store.h"

#define LAYOUT_PARTITION_NAME     "layout"
#define LAYOUT_PARTITION_SUBTYPE  0x40

// Fills in capacity, erase, write, read and ctx; the caller supplies `busy`.
// Without the partition (flashed with an old partition table) the capacity is
// 0, so the store refuses every upload.
void layout_flash_open(LayoutFlash *flash);
