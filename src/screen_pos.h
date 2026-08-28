#pragma once

void screen_pos_push(int user_id);
// Consumes a scanned barcode as an item lookup while the TRANSACTION list is showing.
// Returns false (not consumed) when this screen isn't the active one, so main.cpp's
// on_scan() falls through to badge routing — same pattern as screen_enroll_on_scan().
bool screen_pos_on_scan(const char *upc);
