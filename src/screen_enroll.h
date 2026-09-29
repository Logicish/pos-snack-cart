#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Shared badge-scan -> name-wheel -> confirm -> re-scan-confirm enrollment
            widget, reached from Add User. Creates a non-admin user on success.
*/

void screen_enroll_push(const char *badge_id);  // begins enrollment for a badge already known to be unenrolled
bool screen_enroll_on_scan(const char *badge_id);  // consumes the re-scan-confirm step's matching scan
