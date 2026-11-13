# HTTP: stale keep-alive connections

Status: Complete

## Description
With real equipment (urem2, 2026-11-13) the first Yamaha request after a quiet spell often fails with
`ESP_ERR_HTTP_FETCH_HEADER` (e.g. `setPower?power=on` when entering TV), so the receiver stays off while the
device mask says it is on. Cause: `net_worker.cpp` keeps one keep-alive `esp_http_client` and reuses its socket
blindly; the receiver closes idle connections (and on power changes), and the next request is written into the
dead socket. The worker drops the client after the failure, so the following request works ("sometimes").

urem (Node) never saw it: Node's keep-alive agent drops pooled sockets the server closes while idle, and urem
calls every device's `turnOn` on every switch (ControlBlock only powers devices whose use changes, by design).

Fix (what Node and browsers do): check an idle connection before reusing it, and retry once on a fresh
connection when a reused one dies before any reply arrives.

## Notes
- `esp_http_client_perform` (IDF v6.0): a read timeout in the header fetch returns `ESP_ERR_HTTP_EAGAIN`, so
  `ESP_ERR_HTTP_FETCH_HEADER` is a read error (reset) and `ESP_ERR_HTTP_CONNECTION_CLOSED` a FIN; a failed
  request write is `ESP_ERR_HTTP_WRITE_DATA`. None of these has delivered response bytes to our event handler.
- `esp_http_client_get_socket()` is -1 once the transport is closed (`base_close` sets `INVALID_SOCKET`);
  `esp_http_client_close()` keeps the handle and resets its state to INIT, so the next perform reconnects.
- The idle check: `recv(fd, buf, 1, MSG_PEEK | MSG_DONTWAIT)`: -1 with EAGAIN/EWOULDBLOCK = alive; 0 = FIN;
  > 0 = stray bytes (not ours to read); other errors = reset. Anything but "would block" closes it first.
- Pure parts in `net_host.h` (`NetIdleSocketAlive`, `NetHttpStage`, `NetHttpRetryFresh`), tested in
  `test/TestNetRequest.cpp`; the esp_http_client glue in `net_worker.cpp` is hardware only.

## Flagged assumptions
- The retry also covers POST: a reused connection that dies before any reply means the server never read the
  request (as Chromium retries on a reused socket). Timeouts are never retried (the server may have acted).

## Plan
- [x] 1. Tests for the pure helpers (idle peek verdict, retry decision) in `TestNetRequest.cpp`; add them to
       `net_host.h`; host tests pass. (789 passed.)
- [x] 2. `net_worker.cpp`: probe the idle socket before reuse (close if not alive), map the error to a stage,
       retry once on a fresh connection; log the retry. Release build 0 errors. (Result mapping unchanged:
       Send and Timeout stages report `timed out`, as `WRITE_DATA` did before.)
- [x] 3. Docs: `docs/standalone.md` updated (done). CLAUDE.md Net ops line added. Verified on the hardware by the
       user (Yamaha powers on reliably). Hardware
       check by the user: TV activity from cold, waits between switches; look for `kept-alive connection closed`
       and `retrying on a new one` lines and no `FETCH_HEADER` failures.
