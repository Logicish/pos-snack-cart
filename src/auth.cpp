#include "auth.h"
#include <mbedtls/sha256.h>
#include <esp_random.h>
#include <string.h>
#include <stdlib.h>

#define SALT_BYTES 16
#define HASH_BYTES 32

static void bytes_to_hex(const uint8_t *bytes, int len, char *out) {
    static const char hexchars[] = "0123456789abcdef";
    for (int i = 0; i < len; i++) {
        out[i * 2]     = hexchars[bytes[i] >> 4];
        out[i * 2 + 1] = hexchars[bytes[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static void hex_to_bytes(const char *hex, uint8_t *out, int max_len) {
    for (int i = 0; i < max_len; i++) {
        char pair[3] = { hex[i * 2], hex[i * 2 + 1], '\0' };
        out[i] = (uint8_t)strtol(pair, nullptr, 16);
    }
}

static void sha256_salted(const char *password, const uint8_t *salt, uint8_t *digest_out) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);  // 0 = SHA-256 (not the SHA-224 variant)
    mbedtls_sha256_update(&ctx, salt, SALT_BYTES);
    mbedtls_sha256_update(&ctx, (const unsigned char *)password, strlen(password));
    mbedtls_sha256_finish(&ctx, digest_out);
    mbedtls_sha256_free(&ctx);
}

void auth_hash_password(const char *password, char *hash_out, char *salt_out) {
    uint8_t salt[SALT_BYTES];
    for (int i = 0; i < SALT_BYTES; i++) salt[i] = (uint8_t)esp_random();

    uint8_t digest[HASH_BYTES];
    sha256_salted(password, salt, digest);

    bytes_to_hex(salt, SALT_BYTES, salt_out);
    bytes_to_hex(digest, HASH_BYTES, hash_out);
}

bool auth_verify_password(const char *password, const char *hash, const char *salt) {
    if (!password || !hash || !salt) return false;
    if (strlen(salt) != SALT_BYTES * 2 || strlen(hash) != HASH_BYTES * 2) return false;

    uint8_t salt_bytes[SALT_BYTES];
    hex_to_bytes(salt, salt_bytes, SALT_BYTES);

    uint8_t digest[HASH_BYTES];
    sha256_salted(password, salt_bytes, digest);

    char computed_hex[AUTH_HASH_HEX_LEN];
    bytes_to_hex(digest, HASH_BYTES, computed_hex);

    // Constant-time compare -- not critical for this threat model, but cheap to do right.
    uint8_t diff = 0;
    for (int i = 0; i < HASH_BYTES * 2; i++) diff |= (uint8_t)(computed_hex[i] ^ hash[i]);
    return diff == 0;
}
