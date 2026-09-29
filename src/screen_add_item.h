#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- "Add/Attach Item" -- Inventory -> Add/Attach Item: scan a UPC, either
            create a brand-new catalog item or link the barcode to an existing one.
*/

// Admin Menu -> Add/Attach Item: scan a UPC. Already linked -> straight to the shared
// item-edit screen. Unknown -> choice of New Item (name entry, creates a fresh row) or Add
// to Existing SKU (catalog picker, links this UPC to an item that already exists -- this is
// how a second flavor's barcode gets attached to an already-created "package" item, e.g.
// Fig Bar's Blueberry UPC after Original was scanned in an earlier session).
void screen_add_item_push();
bool screen_add_item_on_scan(const char *upc);
