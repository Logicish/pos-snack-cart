#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Auto-logout timer for admin/transaction screens, under Settings -> Security.
            The inverse default of idle_timer.h's screensaver timer: ARMED by default --
            buttons_set_handlers() arms it on every screen transition, since most screens
            represent an active session (a transaction in progress, an admin task) that
            shouldn't be left open indefinitely if walked away from. The handful of
            screens with nothing to log out of (IDLE, Browse/Price Check, the
            screensaver, splash) explicitly disarm it right after their own
            buttons_set_handlers() call.
  Notes---- On firing, forces screen_idle_load() -- the same "full logout" target every
            existing Back-to-logout path already uses (Admin Menu Back, POS's
            Logout/Cancel), so a mid-transaction cart is discarded with no DB write,
            exactly like walking through that path by hand would do. Added 2026-09-14.
            Timeout is configurable, range 1-10 minutes, default 5.
*/

void session_timer_reset();   // called on every button press -- pushes the deadline out
void session_timer_arm();     // called automatically by buttons_set_handlers()
void session_timer_disarm();  // call right after buttons_set_handlers() on screens with no session to log out of
void session_timer_check();   // call every loop() iteration -- forces a logout once the deadline passes

void session_timer_load_from_config();        // call once at boot, after db_init() -- loads the saved timeout
int  session_timer_get_minutes();             // returns the current timeout in minutes
void session_timer_set_minutes(int minutes);  // clamps/wraps into 1-10, persists to config
