#pragma once

// The diagnostics HTTP server (docs/diagnostics.md): read-only, LAN trust.
//   GET /            a small page showing both below (auto-refreshing log)
//   GET /status      JSON (diag_status.h)
//   GET /log         text/plain, the whole log ring
//   GET /log?since=N lines after sequence N; X-Log-Next is the next `since`
// Device only (hardware). Started and stopped by the Wi-Fi task with the link.

#define DIAG_HTTP_PORT  80

void diag_http_start(void);     // no-op if running
void diag_http_stop(void);      // no-op if stopped
