#pragma once

// One row per checkout ("Finish" → Payment "Complete + Logout"), NOT per item scan —
// see snack_cart_pos.md's DB schema section for why. A cancelled/logged-out transaction
// never reaches this module at all — the cart only exists in RAM until checkout_save()
// commits it.

struct CheckoutLine {
    int item_id;
    int price_cents;  // snapshot at time of purchase, independent of items.price_cents
    int quantity;
};

// Wraps the checkouts + checkout_items inserts in one BEGIN/COMMIT. Returns the new
// checkouts.id (the transaction number) on success, -1 on failure (nothing partially
// written — COMMIT only runs if every insert in the batch succeeded).
int checkout_save(int user_id, const CheckoutLine *lines, int line_count, int total_cents);

// UI display cap for screen_balances.cpp's row pool, not a DB storage limit — same
// convention as items.h's MAX_ITEMS. Outstanding balances should stay small in practice
// (an admin clears them as Venmo payments land), so this starts conservative.
#define MAX_BALANCES 64

struct OutstandingCheckout {
    int  id;                 // checkouts.id -- the same number the Venmo note carries
    int  user_id;
    int  total_price_cents;
    long created_at;         // 0 = never set (no RTC/NTP yet) -- see checkouts.cpp
};

// Fills out[] with up to `max` uncleared checkouts, oldest first. Returns how many were
// written.
int checkouts_get_outstanding(OutstandingCheckout *out, int max);

// Marks one checkout cleared (cleared_at = now). Returns false if it doesn't exist or was
// already cleared -- never deletes the row, same reasoning as checkouts.cleared_at's
// schema comment in db.cpp (lifetime totals and outstanding balances stay the same data).
bool checkouts_clear(int checkout_id);

// Sum of total_price_cents across one user's uncleared checkouts -- the same "outstanding
// balance" figure the web /balance page and the Balances screen show, just filtered to
// one person instead of listed transaction-by-transaction. Used by Edit Users' detail
// screen (read-only display, 2026-08-28 -- see project memory).
int checkouts_get_balance_cents(int user_id);
