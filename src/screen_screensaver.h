#pragma once

// Screensaver: dancing fish (fixed 16-beat left/right flip pattern) that also random-
// walks within an invisible box, bubble particle stream from its mouth, bobbing
// "LogicishDesigns.com" wordmark + "press any key" prompt, backlight dim.
void screen_screensaver_push();

// True while the screensaver is the active screen — used to ignore scanner input while
// it's up (idle-timeout only arms from IDLE/Browse, so waking is deliberately button-only,
// not scan-triggered; see snack_cart_pos.md's 2026-08-25 planning round for the reasoning).
bool screen_screensaver_is_active();
