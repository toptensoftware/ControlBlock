# Display drive strength and gradient keys

Status: Investigate

## Description
The panel pins now run at the weakest drive strength (`DISPLAY_PIN_DRIVE_CAP` = `GPIO_DRIVE_CAP_0`, set after
`esp_lcd_new_rgb_panel()`), which fixed the Wi-Fi latency caused by the RGB bus. The user suspects this setting
causes display problems that show up on the gradient key backgrounds of the activity test config
(`tools/bake/examples/activity-test.mjs`, uploaded during the step 9 hardware run of
`wip/complete-activity-device-management.md`). Gradients drive many pixel bits changing at once, which a weak drive
may not handle cleanly at the 20 MHz pixel clock.

Goal: find out whether there is a display problem, whether drive strength is the cause, and pick a setting that
keeps both a clean picture and good Wi-Fi latency.

## Notes
- Reported 2026-11-06 by the user, while testing activity switching.
- **What it looks like (user):** always there, on most Home keys. Looks like typical gradient banding, but some
  of the bands seem to line up with the key's text: suspicious, or a coincidence.
- The user agrees it's probably gradient banding, but hasn't seen it on this device before (possibly because
  gradients haven't been used much), so it's worth checking.
- First thoughts (not checked):
  - Banding may be plain RGB565 quantization: the default faces are dark gradients (`#22262e` to a darker shade),
    which span only a couple of 5-bit red/blue steps, so each band is several rows tall. Neither node-buttonImage
    nor the rle565 encoder dithers (to be confirmed).
  - Bands lining up with text: the labels are horizontal rows on the same key, so some alignment can be chance.
    Real alignment could come from the encoder (runs split where the text starts) or from how the fg is blended
    over the bg on the device, rather than from the panel drive.
  - A drive-strength fault would more likely show as wrong or flickering colours, noise on edges, or something
    that depends on what is next to it, not stable bands.
- Next check: render a baked key image on the PC exactly as the device would (decode the rle565 bg + fg to a
  PNG) and compare. Bands in the PNG mean quantization or the encoder; a clean PNG points at the device side.
  Then compare drive 0 against drive 1/2 with Wi-Fi idle, and photograph both.
- Background: CLAUDE.md "Hardware" (Display): at drive 2 (the default), the RGB bus, most likely the 25 MHz PCLK,
  interfered with Wi-Fi (pings 100-1000 ms with loss). Drive 0 gave 2-10 ms pings and looked clean then. The
  pixel clock is now 20 MHz (`CB_PIXEL_CLOCK_MHZ` overrides it). Memory note: `wifi-latency-tts4-2g.md`.
- Separate open item (activity job Notes, section 17): after a task watchdog, the left ~1/8 of the screen stayed
  corrupted. Probably a different cause (an ISR stall), but keep it in mind when telling artefacts apart.
- Possible directions (not investigated): per-pin drive (e.g. stronger on data pins, weakest on PCLK), drive 1,
  a lower pixel clock, and a test config with flat faces next to gradient ones (`color` is shorthand for a
  top-to-darker gradient, so `net-test.json` has gradients too, just darker ones).

## Flagged assumptions
- None yet.

## Plan
(not started)
