#pragma once
#include <stddef.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- GM65 "Settings" sub-screen -- decoded register view with live edit for
            Buzzer/LED/Frequency, reached from the Scanner screen.
*/

// GM65 "Settings" sub-screen, 2026-09-14 — reads registers 0x0000-0x000D raw, shows the
// hex dump at top, decodes the packed fields of 0x0000 (working mode, light, aim, buzzer,
// LED) plus the buzzer-frequency register 0x000A below it, and offers live edit+cycle for
// the fields this project actually has real, hardware-verified reason to change (buzzer
// on/off, LED indicator on/off, buzzer frequency). Working Mode/Light/Aim are shown
// read-only — Working Mode already has its own top-level Toggle Scan Mode control, and
// Light/Aim's non-default values (beyond Off/Normal) were only ever confirmed against the
// real manual text, not independently hardware-verified the way the rest of this
// project's locked values are — see the file's own header comment for the caveat.
void screen_gm65_settings_push();

// Same shape/purpose as screen_gm65_test_capture() — called from main.cpp's loop() for
// every raw line read off the scanner UART, before the normal badge/UPC dispatch. Returns
// true only when this screen is the one active, so a Read/Save reply's ACK doesn't get
// misrouted into on_scan() as a bogus badge scan.
bool screen_gm65_settings_capture(const char *data, size_t len);
