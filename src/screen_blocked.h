#pragma once

// Generic "can't start a transaction, see an admin" screen — used both for a real,
// enrolled-but-inactive user (see users.active in snack_cart_pos.md) and, as of 2026-08-25,
// an unrecognized badge (self-enrollment removed — owner's explicit "no guest checkout,
// admin-only enrollment" call; see Add User). Caller builds the exact message since the two
// cases say different things. Sends back to IDLE on any input.
void screen_blocked_push(const char *message);
