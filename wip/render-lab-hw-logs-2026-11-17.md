# Render lab hardware logs (2026-11-17)

The user's runs for step 10 of `wip-on-device-render-feasibility.md`, pasted verbatim. Board: Waveshare ESP32-S3-Touch-LCD-4.3, fw `0.2.0-dev+d9e36de.dirty`, pixel clock 20 MHz.

User's visual check of page 0: the keys look OK, except the "Wide key" (2U case): its content shows left of its border, in the gap between the buttons (but inside the button image's bounds). Cause and fix: the job document, Implement notes, step 10.

## idf.py --preset release -DCB_RUN_STRESS=OFF -DCB_RENDER_LAB=ON -p COM4 flash monitor

```
I (1422) render_lab: ==== render lab: release (-O2) build, fw 0.2.0-dev+d9e36de.dirty, CPU 240 MHz, pixel clock 20 MHz (fill budget 615 us); 11 cases on a core 0 task, ~300 ms each, banded = 16-row bands; times are means, cold = the first run's render + encode ====
I (1432) render_lab: assets: render_lab.bin loaded, path_vectors.bin 190128 B; buffers px/scr/ws/out: I internal (<= 48 KB, keeping 24 KB in the largest block), P PSRAM, - none
I (1452) render_lab: heap before the lab: internal 201943 free (largest 155648, min ever 199655), PSRAM 3674972 free
I (1762) render_lab: 1U face + label 20           144x144 | render 777.1 us, banded 1014.3 us, encode 13071.2 us | 84 colours, 3270 B (bound 22412), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 18 reps, cold 14978.0 us | put 2429 us (lock wait 15 us) | px/scr/ws/out I/-/I/I
I (1932) render_lab:   key 61440 at (0,0)q: page 0 with 1 keys on screen in 18.8 ms | state change to screen 40.84 ms = render+encode 14.68 + put 2.45 + draw until shown 23.72 ms
I (2252) render_lab: 1U face + label 27 sp4       144x144 | render 779.8 us, banded 994.1 us, encode 13300.3 us | 90 colours, 3696 B (bound 22436), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 19 reps, cold 14201.0 us | put 2513 us (lock wait 14 us) | px/scr/ws/out I/-/I/I
I (2422) render_lab:   key 61441 at (4,0)q: page 0 with 2 keys on screen in 20.8 ms | state change to screen 32.80 ms = render+encode 14.44 + put 2.57 + draw until shown 15.79 ms
I (2752) render_lab: 1U recoloured face + label   144x144 | render 697.0 us, banded 908.9 us, encode 13017.9 us | 81 colours, 3256 B (bound 22400), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 20 reps, cold 13870.0 us | put 2318 us (lock wait 8 us) | px/scr/ws/out I/-/I/I
I (2912) render_lab:   key 61442 at (8,0)q: page 0 with 3 keys on screen in 22.8 ms | state change to screen 34.84 ms = render+encode 14.02 + put 2.20 + draw until shown 18.62 ms
I (3262) render_lab: 1U slider                    144x144 | render 769.8 us, banded 942.6 us, encode 13213.5 us | 92 colours, 3528 B (bound 22444), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 20 reps, cold 14088.0 us | put 2389 us (lock wait 7 us) | px/scr/ws/out I/-/I/I
I (3422) render_lab:   key 61443 at (12,0)q: page 0 with 4 keys on screen in 25.3 ms | state change to screen 37.34 ms = render+encode 14.25 + put 2.45 + draw until shown 20.64 ms
I (3762) render_lab: 1U image icon + label        144x144 | render 1076.8 us, banded 1409.4 us, encode 14403.2 us | 186 colours, 4994 B (bound 22820), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 17 reps, cold 15495.0 us | put 2595 us (lock wait 7 us) | px/scr/ws/out I/-/I/I
I (3912) render_lab:   key 61444 at (16,0)q: page 0 with 5 keys on screen in 17.3 ms | state change to screen 39.34 ms = render+encode 15.88 + put 2.75 + draw until shown 20.72 ms
I (4252) render_lab: 1U path icon + label         144x144 | render 2250.8 us, banded 2706.4 us, encode 14006.1 us | 115 colours, 4325 B (bound 22536), ws 8192 B | 565 40.5/4.5 KB, path 31.2/6.2 KB | 15 reps, cold 16447.0 us | put 2626 us (lock wait 16 us) | px/scr/ws/out I/I/I/I
I (4412) render_lab:   key 61445 at (0,4)q: page 0 with 6 keys on screen in 19.3 ms | state change to screen 41.34 ms = render+encode 16.43 + put 2.74 + draw until shown 22.17 ms
I (4752) render_lab: 1U glyphs from outlines      144x144 | render 2044.0 us, banded 2256.6 us, encode 13178.0 us | 95 colours, 3490 B (bound 22456), ws 8192 B | 565 40.5/4.5 KB, path 3.9/1.6 KB | 17 reps, cold 15298.0 us | put 2409 us (lock wait 10 us) | px/scr/ws/out I/I/I/I
I (4902) render_lab:   key 61446 at (4,4)q: page 0 with 7 keys on screen in 11.3 ms | state change to screen 33.34 ms = render+encode 15.53 + put 2.52 + draw until shown 15.29 ms
I (5272) render_lab: 2U faces + text 27           288x144 | render 6488.9 us, banded 4348.4 us, encode 29147.0 us | 208 colours, 7135 B (bound 43932), ws 8192 B | 565 81.0/9.0 KB, path 0.0/0.0 KB | 7 reps, cold 35499.0 us | put 4712 us (lock wait 12 us) | px/scr/ws/out P/-/I/I
I (5452) render_lab:   key 61447 at (8,4)q: page 0 with 8 keys on screen in 24.3 ms | state change to screen 56.84 ms = render+encode 35.80 + put 4.90 + draw until shown 16.14 ms
I (6782) render_lab: 720x432 face + text + fill   720x432 | render 47152.3 us, banded 28143.0 us, encode 182159.0 us | 85 colours, 11161 B (bound 319200), ws 8192 B | 565 607.5/22.5 KB, path 0.0/0.0 KB | 3 reps, cold 229074.0 us | put 27620 us (lock wait 17 us) | px/scr/ws/out P/-/I/P
I (6792) render_lab:   not shown (no room on page 0, or not whole cells)
I (6812) render_lab: 1U recolour copy (no render) 144x144 | render 66.2 us, banded 0.0 us, encode 0.0 us | 84 colours, 3270 B (bound 3270), ws 0 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 50 reps, cold 104.0 us | put 2309 us (lock wait 5 us) | px/scr/ws/out -/-/-/I
I (6952) render_lab:   key 61449 at (16,4)q: page 0 with 9 keys on screen in 21.8 ms | state change to screen 13.34 ms = render+encode 0.15 + put 2.36 + draw until shown 10.83 ms
I (7302) render_lab: 1U large palette >256        144x144 | render 4689.1 us, banded 5082.3 us, encode 14838.2 us | 326 colours, 6250 B (bound 44116), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 12 reps, cold 19625.0 us | put 2916 us (lock wait 9 us) | px/scr/ws/out I/-/I/I
I (7462) render_lab:   key 61450 at (0,8)q: page 0 with 10 keys on screen in 13.8 ms | state change to screen 35.84 ms = render+encode 19.73 + put 2.96 + draw until shown 13.15 ms
I (7482) render_lab: fills during the lab: fill max 493 us, 0 over | start late max 9 us, end max 493 us (budget 615 us), 0 missed, 0 seq breaks, 0 buffer mismatches
I (7482) render_lab: lab task stack: 6008 of 8192 bytes never used
I (7492) render_lab: heap after the lab: internal 192799 free (largest 147456, min ever 83923), PSRAM 3620360 free
I (7502) render_lab: image store: 21 images, 53 KB used (was 10, 0 KB); page 0 shows 10 lab keys (stored layout's Home replaced until a host takes control or the next boot) ====
```

