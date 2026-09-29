#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- The fixed top status bar shown (or hidden) above every screen: current
            user/admin name on the left, current screen title on the right.
  Notes---- header_set_current_user() replaced the old static "MICU" branding text,
            2026-08-26.
*/

void header_init();  // call once in setup(), after lvgl_init()
// Right side: current screen/menu name (e.g. "RESTOCK", "GM65 TEST").
void header_set_title(const char *title);
// Left side: whoever's currently identified on the device (badge holder or logged-in
// admin) — persists across screen changes until cleared (typically back at IDLE).
void header_set_current_user(const char *name);
void header_set_visible(bool visible);  // shown by every screen except splash/screensaver
