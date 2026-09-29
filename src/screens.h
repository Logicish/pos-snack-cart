#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Cross-cutting screen navigation declarations -- the push functions
            screens call on EACH OTHER (as opposed to a screen's own push function,
            declared in its own screen_*.h and called only from wherever it's
            entered). Implemented across screen_*.cpp and main.cpp.
*/

void screen_idle_load();               // the Start screen -- main.cpp
void screen_enroll_push(const char *badge_id);  // name-entry flow for a new badge
void screen_pos_push(int user_id);     // the transaction/checkout screen
void screen_browse_push();             // read-only catalog browse
void screen_menu_push();               // the Admin Menu
void screen_splash_push();             // boot splash, falls through to IDLE
void screen_screensaver_push();        // idle-timeout screensaver
void screen_restock_push();            // scan-to-restock
void screen_add_item_push();           // scan-to-add/attach a catalog item
void screen_inventory_push();          // Inventory list
void screen_add_user_push();           // scan-to-enroll a new person
void screen_gm65_test_push();          // Advanced Tools -> Scanner
void screen_extras_push();             // Start screen's Left arrow -- badge-gated menu of
                                        // less-common, non-POS entries
void screen_extras_return_to_list();   // Extras sub-screens' Back target -- re-shows the
                                        // already-identified menu without a re-scan
void screen_laggy_fish_push();         // Extras entry: the minigame
void screen_castle_defense_push();     // Extras entry: the archer-vs-color-horde minigame
void screen_check_balance_push();      // Extras entry: a user's own outstanding balance

// 2026-08-28 Admin Menu reorg — these two submenus are the "Back" target for several
// existing screens (Restock/Add-Attach/Inventory Count now return to Inventory Management,
// not straight to the top-level Admin Menu; Add User now returns to User Management), so
// they need to be reachable from those files too, not just from screen_menu.cpp.
void screen_inventory_menu_push();
void screen_user_menu_push();

// Not a screen push itself, but main.cpp's exported boot-DB-init sequence (db_init() +
// backup self-heal + SD write test + items/users init + a fresh backup), added 2026-09-14
// so screen_sd_error.cpp's Retry button can re-run the exact same sequence setup() runs,
// rather than duplicating it. Returns true if the DB ended up usable.
bool boot_try_init_db();
void screen_sd_error_push();

// First-boot / DB-recovery setup wizard, 2026-09-14 — see screen_setup_wizard.h. Runs
// instead of screen_splash_push() when boot_try_init_db() succeeds but no admin exists.
void screen_setup_wizard_push();