## idf.py --preset debug -DCB_RUN_STRESS=OFF -DCB_RENDER_LAB=ON -p COM4 flash monitor

```
I (1520) render_lab: ==== render lab: debug (-Og) build, fw 0.2.0-dev+d9e36de.dirty, CPU 240 MHz, pixel clock 20 MHz (fill budget 615 us); 11 cases on a core 0 task, ~300 ms each, banded = 16-row bands; times are means, cold = the first run's render + encode ====
I (1530) render_lab: assets: render_lab.bin loaded, path_vectors.bin 190128 B; buffers px/scr/ws/out: I internal (<= 48 KB, keeping 24 KB in the largest block), P PSRAM, - none
I (1540) render_lab: heap before the lab: internal 189511 free (largest 139264, min ever 187223), PSRAM 3631156 free
I (1880) render_lab: 1U face + label 20           144x144 | render 2240.5 us, banded 2445.0 us, encode 20007.3 us | 84 colours, 3270 B (bound 22412), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 11 reps, cold 22806.0 us | put 2697 us (lock wait 19 us) | px/scr/ws/out I/-/I/I
I (2030) render_lab:   key 61440 at (0,0)q: page 0 with 1 keys on screen in 16.0 ms | state change to screen 38.02 ms = render+encode 23.01 + put 2.75 + draw until shown 12.26 ms
I (2370) render_lab: 1U face + label 27 sp4       144x144 | render 2174.7 us, banded 2440.5 us, encode 20438.7 us | 90 colours, 3696 B (bound 22436), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 11 reps, cold 22637.0 us | put 2866 us (lock wait 18 us) | px/scr/ws/out I/-/I/I
I (2530) render_lab:   key 61441 at (4,0)q: page 0 with 2 keys on screen in 8.0 ms | state change to screen 40.03 ms = render+encode 22.82 + put 2.90 + draw until shown 14.31 ms
I (2880) render_lab: 1U recoloured face + label   144x144 | render 2075.2 us, banded 2347.2 us, encode 19986.2 us | 81 colours, 3256 B (bound 22400), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 12 reps, cold 22071.0 us | put 2706 us (lock wait 19 us) | px/scr/ws/out I/-/I/I
I (3040) render_lab:   key 61442 at (8,0)q: page 0 with 3 keys on screen in 20.5 ms | state change to screen 32.53 ms = render+encode 22.30 + put 2.67 + draw until shown 7.55 ms
I (3370) render_lab: 1U slider                    144x144 | render 2208.9 us, banded 2430.5 us, encode 20300.5 us | 92 colours, 3528 B (bound 22444), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 11 reps, cold 22641.0 us | put 2783 us (lock wait 10 us) | px/scr/ws/out I/-/I/I
I (3530) render_lab:   key 61443 at (12,0)q: page 0 with 4 keys on screen in 12.5 ms | state change to screen 34.52 ms = render+encode 22.68 + put 2.89 + draw until shown 8.95 ms
I (3870) render_lab: 1U image icon + label        144x144 | render 2575.3 us, banded 2981.1 us, encode 22291.5 us | 186 colours, 4994 B (bound 22820), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 10 reps, cold 24937.0 us | put 3118 us (lock wait 9 us) | px/scr/ws/out I/-/I/I
I (4040) render_lab:   key 61444 at (16,0)q: page 0 with 5 keys on screen in 25.0 ms | state change to screen 37.03 ms = render+encode 25.10 + put 3.15 + draw until shown 8.78 ms
I (4400) render_lab: 1U path icon + label         144x144 | render 4342.1 us, banded 4977.3 us, encode 21610.9 us | 115 colours, 4325 B (bound 22536), ws 8192 B | 565 40.5/4.5 KB, path 31.2/6.2 KB | 9 reps, cold 26092.0 us | put 2977 us (lock wait 10 us) | px/scr/ws/out I/I/I/I
I (4550) render_lab:   key 61445 at (0,4)q: page 0 with 6 keys on screen in 17.5 ms | state change to screen 39.52 ms = render+encode 26.29 + put 3.05 + draw until shown 10.18 ms
I (4910) render_lab: 1U glyphs from outlines      144x144 | render 3824.9 us, banded 4193.8 us, encode 20250.8 us | 95 colours, 3490 B (bound 22456), ws 8192 B | 565 40.5/4.5 KB, path 3.9/1.6 KB | 10 reps, cold 24177.0 us | put 2765 us (lock wait 17 us) | px/scr/ws/out I/I/I/I
I (5090) render_lab:   key 61446 at (4,4)q: page 0 with 7 keys on screen in 20.0 ms | state change to screen 52.52 ms = render+encode 24.37 + put 2.84 + draw until shown 25.32 ms
I (5500) render_lab: 2U faces + text 27           288x144 | render 7282.8 us, banded 6157.0 us, encode 43847.4 us | 208 colours, 7135 B (bound 43932), ws 8192 B | 565 81.0/9.0 KB, path 0.0/0.0 KB | 5 reps, cold 51218.0 us | put 5565 us (lock wait 10 us) | px/scr/ws/out P/-/I/I
I (5700) render_lab:   key 61447 at (8,4)q: page 0 with 8 keys on screen in 14.5 ms | state change to screen 77.48 ms = render+encode 51.60 + put 5.70 + draw until shown 20.18 ms
I (7570) render_lab: 720x432 face + text + fill   720x432 | render 51341.3 us, banded 45253.0 us, encode 269020.3 us | 85 colours, 11161 B (bound 319200), ws 8192 B | 565 607.5/22.5 KB, path 0.0/0.0 KB | 3 reps, cold 320589.0 us | put 28882 us (lock wait 18 us) | px/scr/ws/out P/-/I/P
I (7590) render_lab:   not shown (no room on page 0, or not whole cells)
I (7600) render_lab: 1U recolour copy (no render) 144x144 | render 98.7 us, banded 0.0 us, encode 0.0 us | 84 colours, 3270 B (bound 3270), ws 0 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 50 reps, cold 155.0 us | put 2714 us (lock wait 8 us) | px/scr/ws/out -/-/-/I
I (7730) render_lab:   key 61449 at (16,4)q: page 0 with 9 keys on screen in 5.5 ms | state change to screen 17.02 ms = render+encode 0.19 + put 2.72 + draw until shown 14.11 ms
I (8090) render_lab: 1U large palette >256        144x144 | render 7327.0 us, banded 7868.0 us, encode 22816.7 us | 326 colours, 6250 B (bound 44116), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 7 reps, cold 30206.0 us | put 3718 us (lock wait 16 us) | px/scr/ws/out I/-/I/I
I (8240) render_lab:   key 61450 at (0,8)q: page 0 with 10 keys on screen in 7.5 ms | state change to screen 39.52 ms = render+encode 30.41 + put 3.80 + draw until shown 5.32 ms
I (8260) render_lab: fills during the lab: fill max 247 us, 0 over | start late max 9 us, end max 247 us (budget 615 us), 0 missed, 0 seq breaks, 0 buffer mismatches
I (8260) render_lab: lab task stack: 6024 of 8192 bytes never used
I (8270) render_lab: heap after the lab: internal 180367 free (largest 131072, min ever 71491), PSRAM 3576544 free
I (8280) render_lab: image store: 21 images, 53 KB used (was 10, 0 KB); page 0 shows 10 lab keys (stored layout's Home replaced until a host takes control or the next boot) ====
```

