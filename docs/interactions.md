# Touch Interactions

How a touch becomes either a **swipe** (page change) or a **key press**, and which key events the host sees. Protocol encoding of the events is in [`protocol.md`](protocol.md) (KEY); swipe feel is in `swipe.cpp`. Built in `touch_router.cpp/.h` (pure, tested) and driven by `swipe.cpp`; the hit test by key rect (2026-11-12) is not yet run on the hardware.

Keys are rects of any size on a 5x3 grid of 144 px cells packed edge to edge, with no gap between them ([`page-compositor.md`](page-compositor.md#layout)); each has an id, unique across the layout, which the events carry.

## Goals

- A **long press** must never start or switch to a swipe.
- A key **press** fires on touch, or shortly after (when the swipe window ends), never later than that.
- A swipe can only start in a very short window after touch-down. After it, the touch belongs to the key until the finger lifts.

## Key actions

Three actions per key, always in this order and only for a touch that reached `PRESS`:

| Action | Fires when | Meaning |
|---|---|---|
| **PRESS** | The swipe window ends with the finger still on the key (or at lift, if lifted sooner) | The key is down. Trigger press actions here, and start hold timing |
| **RELEASE** | The finger lifts while still on the key | Normal end of a press |
| **CANCEL** | The finger drags out of the key after `PRESS` | End of the press **without triggering**: do not run release/click actions |

Every `PRESS` is followed by exactly one `RELEASE` or `CANCEL`. A touch that becomes a swipe sends no key events at all.

## The touch state machine

One touch (the first finger) owns the interaction. States:

```
       touch down inside a key's safe area
IDLE ─────────────────────────────▶ PENDING ──┐
  ▲   (down off any key or in an       │       │ travel > DRAG_SLOP_PX
  ▲    edge zone: SWIPE only)          │       │
  │                                    │       ▼
  │                                    │     SWIPING  (no key events; swipe.cpp owns it)
  │                       window ends  │
  │                       or lift      ▼
  │                                  HELD  ── PRESS sent
  │                                    │  │
  │              lift on key: RELEASE  │  │ finger leaves key bounds: CANCEL
  └────────────────────────────────────┘  ▼
                                        DEAD  (swallows the rest of the touch until lift)
```

**PENDING** (from touch-down until `SWIPE_WINDOW_MS`):
- Travel beyond `DRAG_SLOP_PX` (the existing swipe constant) starts a swipe. The key never fires.
- Lift before the window ends is a quick tap: send `PRESS` then `RELEASE` together at lift (the key was never held visibly long enough to matter).
- Leaving the key's bounds before the window ends, without reaching the slop, means no key and no swipe yet; the touch carries on as swipe-only (it can still become a swipe, and never fires).
- Window end with the finger still on the key and within slop: go to **HELD**, send `PRESS`.

**HELD** (after the window):
- Swiping is **locked out** for the rest of the touch, however far the finger moves. A long press, a wobble or a slow slide never changes page.
- Lift while on the key: `RELEASE`.
- Moving outside the key's bounds: `CANCEL`, then **DEAD**.

**DEAD**: the touch is ignored until lift. Moving back over the key does not press it again, and does not start a swipe.

## Rules and edge cases

- **Only the first finger** is considered. Extra fingers are ignored. If the first finger lifts while another is down, the interaction ends; the second finger does not take over.
- **Off any key and the status bar:** a touch that starts outside every key (the margin round the grid, an area of the grid no key covers, the status bar) can only swipe; it sends no key events.
- **Ambiguous edge zone:** a touch that lands within `EDGE_AMBIGUOUS_PX` (12 px) of any edge of a key's rect is ambiguous: the user may have meant the neighbouring key. It can only swipe and never presses a key, the same as a touch off any key. This applies to touch-down only; once HELD, leaving the key is judged against the key's full rect (plus `KEY_EXIT_SLOP_PX`), not the zone.
- **Seams:** keys abut, so between two neighbouring keys there's a 24 px swipe-only strip (12 px each side). With the usual look (8 px of black round each face) the zone reaches ~4 px into a face. That includes the seam inside a **group** (two keys sharing one picture, like Volume `-`/`+`): it's a 24 px strip across the middle of the picture. A **span** (one key over several cells: 2U, 2x2) is one rect, so it has no inner seam and presses as one key anywhere inside its edge zone. A 0.25U key (36 px) keeps a 12 px active strip. Per-key insets only if the hardware says the zone is wrong.
- **Mid-glide touches:** a touch that lands while a page is still settling (or being flicked) catches the page and can only swipe. It never presses a key.
- **Mid-swipe pages:** while a swipe is in progress a touch never reaches a key; the page under the finger is moving.
- **Hit testing** happens once, at touch-down, on the page most in view: the router asks for that page's keys (`TouchRouterHost::page_keys`: ids and pixel rects, read under the model lock) and picks the key whose rect, shrunk by `EDGE_AMBIGUOUS_PX` on every side, holds the point (`touch_key_at`). It keeps that key's id and rect: the bounds for "leaving the key" are the full rect as it was at touch-down (no lookup per move). Rects never overlap (the device refuses keys that do).
- **Key removed, moved to another page, or page changed while HELD:** send `CANCEL` (checked every frame through `key_active(page, id)`), then **DEAD**. A key the host moves on the same page (same id, new rect) keeps the touch-down rect until the touch ends.
- **Backlight off:** touches never reach the state machine. Each touch-down sends a `WAKE` event instead and the rest of the touch is swallowed (also after the host turns the light back on). Turning the light off mid-touch sends `CANCEL` for a held key. See [`backlight.md`](backlight.md).
- **Host disconnect, event overflow:** unchanged from the protocol. The host treats any key it thinks is held as released.
- **Pressed look:** separate from the events. The key's pressed image is shown from touch-down on an active key (so a tap gives feedback at once), and reverted if the touch turns into a swipe or leaves the key before the window ends. Once `PRESS` is sent it stays until `RELEASE`/`CANCEL`. A swipe therefore flashes the pressed look briefly. Touches that can only swipe (off any key, edge zone, inactive key, busy page) never show it. A key in a group presses alone: only its own part of the picture changes. The look is a `key_look` callback on the router, not part of the KEY events.

## Constants

Firmware constants, to be tuned on the hardware (`TOUCH_*` in `touch_router.h`):

| Constant | Start value | Notes |
|---|---|---|
| `SWIPE_WINDOW_MS` | 150 | Long enough to catch the start of a deliberate swipe at 100 Hz touch polling (about 15 samples), short enough to feel instant. 100 was too short on the hardware: swipes were read as presses |
| `EDGE_AMBIGUOUS_PX` | 12 | Touch-down this close to any edge of a key's rect is swipe-only. 1U cells are 144 px with an 8 px black margin round a 128 px face, so it reaches ~4 px into the face; a seam between keys is a 24 px swipe-only strip |
| `KEY_EXIT_SLOP_PX` | 0 | Extra margin around the key before `CANCEL`. Raise it if cancels feel twitchy |
| `DRAG_SLOP_PX` | 10 | Travel that starts a swipe inside the window |

## Host side

- Run the press action on `PRESS` for instant keys. Run it on `RELEASE` for keys that distinguish click from long press; ignore `CANCEL`.
- Long press and double press are detected host side, from `PRESS`/`RELEASE` times (`time_ms`). Because `PRESS` is delayed by up to the swipe window, hold times should be measured between `PRESS` and `RELEASE`, not from the real touch-down.

## Implementation and testing

- The state machine is a pure, platform-independent module (`touch_router.cpp/.h`), driven by `(id, DOWN|MOVE|UP, x, y, time)` and a host that gives a page's keys (ids and rects), producing `PRESS`/`RELEASE`/`CANCEL` and "swipe may start/must not start" decisions. Unit tested in `firmware/test/TestTouchRouter.cpp` with a fake clock: tap, long press, wobble inside slop, swipe in the window, slide out after the window, slide out and back, touch off any key, touch in the edge zone (just inside and just outside the margin), seams between keys and inside groups, spans, 0.25U keys, empty areas, per-page layouts, extra fingers, key removed while held, and a fuzz over random layouts.
- `swipe.cpp` stops deciding drag-versus-press on its own: it starts a drag only when the router says the touch is a swipe.
- Feel (window length, slop) can only be judged on the hardware.

## Open questions

- Should `PRESS` for an instant tap be sent at lift (above) or should a short window plus quick lift be treated as press-then-release with the device's own timestamps? Sending both at lift keeps ordering simple.
- Per-key opt-in to a zero-latency `PRESS` (no swipe window) for things like push-to-talk, at the cost of that key never starting a swipe. Not built: add only if the standard window proves too slow for a real use.
