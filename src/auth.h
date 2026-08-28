#pragma once

// Salted SHA-256 password hashing for admin web login. Deliberately not bcrypt/Argon2 —
// those are slow-by-design to resist offline cracking of a leaked hash database at scale,
// which isn't this device's threat model (a local AP, on rarely and briefly, in a
// physically secured building, 2-3 admin accounts). mbedTLS's SHA-256 is already bundled
// in the ESP32 Arduino core, so this needs no new dependency. See snack_cart_pos.md's
// 2026-08-25 planning round for the full reasoning.

#define AUTH_HASH_HEX_LEN 65  // 32-byte SHA-256 digest as hex, +1 for NUL
#define AUTH_SALT_HEX_LEN 33  // 16-byte salt as hex, +1 for NUL

// Hashes `password` with a freshly-generated random salt. hash_out must be at least
// AUTH_HASH_HEX_LEN bytes, salt_out at least AUTH_SALT_HEX_LEN.
void auth_hash_password(const char *password, char *hash_out, char *salt_out);

// Re-hashes `password` with the given salt (hex string) and compares to `hash` (hex string).
bool auth_verify_password(const char *password, const char *hash, const char *salt);
