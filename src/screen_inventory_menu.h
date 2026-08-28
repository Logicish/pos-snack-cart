#pragma once

// "Inventory Management" — sub-menu under the main Admin Menu, 2026-08-28 reorg. Wraps
// what used to be three separate top-level Admin Menu entries (Restock, Add/Attach Item,
// Inventory Count). Back here returns to the main Admin Menu, not a full logout.
void screen_inventory_menu_push();
