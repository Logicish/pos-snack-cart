#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Device-health warnings surfaced as a yellow banner at the top of the Admin
            Menu. Tracks a small set of passive conditions (SD card missing/low space,
            DB unavailable, restored-from-backup-this-boot, SD not writable) and
            reports the single most urgent one, plus a count of anything else active.
  Notes---- Added 2026-09-14 -- the "alert-strip" component sketched in the 2026-08-25
            planning round (originally scoped for a default-password reminder; that use
            case ended up as a web-only forced redirect instead, so this never got
            built) finally landed, generalized from the start the same way that plan
            intended: cheap to add a new tracked condition later. Deliberately checks
            only cheap, passive, already-proven primitives (SD.cardType(),
            usedBytes()/totalBytes(), db_handle()) on every refresh -- it does NOT
            re-run the SD write/read test itself (that writes to the card; see
            system_alerts_set_sd_write_result() below), so opening the Admin Menu
            never causes an extra card write on its own.
*/

void system_alerts_refresh();  // recomputes every passive-check condition -- call at boot and whenever the Admin Menu is (re)opened

// The SD write/read test result is set explicitly by whoever last ran it (main.cpp at
// boot, screen_sdinfo.cpp's manual Check Write) rather than re-run here.
void system_alerts_set_sd_write_result(bool ok);

// One-shot note for this boot session only, not persisted -- lets the owner know the
// device quietly fixed itself, since restoring from backup means real (if likely small)
// data loss for anything written since the last backup.
void system_alerts_note_restored_from_backup();

// One-shot for this boot session, 2026-10-02 -- the chip last restarted from a brownout,
// crash, or watchdog rather than a normal power-on (reason is boot_log_reset_reason()).
// Details for every boot are in /boot_log.csv.
void system_alerts_note_unexpected_restart(const char *reason);

// Boot-check findings, 2026-10-02 -- scanner, stuck button, data consistency, missing
// game files. One-shot for this boot session, up to 6, shown in the order added after
// the built-in conditions above. `msg` must be a string literal (stored by pointer).
void system_alerts_add_boot_issue(const char *msg);

bool        system_alerts_active();   // true if at least one warning is currently active
const char *system_alerts_message();  // the single most urgent active message, or "" if none
