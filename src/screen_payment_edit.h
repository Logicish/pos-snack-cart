#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Generic "<Method> Payment Info" editor -- handle + owner-name wheel,
            shared by Venmo/Zelle/Cashapp against their own payment_methods row.
*/

// Generic "<Method> Payment Info" editor — generalized 2026-09-14 from what was originally
// a Venmo-only screen (screen_venmo_settings.cpp/.h), when Settings gained a Payment Info
// submenu listing Venmo/Zelle/Cashapp (screen_payment_menu.cpp). Same handle+owner-name
// wheel UI either way; edits whichever payment_methods row `method` names. `method` is the
// DB key (lowercase, e.g. "venmo"/"zelle"/"cashapp" — matches screen_pos.cpp's own method
// checks); `display_label` is what the header/hint text show (e.g. "Venmo"/"Zelle"/"Cashapp").
// Back (from the first field, or after Save) returns to the Payment Info submenu, not
// Settings directly — this is now one level deeper than it used to be.
void screen_payment_edit_push(const char *method, const char *display_label);