## idf.py --preset release -DCB_RUN_STRESS=ON -DCB_RENDER_LAB=ON -p COM4 flash monitor

```
I (63431) controlblock: ---- stress: fill budget 615 us; compositor: ~1024 B chunks, chunk worst case COMP_CHUNK_WORST_US 140 us, margin COMP_GAP_MARGIN_US 65 us (gap deadline 550 us after the fill start), guard estimate COMP_GAP_BYTES_EST 3072 B/fill ----
I (63441) controlblock: stress: 20 demo images (15 1U, 5 2U), the store then had 30 images | all accepted, PSRAM copy intact
I (63451) controlblock: stress at rest, incremental       fill max 143 us, 0 over | keys 302 (max  2916 us),   0 erases, busy  38% | 5448 B/gap, chunk max 86/140 us, gap end max 486/550 us, 0 late | frames 98/97 | start late max   2 us, end max 143 us, 0 missed
I (63481) controlblock: stress at rest, flush             fill max 143 us, 0 over | keys 368 (max  2911 us),   0 erases, busy  46% | 5538 B/gap, chunk max 81/140 us, gap end max 489/550 us, 0 late | frames 99/98 | start late max   2 us, end max 143 us, 0 missed
I (63501) controlblock: stress spans at rest, incr        fill max 141 us, 0 over | keys 220 (max  4995 us),   0 erases, busy  35% | 5584 B/gap, chunk max 78/140 us, gap end max 476/550 us, 0 late | frames 98/98 | start late max   3 us, end max 141 us, 0 missed
I (63531) controlblock: stress mid-swipe, incremental     fill max 184 us, 0 over | keys 290 (max  2981 us),   8 erases, busy  37% | 4046 B/gap, chunk max 82/140 us, gap end max 487/550 us, 0 late | frames 98/98 | start late max   2 us, end max 186 us, 0 missed
I (63551) controlblock: stress mid-swipe, flush           fill max 184 us, 0 over | keys 292 (max  2977 us),   0 erases, busy  38% | 3946 B/gap, chunk max 83/140 us, gap end max 485/550 us, 0 late | frames 98/97 | start late max   2 us, end max 185 us, 0 missed
I (63571) controlblock: stress page build, 1U             fill max 143 us, 0 over | keys 395 (max  2915 us),   0 erases, busy  48% | 5546 B/gap, chunk max 81/140 us, gap end max 488/550 us, 0 late | frames 103/102 | build 87 ms (max 100), 24 builds | start late max   3 us, end max 143 us, 0 missed
I (63601) controlblock: stress page build, spans          fill max 143 us, 0 over | keys 274 (max  4972 us),   0 erases, busy  47% | 5666 B/gap, chunk max 77/140 us, gap end max 473/550 us, 0 late | frames 100/99 | build 82 ms (max 90), 25 builds | start late max   2 us, end max 143 us, 0 missed
I (63621) controlblock: stress layout changes, at rest    fill max 143 us, 0 over | keys 269 (max  4983 us),  38 erases, busy  42% | 6178 B/gap, chunk max 82/140 us, gap end max 489/550 us, 0 late | frames 98/98 | start late max   4 us, end max 143 us, 0 missed
I (63651) controlblock: stress layout changes, mid        fill max 184 us, 0 over | keys 196 (max  5061 us), 136 erases, busy  33% | 5735 B/gap, chunk max 78/140 us, gap end max 476/550 us, 0 late | frames 97/97 | start late max   2 us, end max 186 us, 0 missed
I (63671) controlblock: stress status bar only, mid       fill max 288 us, 0 over | keys   0 (max     0 us),   0 erases, busy   0% |    0 B/gap, chunk max  0/140 us, gap end max   0/550 us, 0 late | frames 99/98 | start late max   2 us, end max 289 us, 0 missed
I (63691) controlblock: stress mid, flush + status        fill max 224 us, 0 over | keys 181 (max  3072 us),   0 erases, busy  24% | 3863 B/gap, chunk max 118/140 us, gap end max 488/550 us, 0 late | frames 98/97 | start late max   7 us, end max 224 us, 0 missed
I (63711) controlblock: stress image replaces, at rest    fill max 189 us, 0 over | keys 199 (max  2724 us),   0 erases, busy  25% | 5406 B/gap, chunk max 99/140 us, gap end max 496/550 us, 0 late | frames 98/98 | start late max   9 us, end max 189 us, 0 missed
I (63741) controlblock: stress image replaces, mid        fill max 240 us, 0 over | keys  69 (max  2734 us),   0 erases, busy   9% | 3871 B/gap, chunk max 104/140 us, gap end max 496/550 us, 0 late | frames 98/98 | start late max  10 us, end max 244 us, 0 missed
I (63761) controlblock: stress lua churn, at rest         fill max 192 us, 0 over | keys   0 (max     0 us),   0 erases, busy   0% |    0 B/gap, chunk max  0/140 us, gap end max   0/550 us, 0 late | frames 99/97 | start late max  11 us, end max 192 us, 0 missed
I (63771) controlblock:        lua: 98 runs, 0 errors, heap peak 80 KB, 0 allocs refused (totals so far)
I (63791) controlblock: stress lua churn, mid-swipe       fill max 381 us, 0 over | keys   0 (max     0 us),   0 erases, busy   0% |    0 B/gap, chunk max  0/140 us, gap end max   0/550 us, 0 late | frames 99/98 | start late max  14 us, end max 381 us, 0 missed
I (63811) controlblock:        lua: 68 runs, 0 errors, heap peak 80 KB, 0 allocs refused (totals so far)
I (63821) controlblock: stress lua + keys, mid, flush     fill max 409 us, 0 over | keys 186 (max  4123 us),   0 erases, busy  30% | 2606 B/gap, chunk max 133/140 us, gap end max 519/550 us, 0 late | frames 101/99 | start late max  16 us, end max 414 us, 0 missed
I (63841) controlblock:        lua: 52 runs, 0 errors, heap peak 80 KB, 0 allocs refused (totals so far)
I (63861) controlblock: stress render 1U, at rest         fill max 173 us, 0 over | keys 328 (max  2998 us),   0 erases, busy  43% | 5429 B/gap, chunk max 102/140 us, gap end max 489/550 us, 0 late | frames 100/97 | start late max   5 us, end max 173 us, 0 missed
I (63871) controlblock:        render: 1U slider, 102 runs, 0 failed, 18.32 ms a run (render + encode + put), px/scr/ws/out I/-/I/I
I (63891) controlblock: stress render 1U, mid-swipe       fill max 210 us, 0 over | keys 268 (max  2993 us),   0 erases, busy  35% | 3949 B/gap, chunk max 98/140 us, gap end max 496/550 us, 0 late | frames 100/97 | start late max  16 us, end max 222 us, 0 missed
I (63901) controlblock:        render: 1U slider, 68 runs, 0 failed, 22.25 ms a run (render + encode + put), px/scr/ws/out I/-/I/I
I (63931) controlblock: stress render 1U + keys, mid, fl  fill max 226 us, 0 over | keys 277 (max  3240 us),   8 erases, busy  38% | 3838 B/gap, chunk max 101/140 us, gap end max 496/550 us, 0 late | frames 100/97 | start late max   7 us, end max 229 us, 0 missed
I (63941) controlblock:        render: 1U slider, 68 runs, 0 failed, 22.59 ms a run (render + encode + put), px/scr/ws/out I/-/I/I
I (63961) controlblock: stress render 2U PSRAM, mid       fill max 622 us, 1 over | keys 104 (max  6637 us),   0 erases, busy  29% | 3091 B/gap, chunk max 106/140 us, gap end max 497/550 us, 0 late | frames 103/97 | start late max  17 us, end max 622 us, 1 missed [late: status 0, 1 page 0, 2 pages 1; status drawing 0]
I (63981) controlblock:        render: 2U faces + text 27, 34 runs, 0 failed, 58.85 ms a run (render + encode + put), px/scr/ws/out P/-/I/I
I (64001) controlblock: stress render 2U PSRAM + keys, fl fill max 637 us, 2 over | keys 207 (max  6041 us),   8 erases, busy  31% | 3310 B/gap, chunk max 148/140 us, gap end max 540/550 us, 0 late | frames 105/99 | start late max  27 us, end max 637 us, 2 missed [late: status 0, 1 page 0, 2 pages 2; status drawing 0]
I (64021) controlblock:        render: 2U faces + text 27, 34 runs, 0 failed, 58.41 ms a run (render + encode + put), px/scr/ws/out P/-/I/I
I (64041) controlblock: stress render >256 colours, mid   fill max 216 us, 0 over | keys 252 (max  3128 us),   8 erases, busy  35% | 3816 B/gap, chunk max 105/140 us, gap end max 496/550 us, 0 late | frames 102/99 | start late max   7 us, end max 216 us, 0 missed
I (64051) controlblock:        render: 1U large palette >256, 52 runs, 0 failed, 30.93 ms a run (render + encode + put), px/scr/ws/out I/-/I/I
I (64081) controlblock: stress render 720x432 PSRAM, mid  fill max 629 us, 18 over | keys   0 (max     0 us),   0 erases, busy   0% |    0 B/gap, chunk max  0/140 us, gap end max   0/550 us, 0 late | frames 146/112 | start late max  20 us, end max 629 us, 18 missed [late: status 0, 1 page 0, 2 pages 18; status drawing 0]
I (64091) controlblock:        render: 720x432 face + text + fill, 8 runs, 0 failed, 328.08 ms a run (render + encode + put), px/scr/ws/out P/-/I/P
I (64121) render_lab: ==== render lab: release (-O2) build, fw 0.2.0-dev+d9e36de.dirty, CPU 240 MHz, pixel clock 20 MHz (fill budget 615 us); 11 cases on a core 0 task, ~300 ms each, banded = 16-row bands; times are means, cold = the first run's render + encode ====
I (64131) render_lab: assets: render_lab.bin loaded, path_vectors.bin 190128 B; buffers px/scr/ws/out: I internal (<= 48 KB, keeping 24 KB in the largest block), P PSRAM, - none
I (64151) render_lab: heap before the lab: internal 182331 free (largest 73728, min ever 84119), PSRAM 3239728 free
I (64471) render_lab: 1U face + label 20           144x144 | render 729.4 us, banded 1002.1 us, encode 13085.1 us | 84 colours, 3270 B (bound 22412), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 19 reps, cold 14100.0 us | put 2337 us (lock wait 9 us) | px/scr/ws/out I/-/I/I
I (64621) render_lab:   key 61440 at (0,0)q: page 0 with 1 keys on screen in 17.3 ms | state change to screen 39.31 ms = render+encode 14.06 + put 2.47 + draw until shown 22.78 ms
I (64951) render_lab: 1U face + label 27 sp4       144x144 | render 783.7 us, banded 990.3 us, encode 13298.9 us | 90 colours, 3696 B (bound 22436), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 19 reps, cold 14278.0 us | put 2403 us (lock wait 9 us) | px/scr/ws/out I/-/I/I
I (65121) render_lab:   key 61441 at (4,0)q: page 0 with 2 keys on screen in 19.3 ms | state change to screen 41.31 ms = render+encode 14.33 + put 2.47 + draw until shown 24.51 ms
I (65451) render_lab: 1U recoloured face + label   144x144 | render 698.4 us, banded 908.6 us, encode 13019.8 us | 81 colours, 3256 B (bound 22400), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 20 reps, cold 13885.0 us | put 2415 us (lock wait 9 us) | px/scr/ws/out I/-/I/I
I (65611) render_lab:   key 61442 at (8,0)q: page 0 with 3 keys on screen in 21.3 ms | state change to screen 33.31 ms = render+encode 14.00 + put 2.37 + draw until shown 16.94 ms
I (65931) render_lab: 1U slider                    144x144 | render 767.4 us, banded 941.8 us, encode 13212.9 us | 92 colours, 3528 B (bound 22444), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 19 reps, cold 14152.0 us | put 2437 us (lock wait 8 us) | px/scr/ws/out I/-/I/I
I (66081) render_lab:   key 61443 at (12,0)q: page 0 with 4 keys on screen in 12.8 ms | state change to screen 34.81 ms = render+encode 14.38 + put 2.42 + draw until shown 18.02 ms
I (66421) render_lab: 1U image icon + label        144x144 | render 1081.4 us, banded 1405.9 us, encode 14401.1 us | 186 colours, 4994 B (bound 22820), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 17 reps, cold 15560.0 us | put 2603 us (lock wait 9 us) | px/scr/ws/out I/-/I/I
I (66571) render_lab:   key 61444 at (16,0)q: page 0 with 5 keys on screen in 14.8 ms | state change to screen 36.81 ms = render+encode 15.81 + put 2.80 + draw until shown 18.20 ms
I (66911) render_lab: 1U path icon + label         144x144 | render 2249.7 us, banded 2701.6 us, encode 14030.1 us | 115 colours, 4325 B (bound 22536), ws 8192 B | 565 40.5/4.5 KB, path 31.2/6.2 KB | 15 reps, cold 16476.0 us | put 2696 us (lock wait 8 us) | px/scr/ws/out I/I/I/P
I (67061) render_lab:   key 61445 at (0,4)q: page 0 with 6 keys on screen in 6.8 ms | state change to screen 38.81 ms = render+encode 16.67 + put 2.66 + draw until shown 19.49 ms
I (67411) render_lab: 1U glyphs from outlines      144x144 | render 2042.0 us, banded 2254.9 us, encode 13176.1 us | 95 colours, 3490 B (bound 22456), ws 8192 B | 565 40.5/4.5 KB, path 3.9/1.6 KB | 17 reps, cold 15267.0 us | put 2433 us (lock wait 8 us) | px/scr/ws/out I/I/I/I
I (67561) render_lab:   key 61446 at (4,4)q: page 0 with 7 keys on screen in 8.8 ms | state change to screen 40.81 ms = render+encode 15.48 + put 2.52 + draw until shown 22.81 ms
I (67931) render_lab: 2U faces + text 27           288x144 | render 6309.1 us, banded 4380.7 us, encode 29212.4 us | 208 colours, 7135 B (bound 43932), ws 8192 B | 565 81.0/9.0 KB, path 0.0/0.0 KB | 7 reps, cold 35451.0 us | put 4785 us (lock wait 16 us) | px/scr/ws/out P/-/I/I
I (68111) render_lab:   key 61447 at (8,4)q: page 0 with 8 keys on screen in 21.7 ms | state change to screen 54.31 ms = render+encode 35.59 + put 4.90 + draw until shown 13.83 ms
I (69431) render_lab: 720x432 face + text + fill   720x432 | render 46980.7 us, banded 28339.7 us, encode 182394.7 us | 85 colours, 11161 B (bound 319200), ws 8192 B | 565 607.5/22.5 KB, path 0.0/0.0 KB | 3 reps, cold 229085.0 us | put 27510 us (lock wait 17 us) | px/scr/ws/out P/-/I/P
I (69451) render_lab:   not shown (no room on page 0, or not whole cells)
I (69461) render_lab: 1U recolour copy (no render) 144x144 | render 68.9 us, banded 0.0 us, encode 0.0 us | 84 colours, 3270 B (bound 3270), ws 0 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 50 reps, cold 154.0 us | put 2415 us (lock wait 8 us) | px/scr/ws/out -/-/-/I
I (69611) render_lab:   key 61449 at (16,4)q: page 0 with 9 keys on screen in 19.3 ms | state change to screen 20.81 ms = render+encode 0.10 + put 2.34 + draw until shown 18.37 ms
I (69961) render_lab: 1U large palette >256        144x144 | render 4690.2 us, banded 5083.8 us, encode 14842.2 us | 326 colours, 6250 B (bound 44116), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 12 reps, cold 19634.0 us | put 2882 us (lock wait 16 us) | px/scr/ws/out I/-/I/I
I (70121) render_lab:   key 61450 at (0,8)q: page 0 with 10 keys on screen in 11.3 ms | state change to screen 33.31 ms = render+encode 19.83 + put 2.92 + draw until shown 10.56 ms
I (70131) render_lab: fills during the lab: fill max 493 us, 0 over | start late max 9 us, end max 493 us (budget 615 us), 0 missed, 0 seq breaks, 0 buffer mismatches
I (70141) render_lab: lab task stack: 6072 of 8192 bytes never used
I (70141) render_lab: heap after the lab: internal 173679 free (largest 73728, min ever 75923), PSRAM 3185116 free
I (70151) render_lab: image store: 11 images, 53 KB used (was 0, 0 KB); page 0 shows 10 lab keys (stored layout's Home replaced until a host takes control or the next boot) ====
```

