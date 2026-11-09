#pragma once
#include "esp_err.h"
#include "blobpack.h"
#include "wifi_state.h"

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

// The clock was set or its time zone changed: the task re-reads the clock's
// text (clock.h) now rather than at the next minute. A text holding
// `{clock}` (status_bar.h) shows it; the task wakes each minute while one does.
void status_clock_changed(void);

// Status icons (status_icons.h): Wi-Fi state, the op queue busy and net
// requests pending (busy shows while either is), an external failure (shown
// STATUS_ERROR_SHOW_MS), and hosted (Wi-Fi icon only)
void status_set_wifi(WifiState state);
void status_busy(bool busy);
void status_net_busy(bool busy);
void status_error(void);
void status_set_hosted(bool hosted);
