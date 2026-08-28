#pragma once

// ── layout ────────────────────────────────────────────────────────────────────
#define SCREEN_W  320
#define SCREEN_H  480
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
