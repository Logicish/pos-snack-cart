#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- "Security" submenu -- WiFi Password / Auto Logout.
*/

// "Security" — sub-menu under Settings, added 2026-09-14: the WiFi AP password (moved
// on-device, screen_wifi_password.cpp) and the auto-logout timeout (session_timer.h — force-returns to IDLE if a
// non-idle screen, e.g. mid-transaction or an admin task, sits untouched past this many
// minutes). Back here returns to Settings.
void screen_security_menu_push();
