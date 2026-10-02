#pragma once

/*
  Author--- LogicishDesigns
  Date----- October 2026
  Function- "Boot Log" -- Advanced Tools viewer for /boot_log.csv: one boot per page,
            newest first, every boot check on its own line with failures in red and
            warnings in orange.
  Notes---- Added 2026-10-02. Left = older boot, Right = newer (both wrap), Up/Down
            scroll a long entry, Back returns to Advanced Tools. Indexes the newest
            BOOT_LOG_VIEW_MAX lines each time it opens, so it never loads the whole file
            (which can grow to ~512 KB before boot_log.cpp rotates it).
*/

void screen_boot_log_push();
