#pragma once

// Wi-Fi, device layer (docs/wifi.md): the NVS storage under WifiConfig, the
// ESP-IDF driver calls, the event glue and the Wi-Fi task (core 0). The rules
// are in wifi_credentials.cpp and wifi_state.cpp (host tested); this file is
// checked on the hardware only.

#include <stdint.h>
#include "esp_err.h"
#include "wifi_credentials.h"
#include "wifi_state.h"
#include "device_name.h"

// The credentials (shared with Protocol). Empty until wifi_init().
WifiConfig *wifi_config(void);

// Load the stored credentials from NVS (nvs_flash_init() first). Starts no
// radio and no task, so it can't affect the display or USB.
void wifi_init(void);

// Start the Wi-Fi task on core 0. If credentials are stored it brings Wi-Fi
// up and connects; with none, Wi-Fi isn't started at all (no RAM, no radio).
// Call last in the boot, after the display, touch and USB.
esp_err_t wifi_start(void);

// The state changed (or the address): called from the Wi-Fi task, so it must
// not block. `ip` is a.b.c.d while connected, else zeros.
typedef void (*wifi_state_cb_t)(WifiState state, const uint8_t ip[4]);
void wifi_set_state_cb(wifi_state_cb_t cb);

// WIFI_STATUS: the state now, with the signal and MAC from the driver
void wifi_get_status(WifiStatus *out);

// The clock settings changed (clock.cpp; any task, doesn't block): the Wi-Fi
// task restarts SNTP if the NTP server differs and Wi-Fi is connected. SNTP
// itself starts on each GotIp (server from clock_ntp_server()) and stops with
// the driver; a sync calls clock_ntp_synced().
void wifi_ntp_server_changed(void);

// Has an IP (cheap, any task): what the network ops check before sending
bool wifi_is_connected(void);

// SNTP is running (for the diagnostics; any task)
bool wifi_sntp_on(void);

// The device name (mDNS/DHCP hostname; shared with Protocol). Loaded by wifi_init().
DeviceName *device_name(void);
