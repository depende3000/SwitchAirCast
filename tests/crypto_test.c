/* Prints deterministic outputs of the crypto.h API so two backends can be diffed. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "crypto.h"

static void hex(const char *label, const unsigned char *p, int n) {
    printf("%s: ", label);
    for (int i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}

int main(void) {
    unsigned char key[16], iv[16], buf[256], out[256], out2[256];
    for (int i = 0; i < 16; i++) { key[i] = (unsigned char)(i * 7 + 3); iv[i] = (unsigned char)(0xf0 + i); }
    for (int i = 0; i < 256; i++) buf[i] = (unsigned char)(i * 13 + 1);

    /* AES-CTR streaming across odd-sized chunks, fresh block, reset */
    aes_ctx_t *ctr = aes_ctr_init(key, iv);
    aes_ctr_encrypt(ctr, buf, out, 5);
    aes_ctr_encrypt(ctr, buf + 5, out + 5, 23);
    aes_ctr_start_fresh_block(ctr);
    aes_ctr_encrypt(ctr, buf + 28, out + 28, 100);
    aes_ctr_decrypt(ctr, buf + 128, out + 128, 37);
    hex("ctr", out, 165);
    aes_ctr_reset(ctr);
    aes_ctr_decrypt(ctr, out, out2, 28);
    hex("ctr_reset_roundtrip", out2, 28);
    aes_ctr_destroy(ctr);

    /* AES-CBC chaining across calls, then reset */
    aes_ctx_t *cbc = aes_cbc_init(key, iv, AES_DECRYPT);
    aes_cbc_decrypt(cbc, buf, out, 32);
    aes_cbc_decrypt(cbc, buf + 32, out + 32, 32);
    hex("cbc_dec", out, 64);
    aes_cbc_reset(cbc);
    aes_cbc_decrypt(cbc, buf, out, 48);
    hex("cbc_dec_reset", out, 48);
    aes_cbc_destroy(cbc);
    cbc = aes_cbc_init(key, iv, AES_ENCRYPT);
    aes_cbc_encrypt(cbc, buf, out, 64);
    hex("cbc_enc", out, 64);
    aes_cbc_destroy(cbc);

    /* GCM */
    unsigned char tag[16], pt[64];
    int n = gcm_encrypt(buf, 32, out, key, iv, tag);
    printf("gcm_len: %d\n", n);
    hex("gcm_ct", out, 32);
    hex("gcm_tag", tag, 16);
    n = gcm_decrypt(out, 32, pt, key, iv, tag);
    printf("gcm_dec_len: %d match: %d\n", n, memcmp(pt, buf, 32) == 0);
    tag[0] ^= 1;
    n = gcm_decrypt(out, 32, pt, key, iv, tag);
    printf("gcm_bad_tag: %d\n", n);

    /* Ed25519 deterministic key from device id */
    int result = -1;
    ed25519_key_t *ed = ed25519_key_generate("AA:BB:CC:DD:EE:FF", "", &result);
    unsigned char pk[32], sig[64];
    ed25519_key_get_raw(pk, ed);
    hex("ed_pk", pk, 32);
    printf("ed_result: %d\n", result);
    ed25519_sign(sig, 64, buf, 100, ed);
    hex("ed_sig", sig, 64);
    ed25519_key_t *edpub = ed25519_key_from_raw(pk);
    printf("ed_verify: %d\n", ed25519_verify(sig, 64, buf, 100, edpub));
    sig[3] ^= 0x40;
    printf("ed_verify_bad: %d\n", ed25519_verify(sig, 64, buf, 100, edpub));
    ed25519_key_t *edcopy = ed25519_key_copy(ed);
    ed25519_sign(sig, 64, buf, 10, edcopy);
    hex("ed_sig_copy", sig, 64);
    ed25519_key_destroy(edcopy);
    ed25519_key_destroy(edpub);
    ed25519_key_destroy(ed);

    char b64[64];
    pk_to_base64(pk, 32, b64, sizeof(b64));
    printf("pk_b64: %s\n", b64);
    pk_to_base64(buf, 7, b64, sizeof(b64));
    printf("b64_7: %s\n", b64);

    /* SHA-512 incremental + reset, MD5 */
    unsigned char h[64];
    unsigned int hl = 0;
    sha_ctx_t *sha = sha_init();
    sha_update(sha, buf, 10);
    sha_update(sha, buf + 10, 200);
    sha_final(sha, h, &hl);
    hex("sha512", h, 64);
    printf("sha_len: %u\n", hl);
    sha_reset(sha);
    sha_update(sha, (const unsigned char *) "abc", 3);
    sha_final(sha, h, NULL);
    hex("sha512_abc", h, 64);
    sha_destroy(sha);
    char *md5 = get_md5("The quick brown fox");
    printf("md5: %s\n", md5);
    free(md5);

    /* X25519 agreement consistency */
    x25519_key_t *a = x25519_key_generate(), *b = x25519_key_generate();
    unsigned char apub[32], bpub[32], s1[32], s2[32];
    x25519_key_get_raw(apub, a);
    x25519_key_get_raw(bpub, b);
    x25519_key_t *A = x25519_key_from_raw(apub), *B = x25519_key_from_raw(bpub);
    x25519_derive_secret(s1, a, B);
    x25519_derive_secret(s2, b, A);
    printf("x25519_agree: %d\n", memcmp(s1, s2, 32) == 0);
    unsigned char rnd[32] = {0};
    printf("random_ok: %d\n", get_random_bytes(rnd, 32));
    return 0;
}