## idf.py --preset debug -DCB_RUN_STRESS=ON -DCB_RENDER_LAB=ON -p COM4 flash monitor

```
I (64008) controlblock: ---- stress: fill budget 615 us; compositor: ~1024 B chunks, chunk worst case COMP_CHUNK_WORST_US 140 us, margin COMP_GAP_MARGIN_US 65 us (gap deadline 550 us after the fill start), guard estimate COMP_GAP_BYTES_EST 3072 B/fill ----
I (64018) controlblock: stress: 20 demo images (15 1U, 5 2U), the store then had 30 images | all accepted, PSRAM copy intact
I (64028) controlblock: stress at rest, incremental       fill max 142 us, 0 over | keys 278 (max  2976 us),   0 erases, busy  36% | 5291 B/gap, chunk max 108/140 us, gap end max 486/550 us, 0 late | frames 98/98 | start late max   4 us, end max 142 us, 0 missed
I (64058) controlblock: stress at rest, flush             fill max 142 us, 0 over | keys 371 (max  2982 us),   0 erases, busy  48% | 5404 B/gap, chunk max 92/140 us, gap end max 483/550 us, 0 late | frames 98/98 | start late max   5 us, end max 142 us, 0 missed
I (64078) controlblock: stress spans at rest, incr        fill max 142 us, 0 over | keys 219 (max  5149 us),   0 erases, busy  36% | 5461 B/gap, chunk max 89/140 us, gap end max 483/550 us, 0 late | frames 99/98 | start late max   3 us, end max 142 us, 0 missed
I (64108) controlblock: stress mid-swipe, incremental     fill max 198 us, 0 over | keys 243 (max  3030 us),   8 erases, busy  32% | 3899 B/gap, chunk max 96/140 us, gap end max 485/550 us, 0 late | frames 99/98 | start late max   3 us, end max 198 us, 0 missed
I (64128) controlblock: stress mid-swipe, flush           fill max 189 us, 0 over | keys 246 (max  3015 us),   0 erases, busy  33% | 3809 B/gap, chunk max 90/140 us, gap end max 480/550 us, 0 late | frames 98/97 | start late max   3 us, end max 191 us, 0 missed
I (64148) controlblock: stress page build, 1U             fill max 142 us, 0 over | keys 378 (max  2961 us),   0 erases, busy  47% | 5412 B/gap, chunk max 91/140 us, gap end max 490/550 us, 0 late | frames 102/101 | build 91 ms (max 100), 23 builds | start late max   2 us, end max 142 us, 0 missed
I (64178) controlblock: stress page build, spans          fill max 142 us, 0 over | keys 274 (max  5135 us),   0 erases, busy  49% | 5535 B/gap, chunk max 84/140 us, gap end max 481/550 us, 0 late | frames 100/99 | build 82 ms (max 90), 25 builds | start late max   2 us, end max 142 us, 0 missed
I (64198) controlblock: stress layout changes, at rest    fill max 142 us, 0 over | keys 332 (max  5156 us),  38 erases, busy  47% | 5890 B/gap, chunk max 88/140 us, gap end max 479/550 us, 0 late | frames 98/98 | start late max   2 us, end max 142 us, 0 missed
I (64228) controlblock: stress layout changes, mid        fill max 188 us, 0 over | keys 202 (max  5256 us), 103 erases, busy  33% | 4987 B/gap, chunk max 97/140 us, gap end max 489/550 us, 0 late | frames 98/98 | start late max   3 us, end max 190 us, 0 missed
I (64248) controlblock: stress status bar only, mid       fill max 292 us, 0 over | keys   0 (max     0 us),   0 erases, busy   0% |    0 B/gap, chunk max  0/140 us, gap end max   0/550 us, 0 late | frames 98/98 | start late max   3 us, end max 293 us, 0 missed
I (64268) controlblock: stress mid, flush + status        fill max 239 us, 0 over | keys 181 (max  3099 us),   0 erases, busy  25% | 3658 B/gap, chunk max 119/140 us, gap end max 486/550 us, 0 late | frames 98/97 | start late max   5 us, end max 239 us, 0 missed
I (64298) controlblock: stress image replaces, at rest    fill max 336 us, 0 over | keys 190 (max  2788 us),   0 erases, busy  25% | 5281 B/gap, chunk max 100/140 us, gap end max 502/550 us, 0 late | frames 98/98 | start late max   8 us, end max 336 us, 0 missed
I (64318) controlblock: stress image replaces, mid        fill max 258 us, 0 over | keys  39 (max  3071 us),   0 erases, busy   5% | 3663 B/gap, chunk max 115/140 us, gap end max 493/550 us, 0 late | frames 98/97 | start late max  12 us, end max 258 us, 0 missed
I (64338) controlblock: stress lua churn, at rest         fill max 279 us, 0 over | keys   0 (max     0 us),   0 erases, busy   0% |    0 B/gap, chunk max  0/140 us, gap end max   0/550 us, 0 late | frames 101/98 | start late max  17 us, end max 279 us, 0 missed
I (64348) controlblock:        lua: 69 runs, 0 errors, heap peak 80 KB, 0 allocs refused (totals so far)
I (64368) controlblock: stress lua churn, mid-swipe       fill max 349 us, 0 over | keys   0 (max     0 us),   0 erases, busy   0% |    0 B/gap, chunk max  0/140 us, gap end max   0/550 us, 0 late | frames 99/97 | start late max  17 us, end max 357 us, 0 missed
I (64388) controlblock:        lua: 51 runs, 0 errors, heap peak 80 KB, 0 allocs refused (totals so far)
I (64408) controlblock: stress lua + keys, mid, flush     fill max 384 us, 0 over | keys 190 (max  4197 us),   0 erases, busy  31% | 2584 B/gap, chunk max 129/140 us, gap end max 532/550 us, 0 late | frames 103/101 | start late max  19 us, end max 384 us, 0 missed
I (64418) controlblock:        lua: 43 runs, 0 errors, heap peak 80 KB, 0 allocs refused (totals so far)
I (64438) controlblock: stress render 1U, at rest         fill max 154 us, 0 over | keys 374 (max  2895 us),   0 erases, busy  48% | 5421 B/gap, chunk max 93/140 us, gap end max 487/550 us, 0 late | frames 101/98 | start late max   8 us, end max 154 us, 0 missed
I (64448) controlblock:        render: 1U slider, 68 runs, 0 failed, 26.99 ms a run (render + encode + put), px/scr/ws/out I/-/I/I
I (64468) controlblock: stress render 1U, mid-swipe       fill max 225 us, 0 over | keys 255 (max  2941 us),   0 erases, busy  33% | 3888 B/gap, chunk max 93/140 us, gap end max 490/550 us, 0 late | frames 101/97 | start late max  10 us, end max 227 us, 0 missed
I (64478) controlblock:        render: 1U slider, 51 runs, 0 failed, 32.98 ms a run (render + encode + put), px/scr/ws/out I/-/I/I
I (64508) controlblock: stress render 1U + keys, mid, fl  fill max 238 us, 0 over | keys 273 (max  3221 us),   8 erases, busy  38% | 3778 B/gap, chunk max 102/140 us, gap end max 492/550 us, 0 late | frames 101/97 | start late max  12 us, end max 244 us, 0 missed
I (64518) controlblock:        render: 1U slider, 51 runs, 0 failed, 33.18 ms a run (render + encode + put), px/scr/ws/out I/-/I/I
I (64538) controlblock: stress render 2U PSRAM, mid       fill max 481 us, 0 over | keys 104 (max  6591 us),   0 erases, busy  28% | 3162 B/gap, chunk max 89/140 us, gap end max 473/550 us, 0 late | frames 106/98 | start late max   7 us, end max 481 us, 0 missed
I (64548) controlblock:        render: 2U faces + text 27, 26 runs, 0 failed, 78.34 ms a run (render + encode + put), px/scr/ws/out P/-/I/I
I (64578) controlblock: stress render 2U PSRAM + keys, fl fill max 484 us, 0 over | keys 211 (max  6755 us),  27 erases, busy  32% | 3545 B/gap, chunk max 135/140 us, gap end max 514/550 us, 0 late | frames 107/99 | start late max  14 us, end max 487 us, 0 missed
I (64588) controlblock:        render: 2U faces + text 27, 25 runs, 0 failed, 79.53 ms a run (render + encode + put), px/scr/ws/out P/-/I/I
I (64608) controlblock: stress render >256 colours, mid   fill max 230 us, 0 over | keys 242 (max  3177 us),   8 erases, busy  34% | 3602 B/gap, chunk max 131/140 us, gap end max 524/550 us, 0 late | frames 105/99 | start late max  10 us, end max 230 us, 0 missed
I (64618) controlblock:        render: 1U large palette >256, 42 runs, 0 failed, 44.55 ms a run (render + encode + put), px/scr/ws/out I/-/I/I
I (64648) controlblock: stress render 720x432 PSRAM, mid  fill max 483 us, 0 over | keys   0 (max     0 us),   0 erases, busy   0% |    0 B/gap, chunk max  0/140 us, gap end max   0/550 us, 0 late | frames 155/109 | start late max   9 us, end max 485 us, 0 missed
I (64658) controlblock:        render: 720x432 face + text + fill, 6 runs, 0 failed, 443.56 ms a run (render + encode + put), px/scr/ws/out P/-/I/P
I (64678) render_lab: ==== render lab: debug (-Og) build, fw 0.2.0-dev+d9e36de.dirty, CPU 240 MHz, pixel clock 20 MHz (fill budget 615 us); 11 cases on a core 0 task, ~300 ms each, banded = 16-row bands; times are means, cold = the first run's render + encode ====
I (64698) render_lab: assets: render_lab.bin loaded, path_vectors.bin 190128 B; buffers px/scr/ws/out: I internal (<= 48 KB, keeping 24 KB in the largest block), P PSRAM, - none
I (64708) render_lab: heap before the lab: internal 169899 free (largest 73728, min ever 71687), PSRAM 3196356 free
I (65068) render_lab: 1U face + label 20           144x144 | render 2201.6 us, banded 2490.2 us, encode 19998.2 us | 84 colours, 3270 B (bound 22412), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 12 reps, cold 22484.0 us | put 2672 us (lock wait 20 us) | px/scr/ws/out I/-/I/I
I (65218) render_lab:   key 61440 at (0,0)q: page 0 with 1 keys on screen in 16.8 ms | state change to screen 38.79 ms = render+encode 22.33 + put 2.75 + draw until shown 13.71 ms
I (65558) render_lab: 1U face + label 27 sp4       144x144 | render 2168.6 us, banded 2446.4 us, encode 20437.4 us | 90 colours, 3696 B (bound 22436), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 11 reps, cold 22644.0 us | put 2838 us (lock wait 22 us) | px/scr/ws/out I/-/I/I
I (65718) render_lab:   key 61441 at (4,0)q: page 0 with 2 keys on screen in 8.8 ms | state change to screen 40.79 ms = render+encode 22.81 + put 2.92 + draw until shown 15.06 ms
I (66068) render_lab: 1U recoloured face + label   144x144 | render 2070.3 us, banded 2344.8 us, encode 19984.7 us | 81 colours, 3256 B (bound 22400), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 12 reps, cold 22174.0 us | put 2612 us (lock wait 10 us) | px/scr/ws/out I/-/I/I
I (66228) render_lab:   key 61442 at (8,0)q: page 0 with 3 keys on screen in 21.3 ms | state change to screen 33.29 ms = render+encode 22.31 + put 2.76 + draw until shown 8.22 ms
I (66558) render_lab: 1U slider                    144x144 | render 2212.8 us, banded 2423.4 us, encode 20305.5 us | 92 colours, 3528 B (bound 22444), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 11 reps, cold 22554.0 us | put 2758 us (lock wait 21 us) | px/scr/ws/out I/-/I/I
I (66718) render_lab:   key 61443 at (12,0)q: page 0 with 4 keys on screen in 13.3 ms | state change to screen 35.26 ms = render+encode 22.71 + put 2.86 + draw until shown 9.69 ms
I (67068) render_lab: 1U image icon + label        144x144 | render 2579.4 us, banded 2987.1 us, encode 22288.3 us | 186 colours, 4994 B (bound 22820), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 10 reps, cold 25062.0 us | put 3122 us (lock wait 10 us) | px/scr/ws/out I/-/I/I
I (67228) render_lab:   key 61444 at (16,0)q: page 0 with 5 keys on screen in 15.8 ms | state change to screen 37.72 ms = render+encode 25.09 + put 3.15 + draw until shown 9.48 ms
I (67588) render_lab: 1U path icon + label         144x144 | render 4342.1 us, banded 4969.8 us, encode 21630.4 us | 115 colours, 4325 B (bound 22536), ws 8192 B | 565 40.5/4.5 KB, path 31.2/6.2 KB | 9 reps, cold 26097.0 us | put 3059 us (lock wait 11 us) | px/scr/ws/out I/I/I/P
I (67748) render_lab:   key 61445 at (0,4)q: page 0 with 6 keys on screen in 18.3 ms | state change to screen 40.28 ms = render+encode 26.24 + put 3.26 + draw until shown 10.78 ms
I (68098) render_lab: 1U glyphs from outlines      144x144 | render 3827.4 us, banded 4195.4 us, encode 20242.6 us | 95 colours, 3490 B (bound 22456), ws 8192 B | 565 40.5/4.5 KB, path 3.9/1.6 KB | 10 reps, cold 24110.0 us | put 2738 us (lock wait 11 us) | px/scr/ws/out I/I/I/I
I (68258) render_lab:   key 61446 at (4,4)q: page 0 with 7 keys on screen in 20.8 ms | state change to screen 32.78 ms = render+encode 24.38 + put 2.76 + draw until shown 5.64 ms
I (68668) render_lab: 2U faces + text 27           288x144 | render 7256.6 us, banded 6127.4 us, encode 43964.2 us | 208 colours, 7135 B (bound 43932), ws 8192 B | 565 81.0/9.0 KB, path 0.0/0.0 KB | 5 reps, cold 51200.0 us | put 5539 us (lock wait 10 us) | px/scr/ws/out P/-/I/I
I (68868) render_lab:   key 61447 at (8,4)q: page 0 with 8 keys on screen in 14.7 ms | state change to screen 77.79 ms = render+encode 51.53 + put 5.58 + draw until shown 20.68 ms
I (70738) render_lab: 720x432 face + text + fill   720x432 | render 51503.3 us, banded 45055.3 us, encode 269045.3 us | 85 colours, 11161 B (bound 319200), ws 8192 B | 565 607.5/22.5 KB, path 0.0/0.0 KB | 3 reps, cold 320374.0 us | put 28822 us (lock wait 22 us) | px/scr/ws/out P/-/I/P
I (70758) render_lab:   not shown (no room on page 0, or not whole cells)
I (70768) render_lab: 1U recolour copy (no render) 144x144 | render 98.3 us, banded 0.0 us, encode 0.0 us | 84 colours, 3270 B (bound 3270), ws 0 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 50 reps, cold 184.0 us | put 2616 us (lock wait 15 us) | px/scr/ws/out -/-/-/I
I (70898) render_lab:   key 61449 at (16,4)q: page 0 with 9 keys on screen in 5.8 ms | state change to screen 17.22 ms = render+encode 0.20 + put 2.71 + draw until shown 14.30 ms
I (71258) render_lab: 1U large palette >256        144x144 | render 7337.7 us, banded 7879.3 us, encode 22848.1 us | 326 colours, 6250 B (bound 44116), ws 8192 B | 565 40.5/4.5 KB, path 0.0/0.0 KB | 7 reps, cold 30212.0 us | put 3746 us (lock wait 11 us) | px/scr/ws/out I/-/I/P
I (71408) render_lab:   key 61450 at (0,8)q: page 0 with 10 keys on screen in 7.8 ms | state change to screen 39.79 ms = render+encode 30.44 + put 3.98 + draw until shown 5.36 ms
I (71428) render_lab: fills during the lab: fill max 240 us, 0 over | start late max 10 us, end max 240 us (budget 615 us), 0 missed, 0 seq breaks, 0 buffer mismatches
I (71428) render_lab: lab task stack: 6024 of 8192 bytes never used
I (71438) render_lab: heap after the lab: internal 161247 free (largest 73728, min ever 71687), PSRAM 3141744 free
I (71448) render_lab: image store: 11 images, 53 KB used (was 0, 0 KB); page 0 shows 10 lab keys (stored layout's Home replaced until a host takes control or the next boot) ====
```
