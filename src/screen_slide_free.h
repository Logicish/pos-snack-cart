#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- "Slide Free" -- an Extras minigame, replaced Castle Defense 2026-09-30. A
            tilt puzzle: every press slides ALL blocks as far as they go; get the red
            block out through the exit gap. Boards load from the SD card
            (/slidefree/tier1.txt, tier2.txt, ...), generated and difficulty-graded
            offline by tools/tilt_lab.py. Score = boards solved in a run, saved as a
            per-user best via game_scores.h.
*/

void screen_slide_free_push();
