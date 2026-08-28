#pragma once

// "DB" — sub-menu under Advanced Tools, 2026-08-28. Lists the DB's tables (order matches
// screen_db_view.h's DbTable enum); picking one opens screen_db_view.cpp's generic
// read-only row browser for it. Diagnostic only, no editing. Back returns to Advanced
// Tools.
void screen_db_menu_push();
