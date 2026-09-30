#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Shared visual constants: screen dimensions and the dark-mode color
            palette every screen builds its UI from.
*/

// ── layout ────────────────────────────────────────────────────────────────────
// Physical panel mounting compensation — the panel sits a little off-center in the
// finished enclosure. First tried shifting the whole physical address window in
// main.cpp's lv_flush() (a DISPLAY_OFFSET_X/Y added to tft.setAddrWindow()) — CONFIRMED
// ON HARDWARE 2026-09-14 that a negative offset crashes the screen: TFT_eSPI's
// setAddrWindow() takes signed ints, but the ILI9488 panel itself only understands
// unsigned column/row addresses, so a negative offset sends a below-zero address for any
// redraw whose original x1/y1 is smaller than the offset magnitude — which includes the
// header bar and the top-left of every screen, since LVGL redraws from (0,0) on every
// screen load. Tried -5/-5, crashed immediately.
// Switched to shrinking the canvas instead, same day: SCREEN_W/H below are now 5px
// smaller than the physical 320x480 panel on each axis, with the origin left at (0,0) —
// every screen still renders from the true top-left corner, it just no longer reaches
// the panel's bottom/right edge. `lv_obj_create(nullptr)` auto-sizes every screen to
// whatever's registered as the LVGL display resolution (set from these two constants in
// main.cpp's lvgl_init()), so this one change covers every screen with no per-screen
// edits. The uncovered 5px strip along the bottom/right comes up clean black on its own —
// main.cpp's setup() already does a full-physical-panel tft.fillScreen(TFT_BLACK) at
// boot, before LVGL draws anything, so no extra clear step was needed. Trade-off: that
// strip is permanently unused screen space, not available to any screen's layout.
#define SCREEN_W  315
#define SCREEN_H  475
#define HDR_H      36

// ── colors (RGB888 — LVGL converts to RGB565) ─────────────────────────────────
// CORRECTED 2026-08-24 — C_SURFACE/C_DIM had a blue channel noticeably higher than
// red/green (0x1E1E2E, 0x4A4A5A), reading as blue-tinted "black" on real hardware,
// especially after RGB565 rounding (5/6/5 bits) exaggerates the imbalance further.
// Neutralized to equal R=G=B. C_GREEN/C_RED were both Material "A200" tones — high
// lightness, low saturation — which read as pastel mint / salmon-pink rather than a
// crisp green/red; swapped for Material 500 (classic, high-recognition traffic-light
// pair, better contrast at small text sizes on a dark background).
#define C_BG      0x121212
#define C_SURFACE 0x1E1E1E
#define C_CYAN    0x00BCD4
#define C_TEXT    0xEFEFEF
#define C_GREEN   0x4CAF50
#define C_ORANGE  0xFFB74D
#define C_DIM     0x4A4A4A
#define C_RED     0xF44336  /* physical Back-button color cue — distinct from the amber warning color */
#define C_YELLOW  0xFFEB3B  /* physical Up/Down/Left/Right button color cue, added 2026-08-26 — same Material 500 family as C_GREEN/C_RED */
#define C_BLUE    0x2196F3  /* added 2026-09-15 (Castle Defense, since removed); Slide Free block color now — Material 500, same family as the rest */
#define C_PURPLE  0x9C27B0  /* same — picked over pink specifically since pink reads too close to C_RED at a glance */
