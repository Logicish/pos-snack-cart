#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- "Payment Info" submenu -- Venmo / Zelle / Cashapp, each opening the same
            generic editor (screen_payment_edit.h).
*/

// "Payment Info" — sub-menu under Settings, added 2026-09-14 when the old single-entry
// "Venmo Payment Info" row grew into a menu of payment methods (Venmo/Zelle/Cashapp), each
// opening the same generic editor (screen_payment_edit.h) against its own payment_methods
// row. Back here returns to Settings.
void screen_payment_menu_push();
