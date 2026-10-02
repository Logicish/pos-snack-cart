#pragma once

/*
  Author--- LogicishDesigns
  Date----- October 2026
  Function- Boot log: one CSV line per boot appended to /boot_log.csv on the SD card --
            time, why the chip last restarted, and a details field of key=value results
            from each boot check (DB, SD write test, backup, ...).
  Notes---- Added 2026-10-02 for post-launch diagnosis ("it rebooted itself yesterday").
            Details are key=value pairs in one column rather than a column per check, so
            new checks can add notes without ever changing the file's columns. The file
            rotates to /boot_log_old.csv past BOOT_LOG_MAX_BYTES (~1700 boots), keeping one
            older file.
*/

#define BOOT_LOG_PATH "/boot_log.csv"

// Adds "key=value" to this boot's details (printf-style value). Safe to call any time
// before boot_log_write(); later calls are ignored once the line is written.
void boot_log_note(const char *key, const char *fmt, ...);

// Appends this boot's line to /boot_log.csv. Call once, at the end of setup(), after the
// RTC has synced the clock. A no-op without a working SD card.
void boot_log_write();

// Short name for why the chip last restarted (POWER_ON, BROWNOUT, CRASH, ...).
const char *boot_log_reset_reason();

// True if the last restart wasn't a normal power-on/reset/reflash -- a brownout, crash,
// or watchdog. Raises an Admin Menu alert (see system_alerts.h).
bool boot_log_unexpected_restart();
