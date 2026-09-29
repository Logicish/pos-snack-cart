#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Extras entry: a read-only view of the current Extras user's own outstanding
            balance (see screen_extras.h's badge gate) -- lists their unpaid checkouts
            with a disclaimer that admin hasn't verified them yet, and lets them re-view
            a transaction's Venmo QR in case a payment never actually landed the first
            time (hit Pay/Complete without the Venmo app actually going through).
*/

void screen_check_balance_push();
