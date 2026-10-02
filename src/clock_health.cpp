#include "clock_health.h"
#include "rtc.h"
#include "db.h"
#include <sqlite3.h>
#include <Arduino.h>

/*
  Author--- LogicishDesigns
  Date----- October 2026
  Function- Implements the clock health check declared in clock_health.h.
*/

// A sale stamped a few seconds "in the future" relative to a fresh boot isn't a problem --
// only flag a clock that's clearly behind.
#define BEHIND_TOLERANCE_S 60

static int    _flags;
static time_t _last_sale;

// Newest checkout timestamp, or 0 if there are none (or no DB).
static time_t newest_sale() {
    sqlite3 *db = db_handle();
    if (!db) return 0;
    time_t t = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(created_at),0) FROM checkouts;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) t = (time_t)sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return t;
}

// Re-runs every check against the chip, the system clock, and the DB.
int clock_health_check() {
    int f = CLOCK_OK;
    if (!rtc_available())     f |= CLOCK_NO_RTC;
    else if (rtc_lost_power()) f |= CLOCK_LOST_POWER;

    time_t now = time(nullptr);
    struct tm tmval;
    gmtime_r(&now, &tmval);
    int year = tmval.tm_year + 1900;
    if (year < CLOCK_MIN_YEAR || year > CLOCK_MAX_YEAR) f |= CLOCK_BAD_YEAR;

    _last_sale = newest_sale();
    if (_last_sale > 0 && now + BEHIND_TOLERANCE_S < _last_sale) f |= CLOCK_BEHIND_LAST_SALE;

    _flags = f;
    return f;
}

int    clock_health_flags()    { return _flags; }
time_t clock_last_sale_time()  { return _last_sale; }

// Joins the active problems for the boot log, or "ok".
void clock_health_summary(char *out, size_t out_len) {
    if (_flags == CLOCK_OK) { snprintf(out, out_len, "ok"); return; }
    out[0] = '\0';
    auto add = [&](int bit, const char *name) {
        if (!(_flags & bit)) return;
        size_t len = strlen(out);
        snprintf(out + len, out_len - len, "%s%s", len ? "+" : "", name);
    };
    add(CLOCK_NO_RTC, "no_rtc");
    add(CLOCK_LOST_POWER, "lost_power");
    add(CLOCK_BAD_YEAR, "bad_year");
    add(CLOCK_BEHIND_LAST_SALE, "behind_last_sale");
}

// Most important problem first: a wrong clock matters more than a missing chip that the
// system clock may still be covering for this boot.
const char *clock_health_alert() {
    if (_flags & CLOCK_BAD_YEAR)         return "CLOCK IS WRONG - SET CLOCK";
    if (_flags & CLOCK_BEHIND_LAST_SALE) return "CLOCK IS BEHIND LAST SALE";
    if (_flags & CLOCK_LOST_POWER)       return "CLOCK BATTERY LOST POWER";
    if (_flags & CLOCK_NO_RTC)           return "CLOCK CHIP NOT FOUND";
    return nullptr;
}
