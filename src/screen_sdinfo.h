#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- "SD Info" -- SD card status, used/free space, DB status, and a root
            directory listing, plus manual Retry/Check Write actions.
*/

// Dev/diagnostic screen — SD card status, DB status, and a root directory listing.
// Rebuilt 2026-08-24 after the original "DB Check" screen was deleted as no-longer-needed
// — turned out still needed once real hardware surfaced an actual SD mount failure.
void screen_sdinfo_push();
