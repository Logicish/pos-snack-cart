#pragma once

void header_init();
// Right side: current screen/menu name (e.g. "RESTOCK", "GM65 TEST").
void header_set_title(const char *title);
// Left side: whoever's currently identified on the device (badge holder or logged-in
// admin) — persists across screen changes until cleared (typically back at IDLE).
// Replaces the old static "MICU" branding text, 2026-08-26.
void header_set_current_user(const char *name);
void header_set_visible(bool visible);
