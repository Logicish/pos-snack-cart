#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Shared "view/edit one item" screen -- the common piece behind Restock,
            Add/Attach Item's existing-item view, and Inventory Count.
*/

// Shared "view/edit one item" screen — the common piece behind Restock, Add/Attach Item's
// existing-item view, and Inventory's per-item screen. Up/Down move a selection cursor
// across Price / Qty +-1 / Qty +-10, then one row per linked UPC, then a Hide/Unhide Item
// row, then a trailing "Delete Item" row. Left/Right adjust whichever of the three fixed
// rows is selected (note: Left/Right don't auto-repeat on hold the way Up/Down do — see
// buttons.cpp — so adjusting through many steps takes individual presses); every
// adjustment writes to the DB immediately, no separate save step. Enter is
// context-sensitive: a no-op on the fixed rows, unlinks the selected UPC immediately on a
// UPC row, toggles hidden immediately on the Hide/Unhide row (2026-09-14 -- pulls the item
// from Browse/scanning without touching its row or checkout history, see items.h/db.cpp;
// reversible, so no confirm needed), or opens a confirm-gated delete on the trailing row
// (blocked automatically if the item has real checkout history). Scanning a new barcode
// while this screen is open links it to the item currently being viewed (see
// screen_item_edit_on_scan()) — this is how a second flavor's UPC gets attached to an
// already-existing SKU later, not just at first-create time.
void screen_item_edit_push(int item_id, void (*on_back)());

// Consumes a scanned barcode as "link this UPC to the item currently being viewed" while
// this screen is active. Same false-if-not-active pattern as screen_pos_on_scan().
bool screen_item_edit_on_scan(const char *upc);
