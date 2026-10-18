/**
 * SwitchAirCast - crypto backend for the UxPlay AirPlay library
 *
 * Drop-in replacement for UxPlay's OpenSSL-based crypto.c, using
 * mbedtls (AES, GCM, SHA-512, MD5, base64) and libsodium (X25519, Ed25519),
 * both of which are available as devkitPro portlibs for the Nintendo Switch.
 *
 * Based on crypto.c from RPiPlay/UxPlay:
 * Copyright (C) 2019 Florian Draschbacher
 * Copyright (C) 2020 Jaslo Ziska
 * modified by fduncanh 2021-2022
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "crypto.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha512.h>
#include <sodium.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "utils.h"

#define SALT_PK "UxPlay-Persistent-Not-Secure-Public-Key"
#define SHA512_DIGEST_LENGTH 64
#define MD5_DIGEST_LENGTH 16

typedef enum { AES_MODE_CTR, AES_MODE_CBC } aes_mode_t;

struct aes_ctx_s {
    mbedtls_aes_context aes;
    aes_mode_t mode;
    aes_direction_t direction;
    uint8_t key[AES_128_BLOCK_SIZE];
    uint8_t iv[AES_128_BLOCK_SIZE];
    /* running state: CTR counter / CBC chaining vector */
    uint8_t state[AES_128_BLOCK_SIZE];
    /* CTR keystream buffer and position inside it */
    uint8_t stream_block[AES_128_BLOCK_SIZE];
    size_t nc_off;
    uint8_t block_offset;
};

static uint8_t waste[AES_128_BLOCK_SIZE];

// Random numbers and library init

#ifdef __SWITCH__
static const char *switch_random_name(void) { return "switch-csrng"; }
static uint32_t switch_random(void) {
    uint32_t v;
    randomGet(&v, sizeof(v));
    return v;
}
static void switch_random_buf(void *const buf, const size_t size) { randomGet(buf, size); }
static randombytes_implementation switch_randombytes = {
    .implementation_name = switch_random_name,
    .random = switch_random,
    .buf = switch_random_buf,
};
#endif

static void crypto_lib_init(void) {
    static bool initialized = false;
    if (initialized) return;
#ifdef __SWITCH__
    /* libsodium's default RNG wants /dev/urandom, which Horizon does not have */
    randombytes_set_implementation(&switch_randombytes);
#endif
    if (sodium_init() < 0) {
        fprintf(stderr, "crypto: sodium_init failed\n");
    }
    initialized = true;
}

int get_random_bytes(unsigned char *buf, int num) {
    if (num <= 0) return 1;
#ifdef __SWITCH__
    randomGet(buf, (size_t) num);
#else
    crypto_lib_init();
    randombytes_buf(buf, (size_t) num);
#endif
    return 1;
}

// Common AES utilities

static aes_ctx_t *aes_init(const uint8_t *key, const uint8_t *iv, aes_mode_t mode, aes_direction_t direction) {
    aes_ctx_t *ctx = calloc(1, sizeof(aes_ctx_t));
    assert(ctx != NULL);
    mbedtls_aes_init(&ctx->aes);
    ctx->mode = mode;
    ctx->direction = direction;
    memcpy(ctx->key, key, AES_128_BLOCK_SIZE);
    memcpy(ctx->iv, iv, AES_128_BLOCK_SIZE);
    memcpy(ctx->state, iv, AES_128_BLOCK_SIZE);
    /* CTR mode always runs the block cipher forwards */
    if (mode == AES_MODE_CBC && direction == AES_DECRYPT) {
        mbedtls_aes_setkey_dec(&ctx->aes, key, 128);
    } else {
        mbedtls_aes_setkey_enc(&ctx->aes, key, 128);
    }
    return ctx;
}

static void aes_restart(aes_ctx_t *ctx) {
    memcpy(ctx->state, ctx->iv, AES_128_BLOCK_SIZE);
    memset(ctx->stream_block, 0, AES_128_BLOCK_SIZE);
    ctx->nc_off = 0;
}

static void aes_destroy(aes_ctx_t *ctx) {
    if (ctx) {
        mbedtls_aes_free(&ctx->aes);
        free(ctx);
    }
}

// AES CTR

aes_ctx_t *aes_ctr_init(const uint8_t *key, const uint8_t *iv) {
    return aes_init(key, iv, AES_MODE_CTR, AES_ENCRYPT);
}

