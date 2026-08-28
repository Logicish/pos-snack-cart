#pragma once

// Shared "view/edit one item" screen — the common piece behind Restock, Add/Attach Item's
// existing-item view, and Inventory Count's per-item screen. Up/Down move a selection
// cursor between three rows (Price / Qty +-1 / Qty +-10); Left/Right adjust whichever row
// is selected (note: Left/Right don't auto-repeat on hold the way Up/Down do — see
// buttons.cpp — so adjusting through many steps takes individual presses). Every
// adjustment writes to the DB immediately, no separate save step. Scanning a new barcode
// while this screen is open links it to the item currently being viewed (see
// screen_item_edit_on_scan()) — this is how a second flavor's UPC gets attached to an
// already-existing SKU later, not just at first-create time.
void screen_item_edit_push(int item_id, void (*on_back)());

// Consumes a scanned barcode as "link this UPC to the item currently being viewed" while
// this screen is active. Same false-if-not-active pattern as screen_pos_on_scan().
bool screen_item_edit_on_scan(const char *upc);
