#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- The Admin Menu -- top level of admin navigation, reached only through
            Admin Login. Six task-based sections: Balances / Inventory / Users /
            Web Portal / Settings / Advanced Tools, ordered most-to-least used.
  Notes---- Was a flat list of individual actions until the 2026-08-28 reorg
            regrouped them into these submenus once the list was about to overflow.
            Back here always logs out fully (this is the top level of admin nav).
*/
void screen_menu_push();