static void aes_ctr_crypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len) {
    if (len <= 0) return;
    mbedtls_aes_crypt_ctr(&ctx->aes, (size_t) len, &ctx->nc_off, ctx->state, ctx->stream_block, in, out);
}

void aes_ctr_encrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len) {
    aes_ctr_crypt(ctx, in, out, len);
    ctx->block_offset = (ctx->block_offset + len) % AES_128_BLOCK_SIZE;
}

void aes_ctr_start_fresh_block(aes_ctx_t *ctx) {
    if (ctx->block_offset == 0) return;
    aes_ctr_encrypt(ctx, waste, waste, AES_128_BLOCK_SIZE - ctx->block_offset);
}

void aes_ctr_decrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len) {
    aes_ctr_crypt(ctx, in, out, len);
}

void aes_ctr_reset(aes_ctx_t *ctx) {
    aes_restart(ctx);
}

void aes_ctr_destroy(aes_ctx_t *ctx) {
    aes_destroy(ctx);
}

// AES CBC (no padding: only whole blocks are processed, as with OpenSSL padding disabled)

aes_ctx_t *aes_cbc_init(const uint8_t *key, const uint8_t *iv, aes_direction_t direction) {
    return aes_init(key, iv, AES_MODE_CBC, direction);
}

void aes_cbc_encrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len) {
    assert(ctx->direction == AES_ENCRYPT);
    len -= len % AES_128_BLOCK_SIZE;
    if (len <= 0) return;
    mbedtls_aes_crypt_cbc(&ctx->aes, MBEDTLS_AES_ENCRYPT, (size_t) len, ctx->state, in, out);
}

void aes_cbc_decrypt(aes_ctx_t *ctx, const uint8_t *in, uint8_t *out, int len) {
    assert(ctx->direction == AES_DECRYPT);
    len -= len % AES_128_BLOCK_SIZE;
    if (len <= 0) return;
    mbedtls_aes_crypt_cbc(&ctx->aes, MBEDTLS_AES_DECRYPT, (size_t) len, ctx->state, in, out);
}

void aes_cbc_reset(aes_ctx_t *ctx) {
    aes_restart(ctx);
}

void aes_cbc_destroy(aes_ctx_t *ctx) {
    aes_destroy(ctx);
}

// X25519

struct x25519_key_s {
    unsigned char pub[X25519_KEY_SIZE];
    unsigned char priv[X25519_KEY_SIZE];
    bool has_priv;
};

x25519_key_t *x25519_key_generate(void) {
    crypto_lib_init();
    x25519_key_t *key = (x25519_key_t *) calloc(1, sizeof(x25519_key_t));
    assert(key);
    get_random_bytes(key->priv, X25519_KEY_SIZE);
    crypto_scalarmult_curve25519_base(key->pub, key->priv);
    key->has_priv = true;
    return key;
}

x25519_key_t *x25519_key_from_raw(const unsigned char data[X25519_KEY_SIZE]) {
    x25519_key_t *key = (x25519_key_t *) calloc(1, sizeof(x25519_key_t));
    assert(key);
    memcpy(key->pub, data, X25519_KEY_SIZE);
    return key;
}

void x25519_key_get_raw(unsigned char data[X25519_KEY_SIZE], const x25519_key_t *key) {
    assert(key);
    memcpy(data, key->pub, X25519_KEY_SIZE);
}

void x25519_key_destroy(x25519_key_t *key) {
    if (key) {
        sodium_memzero(key, sizeof(*key));
        free(key);
    }
}

void x25519_derive_secret(unsigned char secret[X25519_KEY_SIZE], const x25519_key_t *ours, const x25519_key_t *theirs) {
    assert(ours);
    assert(theirs);
    assert(ours->has_priv);
    crypto_lib_init();
    if (crypto_scalarmult_curve25519(secret, ours->priv, theirs->pub) != 0) {
        /* peer sent a low-order point; leave an all-zero secret so the handshake fails */
        memset(secret, 0, X25519_KEY_SIZE);
    }
}

// GCM AES 128 (16-byte IV, 16-byte tag, no AAD)

