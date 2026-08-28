#pragma once

// "Price Check" (scan-first) — Start screen's Left arrow, 2026-08-26. Scan a product's
// UPC to see its price directly, no browsing/no badge needed. Complements the existing
// Right-arrow "Browse Items" (screen_browse_push()), which is scroll-first instead.
void screen_price_scan_push();
bool screen_price_scan_on_scan(const char *upc);
