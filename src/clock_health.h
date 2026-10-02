#pragma once

#include <time.h>

/*
  Author--- LogicishDesigns
  Date----- October 2026
  Function- Clock health check: is the time the cart is stamping onto every sale actually
            believable? Combines the DS3231's own lost-power flag with two sanity checks
            on the system clock -- an impossible year, and a clock that's earlier than the
            newest sale on record (time went backwards).
  Notes---- Added 2026-10-02. Run at boot (boot log + Admin Menu alert), again whenever
            the Admin Menu refreshes its alerts, and live on Advanced Tools -> DS3231 Test.
            Kept separate from rtc.cpp so the clock driver itself never depends on the DB.
            Every problem here is fixed the same way: Settings -> Set Clock (which also
            writes the chip and clears its lost-power flag).
*/

enum ClockHealthFlag {
    CLOCK_OK               = 0,
    CLOCK_NO_RTC           = 1 << 0,  // DS3231 not answering on I2C
    CLOCK_LOST_POWER       = 1 << 1,  // DS3231's coin cell died (or chip never set) since last set
    CLOCK_BAD_YEAR         = 1 << 2,  // system year outside CLOCK_MIN_YEAR..CLOCK_MAX_YEAR
    CLOCK_BEHIND_LAST_SALE = 1 << 3,  // system time earlier than the newest checkout
};

#define CLOCK_MIN_YEAR 2026
#define CLOCK_MAX_YEAR 2099

int  clock_health_check();          // re-runs every check now; returns ClockHealthFlag bits
int  clock_health_flags();          // the last result, without re-checking
time_t clock_last_sale_time();      // newest checkouts.created_at seen by the last check, 0 if none

// "ok", or the problems joined with '+' (e.g. "lost_power+behind_last_sale") -- boot log format.
void clock_health_summary(char *out, size_t out_len);

// One short, all-caps alert line for the most important problem, or nullptr if healthy.
const char *clock_health_alert();