int gcm_encrypt(const unsigned char *plaintext, int plaintext_len, unsigned char *ciphertext,
                unsigned char *key, unsigned char *iv, unsigned char *tag) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (ret == 0) {
        ret = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, (size_t) plaintext_len, iv, 16,
                                        NULL, 0, plaintext, ciphertext, 16, tag);
    }
    mbedtls_gcm_free(&gcm);
    return ret == 0 ? plaintext_len : -1;
}

int gcm_decrypt(unsigned char *ciphertext, int ciphertext_len, unsigned char *plaintext,
                unsigned char *key, unsigned char *iv, unsigned char *tag) {
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 128);
    if (ret == 0) {
        ret = mbedtls_gcm_auth_decrypt(&gcm, (size_t) ciphertext_len, iv, 16, NULL, 0, tag, 16,
                                       ciphertext, plaintext);
    }
    mbedtls_gcm_free(&gcm);
    return ret == 0 ? ciphertext_len : -1;
}

// ED25519

struct ed25519_key_s {
    unsigned char pk[crypto_sign_ed25519_PUBLICKEYBYTES];
    unsigned char sk[crypto_sign_ed25519_SECRETKEYBYTES];
    bool has_sk;
};

/* keyfile format: the raw 32-byte Ed25519 seed */
static bool read_seed_file(const char *keyfile, unsigned char seed[ED25519_KEY_SIZE]) {
    FILE *file = fopen(keyfile, "rb");
    if (!file) return false;
    size_t n = fread(seed, 1, ED25519_KEY_SIZE, file);
    fclose(file);
    return n == ED25519_KEY_SIZE;
}

static bool write_seed_file(const char *keyfile, const unsigned char seed[ED25519_KEY_SIZE]) {
    FILE *file = fopen(keyfile, "wb");
    if (!file) return false;
    size_t n = fwrite(seed, 1, ED25519_KEY_SIZE, file);
    fclose(file);
    return n == ED25519_KEY_SIZE;
}

ed25519_key_t *ed25519_key_generate(const char *device_id, const char *keyfile, int *result) {
    unsigned char seed[ED25519_KEY_SIZE] = {0};
    bool use_keyfile = keyfile && strlen(keyfile);

    crypto_lib_init();
    *result = 0;

    ed25519_key_t *key = (ed25519_key_t *) calloc(1, sizeof(ed25519_key_t));
    assert(key);

    if (use_keyfile) {
        if (!read_seed_file(keyfile, seed)) {
            get_random_bytes(seed, ED25519_KEY_SIZE);
            if (write_seed_file(keyfile, seed)) {
                *result = 1;
            }
        }
    } else {
        /* generate (insecure) persistent keypair using device_id */
        unsigned char hash[SHA512_DIGEST_LENGTH] = {0};
        char salt[] = SALT_PK;
        sha_ctx_t *ctx = sha_init();
        sha_update(ctx, (const unsigned char *) salt, (unsigned int) strlen(salt));
        sha_update(ctx, (const unsigned char *) device_id, (unsigned int) strlen(device_id));
        sha_final(ctx, hash, NULL);
        sha_destroy(ctx);
        memcpy(seed, hash, ED25519_KEY_SIZE);
    }

    crypto_sign_ed25519_seed_keypair(key->pk, key->sk, seed);
    key->has_sk = true;
    sodium_memzero(seed, sizeof(seed));
    return key;
}

ed25519_key_t *ed25519_key_from_raw(const unsigned char data[ED25519_KEY_SIZE]) {
    ed25519_key_t *key = (ed25519_key_t *) calloc(1, sizeof(ed25519_key_t));
    assert(key);
    memcpy(key->pk, data, ED25519_KEY_SIZE);
    return key;
}

void ed25519_key_get_raw(unsigned char data[ED25519_KEY_SIZE], const ed25519_key_t *key) {
    assert(key);
    memcpy(data, key->pk, ED25519_KEY_SIZE);
}

ed25519_key_t *ed25519_key_copy(const ed25519_key_t *key) {
    assert(key);
    ed25519_key_t *new_key = (ed25519_key_t *) calloc(1, sizeof(ed25519_key_t));
    assert(new_key);
    memcpy(new_key, key, sizeof(ed25519_key_t));
    return new_key;
}

