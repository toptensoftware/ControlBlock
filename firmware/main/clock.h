#pragma once

// The device clock, device layer (hardware only): the system time
// (settimeofday; the RTC timer keeps it over esp_restart, not power-off),
// the time zone applied through newlib (setenv/tzset, DST by its POSIX
// rules) and the status bar clock's text. The rules are in clock_text.cpp
// and clock_config.cpp (host tested).
//
// setenv/tzset and localtime_r share newlib's globals, so the TZ change (on
// the protocol task) and the local time (status task) run under one mutex.

#include <stdint.h>
#include <sys/time.h>
#include "clock_config.h"
#include "clock_text.h"

// Load the settings from NVS (namespace cb_clock; nvs_flash_init() first) and
// apply the time zone. Before the status bar is drawn.
void clock_init(void);

// The settings (shared with Protocol): a change applies the time zone at once
// and redraws the clock
ClockConfig *clock_config(void);

// Set the clock (UTC). False (nothing changed) if before 2025
// (clock_time_valid). Any task.
bool clock_set_unix_ms(int64_t unix_ms, ClockSource source);

// SNTP has set the clock (lwIP set the system time itself, then called this
// on the tcpip task): mark the source ntp and redraw. Ignored before 2025.
void clock_ntp_synced(const struct timeval *tv);

// The NTP server (NUL terminated, CLOCK_FIELD_MAX + 1 bytes): a copy kept
// under the clock's mutex, so the Wi-Fi task can read it while the protocol
// task changes the settings. A change tells wifi_ntp_server_changed().
void clock_ntp_server(char *dst);

// The time now (UTC), where it came from, and whether it is known
int64_t clock_now_ms(void);
void    clock_get(int64_t *unix_ms, ClockSource *source, bool *known);

// The clock's text for the status bar: local "h:mm", or "" while the time
// isn't known. `dst` holds CLOCK_TEXT_MAX bytes.
void clock_local_text(char *dst);
