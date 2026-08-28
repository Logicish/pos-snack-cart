#pragma once

// Idle-to-screensaver timer. Disarmed by default — buttons_set_handlers() disarms it
// on every screen transition, so only screens that explicitly re-arm (IDLE, Browse) can
// ever trigger the screensaver. Never fires mid-transaction/payment/admin work as a result.

void idle_timer_reset();   // called on every button press — pushes the deadline out
void idle_timer_arm();     // call right after buttons_set_handlers() on screens that allow idling
void idle_timer_disarm();  // called automatically by buttons_set_handlers()
void idle_timer_check();   // call every loop() iteration

// Configurable timeout, 2026-08-26 — was a hardcoded 30s. Range 1-10 minutes.
void idle_timer_load_from_config();  // call once at boot, after db_init()
int  idle_timer_get_minutes();
void idle_timer_set_minutes(int minutes);  // clamps/wraps into 1-10, persists to config
