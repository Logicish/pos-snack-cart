#pragma once

// Admin Menu -> Restock: scan an item, adjust its price/quantity, scan the next one --
// stays in restock mode until Back is pressed from the scan-prompt itself, so an admin can
// work through a whole cart's worth of items without re-navigating the menu each time.
void screen_restock_push();
bool screen_restock_on_scan(const char *upc);
