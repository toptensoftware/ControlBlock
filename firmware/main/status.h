#pragma once
#include "esp_err.h"
#include "blobpack.h"

// The status bar on the device: a StatusBar (status_bar.h) with two buffers
// in internal RAM, redrawn by its own task when a property changes and
// swapped in at a frame start. Setting a property to its current value does
// nothing. The setters are safe from any task (not ISRs).

// `ui` is ui.bin (ui_assets.h), checked here; it must stay valid.
esp_err_t status_init(const BLOBPACK *ui);

void status_set_left(const char *text);     // UTF-8, cut to STATUS_BAR_TEXT_MAX - 1 bytes
void status_set_right(const char *text);
void status_set_pill_count(int count);
void status_set_selected_pill(int index);