void ed25519_sign(unsigned char *signature, size_t signature_len,
                  const unsigned char *data, size_t data_len,
                  const ed25519_key_t *key) {
    assert(key && key->has_sk);
    assert(signature_len >= crypto_sign_ed25519_BYTES);
    (void) signature_len;
    crypto_sign_ed25519_detached(signature, NULL, data, data_len, key->sk);
}

int ed25519_verify(const unsigned char *signature, size_t signature_len,
                   const unsigned char *data, size_t data_len,
                   const ed25519_key_t *key) {
    assert(key);
    if (signature_len != crypto_sign_ed25519_BYTES) {
        return 0;
    }
    return crypto_sign_ed25519_verify_detached(signature, data, data_len, key->pk) == 0 ? 1 : 0;
}

void ed25519_key_destroy(ed25519_key_t *key) {
    if (key) {
        sodium_memzero(key, sizeof(*key));
        free(key);
    }
}

// SHA 512

struct sha_ctx_s {
    mbedtls_sha512_context digest_ctx;
};

sha_ctx_t *sha_init() {
    sha_ctx_t *ctx = (sha_ctx_t *) calloc(1, sizeof(sha_ctx_t));
    assert(ctx != NULL);
    mbedtls_sha512_init(&ctx->digest_ctx);
    mbedtls_sha512_starts_ret(&ctx->digest_ctx, 0);
    return ctx;
}

void sha_update(sha_ctx_t *ctx, const uint8_t *in, int len) {
    if (len <= 0) return;
    mbedtls_sha512_update_ret(&ctx->digest_ctx, in, (size_t) len);
}

void sha_final(sha_ctx_t *ctx, uint8_t *out, unsigned int *len) {
    mbedtls_sha512_finish_ret(&ctx->digest_ctx, out);
    if (len) *len = SHA512_DIGEST_LENGTH;
}

void sha_reset(sha_ctx_t *ctx) {
    mbedtls_sha512_free(&ctx->digest_ctx);
    mbedtls_sha512_init(&ctx->digest_ctx);
    mbedtls_sha512_starts_ret(&ctx->digest_ctx, 0);
}

void sha_destroy(sha_ctx_t *ctx) {
    if (ctx) {
        mbedtls_sha512_free(&ctx->digest_ctx);
        free(ctx);
    }
}

// MD5

struct md5_ctx_s {
    mbedtls_md5_context digest_ctx;
};

md5_ctx_t *md5_init() {
    md5_ctx_t *ctx = (md5_ctx_t *) calloc(1, sizeof(md5_ctx_t));
    assert(ctx != NULL);
    mbedtls_md5_init(&ctx->digest_ctx);
    mbedtls_md5_starts_ret(&ctx->digest_ctx);
    return ctx;
}

void md5_update(md5_ctx_t *ctx, const uint8_t *in, int len) {
    if (len <= 0) return;
    mbedtls_md5_update_ret(&ctx->digest_ctx, in, (size_t) len);
}

void md5_final(md5_ctx_t *ctx, uint8_t *out, unsigned int *len) {
    mbedtls_md5_finish_ret(&ctx->digest_ctx, out);
    if (len) *len = MD5_DIGEST_LENGTH;
}

void md5_reset(md5_ctx_t *ctx) {
    mbedtls_md5_free(&ctx->digest_ctx);
    mbedtls_md5_init(&ctx->digest_ctx);
    mbedtls_md5_starts_ret(&ctx->digest_ctx);
}

void md5_destroy(md5_ctx_t *ctx) {
    if (ctx) {
        mbedtls_md5_free(&ctx->digest_ctx);
        free(ctx);
    }
}

char *get_md5(char *string) {
    unsigned char hash[MD5_DIGEST_LENGTH] = {0};
    md5_ctx_t *ctx = md5_init();
    md5_update(ctx, (const unsigned char *) string, strlen(string));
    md5_final(ctx, hash, NULL);
    md5_destroy(ctx);
    char *result_str = utils_hex_to_string(hash, MD5_DIGEST_LENGTH);
    return result_str;   //must free result_str after use
}

void pk_to_base64(const unsigned char *pk, int pk_len, char *pk_base64, int len) {
    memset(pk_base64, 0, len);
    int len64 = (4 * (pk_len / 3)) + (pk_len % 3 ? 4 : 0);
    assert(len > len64);
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *) pk_base64, (size_t) len, &olen, pk, (size_t) pk_len);
    pk_base64[len64] = '\0';
}
