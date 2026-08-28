#pragma once

// Admin Menu -> Inventory Count: walk the whole catalog in list order (not scan-first, like
// Restock/Add-Attach Item) and open each one in the shared item-edit screen. Doubles as
// first-run UPC verification (scan to attach a barcode to each entry) and a periodic
// physical audit (correct quantities to match what's actually still in the cart) -- same
// screen either way, see snack_cart_pos.md's 2026-08-25 planning round.
void screen_inventory_push();
