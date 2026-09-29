#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- The product catalog: item records (name/price/stock) plus the UPC-to-item
            linking table that lets one item be sold under several real barcodes.
  Notes---- MAX_ITEMS is a UI display cap for screen_browse.cpp's/screen_pos.cpp's
            fixed row pools, NOT a DB storage limit — the items table itself can hold
            far more than this. Bumped from 32 to 64 on 2026-08-25 after the real
            53-item catalog exceeded the old cap and crashed the cursor-wraparound
            math in both list screens (see their visible_count() helpers), then to
            255 same day with room for the catalog to grow into the low 100s over
            time without needing another bump — cheap to do since every LVGL row
            object these screens build comes out of PSRAM (see lv_conf.h's
            LV_MEM_CUSTOM_ALLOC), not the small internal RAM budget.
*/
#define MAX_ITEMS     255
#define ITEM_NAME_LEN 32

struct Item {
    int  id;
    char name[ITEM_NAME_LEN];
    int  price_cents;
    // Decremented per-sale by checkout_save() (src/checkouts.cpp) as of 2026-08-24, so it
    // tracks real remaining stock without the owner recounting inventory. Deliberately
    // allowed to go negative — SQLite INTEGER has no unsigned variant anyway — rather than
    // clamped at 0: if real sales outpace what's logged, staying negative preserves the
    // "we're actually N below what we thought" signal instead of silently losing it once
    // the count would've hit zero.
    int  stocked;
    // Pulled from customer-facing Browse/scanning but not deleted -- see db.cpp's items.hidden
    // comment. items_get()/items_count() skip hidden items when include_hidden=false;
    // items_get_by_id() never filters (Item Edit needs to load a hidden item to show/toggle
    // it), and items_find_by_upc() ALWAYS filters (a hidden item's barcode reads as
    // unrecognized at checkout/Price Check/Restock/Add-Attach's scan-to-open).
    bool hidden;
};

void items_init();  // seeds a small hardcoded starter catalog if the table is empty
// Total row count in the items table. include_hidden=false counts only visible items --
// pass that from any customer-facing/checkout-time list (Browse, POS Manual Entry); leave
// it true (default) for admin catalog-management lists (Inventory, Add/Attach's picker),
// which need to keep reaching hidden items to restock/unhide/edit them.
int  items_count(bool include_hidden = true);
// Pointer is to a static internal buffer — valid until the next items_get()/
// items_get_by_id()/items_find_by_upc() call. Alphabetical order, see items.cpp.
// include_hidden works the same as items_count() above -- pass the SAME value to both
// calls in a given list, or the index math won't line up with what's actually being counted.
const Item *items_get(int index, bool include_hidden = true);  // nullptr if out of range
const Item *items_get_by_id(int item_id);  // nullptr if not found -- never filters on hidden
const Item *items_find_by_upc(const char *upc);  // nullptr if not found OR the item is hidden

bool items_update(int item_id, int price_cents, int stocked);  // used by Restock/Inventory
bool items_set_hidden(int item_id, bool hidden);  // Hide/Unhide Item, from Item Edit
int  items_create(const char *name, int price_cents, int stocked);  // returns new id, -1 on failure

// One item, many UPCs (a "package" sold under several barcodes — see db.cpp's item_upcs
// comment for the Fig Bar variety-pack reasoning this was built for).
#define UPC_LEN          24
#define MAX_UPCS_PER_ITEM 8

enum ItemUpcLinkResult {
    ITEM_UPC_LINKED,        // newly linked to item_id
    ITEM_UPC_ALREADY_HERE,  // already linked to this same item_id — harmless no-op
    ITEM_UPC_COLLISION,     // already linked to a DIFFERENT item — caller should look that
                             // item up via items_find_by_upc(upc) if it wants to name it
    ITEM_UPC_ERROR,
};
ItemUpcLinkResult item_upcs_link(int item_id, const char *upc);  // links a UPC to an item, see enum above
// Fills out[] with up to `max` UPCs linked to item_id, returns how many were written.
int items_get_upcs(int item_id, char out[][UPC_LEN], int max);
bool item_upcs_unlink(int item_id, const char *upc);  // removes one UPC link, true if a row was removed

// Deletes an item and its UPC links. Fails (returns false, deletes nothing) if the item
// has real checkout_items history -- foreign_keys=ON with no CASCADE, deliberately not
// worked around, same reasoning as blocking a user delete with existing checkouts.
bool items_delete(int item_id);
