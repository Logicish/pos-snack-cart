#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- "Browse/Price" -- scroll-first catalog list (Start screen's Right arrow),
            merged 2026-09-15 with what used to be a separate scan-first "Price Check"
            screen. No selection, just Page Up/Down -- scanning a UPC shows that item's
            price in a status line above the list without needing a second screen.
*/

void screen_browse_push();
// Consumes a scan while this screen is active, showing that item's price (or "not
// recognized") in the status line above the list. Same false-if-not-active pattern as
// every other screen's on_scan().
bool screen_browse_on_scan(const char *upc);
