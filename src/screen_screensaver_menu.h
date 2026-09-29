#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- "Screensaver" submenu -- idle Timeout and backlight Dim level.
*/

// "Screensaver" — sub-menu under Settings, added 2026-09-14 when the old single-row
// "Screensaver: N min" action (cycled the timeout directly from the Settings list) grew
// into its own screen. Grew a second row (Dim level) the same day. Gives screensaver-
// related settings a real home to grow into later (e.g. if the fish animation or bubble
// behavior ever get tunables) without another Settings reshuffle. Back here returns to
// Settings.
void screen_screensaver_menu_push();
