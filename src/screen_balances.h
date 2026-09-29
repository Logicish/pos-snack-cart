#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- "Balances" -- lists every outstanding checkout, Green clears one once
            the Venmo payment lands.
*/

// "Balances" — top-level Admin Menu entry, 2026-08-28. On-device equivalent of the web
// portal's /balance page: lists uncleared checkouts (checkouts.cleared_at IS NULL) one
// per transaction, since each Venmo payment note already carries its own transaction
// number ("#<id> Snack!") -- no need for per-item detail or multi-select, just highlight
// a transaction and clear it once the Venmo payment lands. Back returns to the main
// Admin Menu.
void screen_balances_push();
