#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Idle-to-screensaver timer. Counts down while armed and pushes the device
            into the screensaver when the deadline passes. Disarmed by default --
            buttons_set_handlers() disarms it on every screen change, so only screens
            that explicitly re-arm it (IDLE, Browse) can ever trigger the screensaver.
            Never fires mid-transaction/payment/admin work as a result.
  Notes---- Timeout is configurable (1-10 min, default 3 -- see idle_timer.cpp), set
            from Settings -> Screensaver and persisted to the config table so it
            survives a reboot. Was a hardcoded 30s before 2026-08-26.
*/

void idle_timer_reset();   // called on every button press -- pushes the deadline out
void idle_timer_arm();     // call right after buttons_set_handlers() on screens that allow idling
void idle_timer_disarm();  // called automatically by buttons_set_handlers()
void idle_timer_check();   // call every loop() iteration -- fires the screensaver once the deadline passes

void idle_timer_load_from_config();        // call once at boot, after db_init() -- loads the saved timeout
int  idle_timer_get_minutes();             // returns the current timeout in minutes
void idle_timer_set_minutes(int minutes);  // clamps/wraps into 1-10, persists to config
