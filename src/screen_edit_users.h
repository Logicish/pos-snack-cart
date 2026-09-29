#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- "Edit Users" -- alphabetical list -> per-user detail view (Name/Admin/
            Locked, plus a read-only computed balance).
*/

// "Edit Users" — built out 2026-08-28. List all users (alphabetical, admin/locked tagged)
// -> highlight one, Enter opens a detail view (name, read-only computed balance, admin
// flag, locked flag) with Name/Admin/Locked as three actionable rows: Name opens a
// first/last name wheel (same widget Add User's enrollment uses), Admin/Locked toggle in
// place. Back from the detail view returns to the list; Back from the list returns to
// User Management.
void screen_edit_users_push();
