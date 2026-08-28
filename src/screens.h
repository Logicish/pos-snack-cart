#pragma once

// Navigation — implemented across screen_*.cpp and main.cpp
void screen_idle_load();
void screen_enroll_push(const char *badge_id);
void screen_pos_push(int user_id);
void screen_browse_push();
void screen_menu_push();
void screen_splash_push();
void screen_screensaver_push();
void screen_restock_push();
void screen_add_item_push();
void screen_inventory_push();
void screen_add_user_push();
void screen_gm65_test_push();

// 2026-08-28 Admin Menu reorg — these two submenus are the "Back" target for several
// existing screens (Restock/Add-Attach/Inventory Count now return to Inventory Management,
// not straight to the top-level Admin Menu; Add User now returns to User Management), so
// they need to be reachable from those files too, not just from screen_menu.cpp.
void screen_inventory_menu_push();
void screen_user_menu_push();
