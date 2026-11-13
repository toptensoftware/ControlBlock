# atvdaemon in Docker on the NAS

Status: Complete

## Description
Run `urem2/atvdaemon` (pyatv UDP daemon, port 7777) on the NAS (`cool`, 10.1.1.2, x86 Linux) in a Docker
container instead of on the urem Pi, and give the user a way to measure its latency from the ESP32's side of
the network.

## Notes
- Latency: Docker on host networking adds nothing; bridge NAT adds tens of µs. The path still has exactly one
  Wi-Fi hop (now the ESP32's, power save off) and the NAS -> ATV leg is wired, so it should be no slower than
  the Pi. The pyatv round trip to the ATV dominates either way.
- `network_mode: host`: pyatv's `scan(hosts=[...])` is a unicast mDNS query (UDP 5353); host networking avoids
  any NAT trouble with it and needs no port mapping.
- Config moves from constants to environment variables (`config.py`, pure, tested): `ATVD_ID`, `ATVD_HOST`,
  `ATVD_PORT`, `ATVD_BIND`, `ATVD_STORAGE`. Defaults are the old constants, so `start.sh` on a Pi still works.
- Credentials: `/data/pyatv.conf` in the container, a bind mount of `./data` (gitignored). Copy the Pi's
  `~/.pyatv.conf` there, or pair inside the container.
- `atvbench.py` (stdlib only, runs on Windows, the NAS or in the container): sends N commands, prints
  min/median/p95/max round trip. Tests: `python -m unittest` in `urem2/atvdaemon` (stdlib `unittest`, no pyatv
  needed).
- **Deployed on cool 2026-11-13:** running in Docker, `ping` -> `pong` and `key menu` -> `ok` from another
  machine. Gotchas hit: ufw blocked UDP 7777 (`sudo ufw allow 7777/udp`; now in the README); the Pi's credentials
  were copied as `data/.pyatv.conf` (with the dot) so the daemon made an empty `pyatv.conf` and connected over
  unauthenticated AirPlay only (`ping` gave `pong`, keys gave "menu is not supported"; `atvremote scan` showed
  `Credentials: None` everywhere). urem now uses cool's daemon too (urem commit 79bb239, `atv.js` ATV_HOST).
- Hardware-only (can't be automated): building the image on the NAS, pairing, and timing against the real ATV.

## Flagged assumptions
- pyatv pinned to `~=0.16.0` (compatible releases) on `python:3.12-slim`; adjust if the NAS build complains.
- Container runs as UID:GID 1000:1000 (`ATVD_UID`/`ATVD_GID` override, e.g. in `.env`); `data/` must be
  chowned to match. The user's UID on the NAS is 1000 (confirmed).

## Plan
- [x] 1. `config.py` + tests; `atvdaemon.py` reads it.
- [x] 2. `atvbench.py` + tests (9 tests pass; `run()` checked against a local UDP responder; Windows'
      ConnectionResetError on a closed port is reported as "nothing listening").
- [x] 3. Dockerfile, requirements.txt (also used by `start.sh`), compose.yaml, .dockerignore, gitignore `data/`.
      Not built here (no Docker on the dev PC): first build is on the NAS.
- [x] 4. READMEs (atvdaemon, urem2).
- [x] 4a. Moved to its own public repo, github.com/toptensoftware/atv-daemon (fresh initial commit 923ea63,
      `.gitattributes` forces LF, +x on the scripts), now a submodule at `urem2/atvdaemon` (push URL SSH).
- [x] 5. **Timed 2026-11-13** (dev PC on Windows -> cool's container, `atvbench.py`, 20 each): `ping` median
      2.66 ms, p95 3.07, max 3.24, 0 lost; `key menu` (to the ATV) median 5.65 ms, p95 6.87, max 14.42 (first
      press). pyatv's ATV round trip adds ~3 ms; Docker cost not measurable. The PC is on Wi-Fi to the same AP, so
      this is the ESP32's path (one Wi-Fi hop); the ESP32 only sends one way, so a press reaches the ATV in about
      half a round trip plus pyatv's ~3 ms. Latency is a non-issue.
      Setup on the NAS (user): `git clone https://github.com/toptensoftware/atv-daemon.git`, then: build, copy/pair credentials, time bare vs container.
