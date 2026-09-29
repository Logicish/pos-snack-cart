#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Extras entry: "Castle Defense" -- an archer on a castle wall shoots two
            loaded arrow colors (Up/Down) at a fully-packed grid of colored blocks
            above, moving column to column (Left/Right). A hit clears the whole
            connected same-color group (flood fill); the rest of that column's blocks
            then fall to fill the gap -- but nothing auto-clears from that fall, only a
            direct shot ever pops a group, so a smart shot sets up a bigger one next
            time. Clearing the whole board wins the stage and adds a color to the next
            one; getting stuck with two loaded colors that don't exist anywhere on the
            board anymore is a real, intended loss.
*/

void screen_castle_defense_push();
