#include "system_alerts.h"
#include "db.h"
#include "clock_health.h"
#include <Arduino.h>
#include <SD.h>

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Implements the device-health banner declared in system_alerts.h -- tracks
            a handful of boolean conditions and picks the single most urgent one to
            show.
  Notes---- See system_alerts.h for the full reasoning.
*/

#define LOW_SPACE_WARN_PCT 5  // matches screen_sdinfo.cpp's own threshold

static bool _sd_missing;
static bool _sd_low_space;
static bool _db_unavailable;
static bool _sd_write_failed;   // set explicitly, see system_alerts_set_sd_write_result()
static bool _sd_write_known;    // false until the write test has actually run at least once
static bool _restored_from_backup;  // one-shot for this boot session
static char _restart_msg[48];      // one-shot for this boot session, "" if the restart was normal
#define MAX_BOOT_ISSUES 6
static const char *_boot_issues[MAX_BOOT_ISSUES];  // see system_alerts_add_boot_issue()
static int         _boot_issue_count;

// Priority order, most severe first -- only the single top active message is shown, to
// keep the banner one line. A count of any other active conditions is appended so nothing
// is silently hidden, just not spelled out in the limited space.
static const char *top_message(int *extra_count) {
    int active = 0;
    const char *first = nullptr;

    auto consider = [&](bool cond, const char *msg) {
        if (!cond) return;
        active++;
        if (!first) first = msg;
    };

    consider(_sd_missing,          "NO SD CARD DETECTED");
    consider(_db_unavailable,      "DATABASE UNAVAILABLE");
    consider(_restored_from_backup,"DB RESTORED FROM BACKUP THIS BOOT");
    consider(_restart_msg[0] != '\0', _restart_msg);
    consider(_sd_write_known && _sd_write_failed, "SD CARD NOT WRITABLE");
    consider(_sd_low_space,        "LOW SD CARD SPACE");
    const char *clock_msg = clock_health_alert();
    consider(clock_msg != nullptr, clock_msg);
    for (int i = 0; i < _boot_issue_count; i++) consider(true, _boot_issues[i]);

    *extra_count = active > 0 ? active - 1 : 0;
    return first;
}

// Recomputes every passive condition from scratch -- cheap, no SD writes.
void system_alerts_refresh() {
    uint8_t cardType = SD.cardType();
    _sd_missing = (cardType == CARD_NONE);

    if (!_sd_missing) {
        uint64_t usedMB  = SD.usedBytes()  / (1024ULL * 1024ULL);
        uint64_t totalMB = SD.totalBytes() / (1024ULL * 1024ULL);
        uint64_t freeMB  = (totalMB > usedMB) ? (totalMB - usedMB) : 0;
        _sd_low_space = totalMB > 0 && (freeMB * 100 / totalMB) < LOW_SPACE_WARN_PCT;
    } else {
        _sd_low_space = false;  // no card at all is already covered by _sd_missing
    }

    _db_unavailable = (db_handle() == nullptr);
    clock_health_check();  // re-asks the chip -- a Set Clock since boot clears it here
}

// Records the outcome of an SD write/read test run elsewhere (main.cpp at boot,
// screen_sdinfo.cpp's manual Check Write).
void system_alerts_set_sd_write_result(bool ok) {
    _sd_write_known  = true;
    _sd_write_failed = !ok;
}

// Marks this boot as having recovered the DB from a backup copy.
void system_alerts_note_restored_from_backup() {
    _restored_from_backup = true;
}

// Marks this boot as following an unexpected restart (brownout/crash/watchdog).
void system_alerts_note_unexpected_restart(const char *reason) {
    snprintf(_restart_msg, sizeof(_restart_msg), "UNEXPECTED RESTART: %s", reason);
}

// Records one boot-check finding (string literal) for this boot session.
void system_alerts_add_boot_issue(const char *msg) {
    // The SD error screen's Retry re-runs boot_try_init_db() -- don't stack repeats.
    for (int i = 0; i < _boot_issue_count; i++) {
        if (strcmp(_boot_issues[i], msg) == 0) return;
    }
    if (_boot_issue_count < MAX_BOOT_ISSUES) _boot_issues[_boot_issue_count++] = msg;
}

// True if at least one tracked condition is currently active.
bool system_alerts_active() {
    int extra;
    return top_message(&extra) != nullptr;
}

// Returns the single most urgent active message (with a "+N more" suffix if others are
// also active), or "" if nothing is active.
const char *system_alerts_message() {
    static char buf[64];
    int extra;
    const char *msg = top_message(&extra);
    if (!msg) return "";

    if (extra > 0) {
        snprintf(buf, sizeof(buf), "%s (+%d more)", msg, extra);
        return buf;
    }
    return msg;
}
