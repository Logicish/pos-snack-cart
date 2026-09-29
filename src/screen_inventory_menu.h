#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- "Inventory" submenu -- Restock / Add-Attach Item / Inventory.
*/

// "Inventory Management" — sub-menu under the main Admin Menu, 2026-08-28 reorg. Wraps
// what used to be three separate top-level Admin Menu entries (Restock, Add/Attach Item,
// Inventory Count -- the third renamed to just "Inventory" on 2026-09-14 once it became
// a catch-all for UPC/item management, not just a quantity audit). Back here returns to
// the main Admin Menu, not a full logout.
void screen_inventory_menu_push();
