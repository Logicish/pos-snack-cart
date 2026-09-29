#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- The transaction screen -- the whole checkout state machine as one file:
            item list (populated by scans) -> Manual Entry / Logout Confirm ->
            Payment. Cart is RAM-only; the DB is only written on Payment's
            "Complete + Logout".
*/

void screen_pos_push(int user_id);
// Consumes a scanned barcode as an item lookup while the TRANSACTION list is showing.
// Returns false (not consumed) when this screen isn't the active one, so main.cpp's
// on_scan() falls through to badge routing — same pattern as screen_enroll_on_scan().
bool screen_pos_on_scan(const char *upc);
