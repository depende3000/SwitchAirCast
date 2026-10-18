/**
 * SwitchAirCast - SRP-6a server side for Apple's pair-setup-pin
 *
 * A Mac asks for a one-time PIN before it sends video to a receiver it has not
 * paired with (POST /pair-pin-start, then /pair-setup-pin). This implements the
 * verifier half of SRP-6a the way csrp does it with UxPlay's APPLE_VARIANT, on
 * mbedtls bignums instead of OpenSSL:
 *
 *   k = H(N | PAD(g))                 x = H(s | H(I ":" P))      v = g^x
 *   B = k*v + g^b                     u = H(PAD(A) | PAD(B))     S = (A * v^u)^b
 *   K = H(S | 00000000) | H(S | 00000001)          (Apple: twice the hash length)
 *   M = H(H(N) xor H(g) | H(I) | s | A | B | K)    HAMK = H(A | M | K)
 *
 * Numbers are hashed as unpadded big-endian bytes except where PAD() says otherwise.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stdlib.h>
#include <string.h>

#include <mbedtls/bignum.h>
#include <mbedtls/md.h>

#include "crypto.h"
#include "srp.h"

#define MAX_HASH 64
#define MAX_KEY (2 * MAX_HASH)

/* RFC 5054 2048-bit group */
static const char N_2048[] =
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
    "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
    "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
    "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
    "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
    "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
    "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73";
static const char G_2048[] = "2";

struct SRPVerifier {
    SRP_HashAlgorithm alg;
    int authenticated;
    char *username;
    unsigned char *bytes_B;
    int len_B;
    unsigned char M[MAX_HASH];
    unsigned char H_AMK[MAX_HASH];
    unsigned char session_key[MAX_KEY];
    int session_key_len;
};

typedef struct {
    mbedtls_mpi N, g;
    size_t len_N;
} group_t;

static const mbedtls_md_info_t *md_info(SRP_HashAlgorithm alg) {
    switch (alg) {
    case SRP_SHA1: return mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    case SRP_SHA224: return mbedtls_md_info_from_type(MBEDTLS_MD_SHA224);
    case SRP_SHA256: return mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    case SRP_SHA384: return mbedtls_md_info_from_type(MBEDTLS_MD_SHA384);
    case SRP_SHA512: return mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    }
    return NULL;
}

static int hash_len(SRP_HashAlgorithm alg) {
    const mbedtls_md_info_t *info = md_info(alg);
    return info ? mbedtls_md_get_size(info) : 0;
}

static int group_init(group_t *grp, SRP_NGType ng_type, const char *n_hex, const char *g_hex) {
    mbedtls_mpi_init(&grp->N);
    mbedtls_mpi_init(&grp->g);
    if (ng_type != SRP_NG_CUSTOM || !n_hex || !g_hex) {
        n_hex = N_2048;
        g_hex = G_2048;
    }
    if (mbedtls_mpi_read_string(&grp->N, 16, n_hex) || mbedtls_mpi_read_string(&grp->g, 16, g_hex)) return -1;
    grp->len_N = mbedtls_mpi_size(&grp->N);
    return 0;
}

static void group_free(group_t *grp) {
    mbedtls_mpi_free(&grp->N);
    mbedtls_mpi_free(&grp->g);
}

/* hash context that accepts byte strings and numbers */
typedef struct {
    mbedtls_md_context_t md;
    int ok;
} hasher_t;

static void h_start(hasher_t *h, SRP_HashAlgorithm alg) {
    mbedtls_md_init(&h->md);
    const mbedtls_md_info_t *info = md_info(alg);
    h->ok = info && mbedtls_md_setup(&h->md, info, 0) == 0 && mbedtls_md_starts(&h->md) == 0;
}

static void h_bytes(hasher_t *h, const unsigned char *data, size_t len) {
    if (h->ok && len) h->ok = mbedtls_md_update(&h->md, data, len) == 0;
}

/* pad = 0: unpadded big-endian; otherwise left-padded with zeros to pad bytes */
static void h_num(hasher_t *h, const mbedtls_mpi *n, size_t pad) {
    size_t len = mbedtls_mpi_size(n);
    if (pad < len) pad = len;
    unsigned char *buf = calloc(1, pad ? pad : 1);
    if (!buf) {
        h->ok = 0;
        return;
    }
    if (mbedtls_mpi_write_binary(n, buf, pad)) h->ok = 0;
    else h_bytes(h, buf, pad);
    free(buf);
}

static int h_finish(hasher_t *h, unsigned char *out) {
    if (h->ok) h->ok = mbedtls_md_finish(&h->md, out) == 0;
    mbedtls_md_free(&h->md);
    return h->ok ? 0 : -1;
}

static int hash_to_mpi(mbedtls_mpi *out, const unsigned char *digest, int len) {
    return mbedtls_mpi_read_binary(out, digest, (size_t) len);
}

/* k = H(N | PAD(g)) */
static int calculate_k(mbedtls_mpi *k, SRP_HashAlgorithm alg, const group_t *grp) {
    unsigned char d[MAX_HASH];
    hasher_t h;
    h_start(&h, alg);
    h_num(&h, &grp->N, 0);
    h_num(&h, &grp->g, grp->len_N);
    if (h_finish(&h, d)) return -1;
    return hash_to_mpi(k, d, hash_len(alg));
}

/* x = H(s | H(I ":" P)) */
static int calculate_x(mbedtls_mpi *x, SRP_HashAlgorithm alg, const unsigned char *s, int len_s,
                       const char *username, const unsigned char *password, int len_password) {
    unsigned char inner[MAX_HASH], d[MAX_HASH];
    hasher_t h;
    h_start(&h, alg);
    h_bytes(&h, (const unsigned char *) username, strlen(username));
    h_bytes(&h, (const unsigned char *) ":", 1);
    h_bytes(&h, password, (size_t) len_password);
    if (h_finish(&h, inner)) return -1;
    /* the salt is hashed as a number, i.e. without leading zero bytes */
    while (len_s > 0 && *s == 0) {
        s++;
        len_s--;
    }
    h_start(&h, alg);
    h_bytes(&h, s, (size_t) len_s);
    h_bytes(&h, inner, (size_t) hash_len(alg));
    if (h_finish(&h, d)) return -1;
    return hash_to_mpi(x, d, hash_len(alg));
}

/* B = (k*v + g^b) mod N */
static int calculate_B(mbedtls_mpi *B, SRP_HashAlgorithm alg, const group_t *grp,
                       const mbedtls_mpi *v, const mbedtls_mpi *b) {
    mbedtls_mpi k, kv, gb;
    mbedtls_mpi_init(&k);
    mbedtls_mpi_init(&kv);
    mbedtls_mpi_init(&gb);
    int ret = calculate_k(&k, alg, grp);
    if (!ret) ret = mbedtls_mpi_mul_mpi(&kv, &k, v);
    if (!ret) ret = mbedtls_mpi_exp_mod(&gb, &grp->g, b, &grp->N, NULL);
    if (!ret) ret = mbedtls_mpi_add_mpi(B, &kv, &gb);
    if (!ret) ret = mbedtls_mpi_mod_mpi(B, B, &grp->N);
    mbedtls_mpi_free(&k);
    mbedtls_mpi_free(&kv);
    mbedtls_mpi_free(&gb);
    return ret;
}

static unsigned char *mpi_to_bytes(const mbedtls_mpi *n, size_t len, int *out_len) {
    if (!len) len = mbedtls_mpi_size(n);
    unsigned char *buf = malloc(len ? len : 1);
    if (!buf || mbedtls_mpi_write_binary(n, buf, len)) {
        free(buf);
        *out_len = 0;
        return NULL;
    }
    *out_len = (int) len;
    return buf;
}

void srp_random_seed(const unsigned char *random_data, int data_length) {
    (void) random_data;
    (void) data_length;
}

void srp_create_salted_verification_key(SRP_HashAlgorithm alg, SRP_NGType ng_type, const char *username,
                                        const unsigned char *password, int len_password,
                                        const unsigned char **bytes_s, int *len_s,
                                        const unsigned char **bytes_v, int *len_v,
                                        const char *n_hex, const char *g_hex) {
    *bytes_s = NULL;
    *len_s = 0;
    *bytes_v = NULL;
    *len_v = 0;
    group_t grp;
    mbedtls_mpi x, v;
    mbedtls_mpi_init(&x);
    mbedtls_mpi_init(&v);
    unsigned char *s = malloc(16);
    if (!s || group_init(&grp, ng_type, n_hex, g_hex)) {
        free(s);
        group_free(&grp);
        return;
    }
    /* a non-zero first byte keeps "salt as bytes" and "salt as a number" identical */
    do {
        get_random_bytes(s, 16);
    } while (s[0] == 0);
    if (calculate_x(&x, alg, s, 16, username, password, len_password) == 0 &&
        mbedtls_mpi_exp_mod(&v, &grp.g, &x, &grp.N, NULL) == 0) {
        /* full width: the caller stores exactly len(N) bytes */
        unsigned char *vb = mpi_to_bytes(&v, grp.len_N, len_v);
        if (vb) {
            *bytes_v = vb;
            *bytes_s = s;
            *len_s = 16;
            s = NULL;
        }
    }
    free(s);
    mbedtls_mpi_free(&x);
    mbedtls_mpi_free(&v);
    group_free(&grp);
}

void srp_create_server_ephemeral_key(SRP_HashAlgorithm alg, SRP_NGType ng_type,
                                     const unsigned char *bytes_v, int len_v,
                                     const unsigned char *bytes_b, int len_b,
                                     const unsigned char **bytes_B, int *len_B,
                                     const char *n_hex, const char *g_hex,
                                     int rfc5054_compat) {
    (void) rfc5054_compat;
    *bytes_B = NULL;
    *len_B = 0;
    group_t grp;
    mbedtls_mpi v, b, B;
    mbedtls_mpi_init(&v);
    mbedtls_mpi_init(&b);
    mbedtls_mpi_init(&B);
    if (group_init(&grp, ng_type, n_hex, g_hex) == 0 &&
        mbedtls_mpi_read_binary(&v, bytes_v, (size_t) len_v) == 0 &&
        mbedtls_mpi_read_binary(&b, bytes_b, (size_t) len_b) == 0 &&
        calculate_B(&B, alg, &grp, &v, &b) == 0) {
        *bytes_B = mpi_to_bytes(&B, 0, len_B);
    }
    mbedtls_mpi_free(&v);
    mbedtls_mpi_free(&b);
    mbedtls_mpi_free(&B);
    group_free(&grp);
}

struct SRPVerifier *srp_verifier_new(SRP_HashAlgorithm alg, SRP_NGType ng_type, const char *username,
                                     const unsigned char *bytes_s, int len_s,
                                     const unsigned char *bytes_v, int len_v,
                                     const unsigned char *bytes_A, int len_A,
                                     const unsigned char *bytes_b, int len_b,
                                     const unsigned char **bytes_B, int *len_B,
                                     const char *n_hex, const char *g_hex,
                                     int rfc5054_compat) {
    (void) rfc5054_compat;
    if (bytes_B) *bytes_B = NULL;
    if (len_B) *len_B = 0;
    int hl = hash_len(alg);
    struct SRPVerifier *ver = calloc(1, sizeof(*ver));
    if (!ver || !hl) {
        free(ver);
        return NULL;
    }
    ver->alg = alg;
    ver->username = strdup(username ? username : "");

    group_t grp;
    mbedtls_mpi v, A, b, B, u, S, tmp;
    mbedtls_mpi_init(&v);
    mbedtls_mpi_init(&A);
    mbedtls_mpi_init(&b);
    mbedtls_mpi_init(&B);
    mbedtls_mpi_init(&u);
    mbedtls_mpi_init(&S);
    mbedtls_mpi_init(&tmp);
    int ret = group_init(&grp, ng_type, n_hex, g_hex);
    if (!ret) ret = mbedtls_mpi_read_binary(&v, bytes_v, (size_t) len_v);
    if (!ret) ret = mbedtls_mpi_read_binary(&A, bytes_A, (size_t) len_A);
    if (!ret) ret = mbedtls_mpi_read_binary(&b, bytes_b, (size_t) len_b);
    /* SRP-6a safety check: abort if A % N == 0 */
    if (!ret) ret = mbedtls_mpi_mod_mpi(&tmp, &A, &grp.N);
    if (!ret && mbedtls_mpi_cmp_int(&tmp, 0) == 0) ret = -1;
    if (!ret) ret = calculate_B(&B, alg, &grp, &v, &b);

    unsigned char d[MAX_HASH];
    hasher_t h;
    if (!ret) {
        /* u = H(PAD(A) | PAD(B)) */
        h_start(&h, alg);
        h_num(&h, &A, grp.len_N);
        h_num(&h, &B, grp.len_N);
        ret = h_finish(&h, d);
        if (!ret) ret = hash_to_mpi(&u, d, hl);
    }
    /* S = (A * v^u) ^ b mod N */
    if (!ret) ret = mbedtls_mpi_exp_mod(&tmp, &v, &u, &grp.N, NULL);
    if (!ret) ret = mbedtls_mpi_mul_mpi(&tmp, &A, &tmp);
    if (!ret) ret = mbedtls_mpi_mod_mpi(&tmp, &tmp, &grp.N);
    if (!ret) ret = mbedtls_mpi_exp_mod(&S, &tmp, &b, &grp.N, NULL);
    /* Apple: K = H(S | 00000000) | H(S | 00000001) */
    for (int i = 0; !ret && i < 2; i++) {
        static const unsigned char counter[2][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 1 } };
        h_start(&h, alg);
        h_num(&h, &S, 0);
        h_bytes(&h, counter[i], 4);
        ret = h_finish(&h, ver->session_key + i * hl);
    }
    if (!ret) {
        ver->session_key_len = 2 * hl;
        /* M = H(H(N) xor H(g) | H(I) | s | A | B | K) */
        unsigned char hN[MAX_HASH], hg[MAX_HASH], hI[MAX_HASH];
        h_start(&h, alg);
        h_num(&h, &grp.N, 0);
        ret = h_finish(&h, hN);
        h_start(&h, alg);
        h_num(&h, &grp.g, 0);
        if (!ret) ret = h_finish(&h, hg);
        else h_finish(&h, hg);
        h_start(&h, alg);
        h_bytes(&h, (const unsigned char *) ver->username, strlen(ver->username));
        if (!ret) ret = h_finish(&h, hI);
        else h_finish(&h, hI);
        for (int i = 0; i < hl; i++) hN[i] ^= hg[i];
        h_start(&h, alg);
        h_bytes(&h, hN, (size_t) hl);
        h_bytes(&h, hI, (size_t) hl);
        h_bytes(&h, bytes_s, (size_t) len_s);
        h_num(&h, &A, 0);
        h_num(&h, &B, 0);
        h_bytes(&h, ver->session_key, (size_t) ver->session_key_len);
        if (!ret) ret = h_finish(&h, ver->M);
        else h_finish(&h, ver->M);
    }
    if (!ret) {
        /* HAMK = H(A | M | K) */
        h_start(&h, alg);
        h_num(&h, &A, 0);
        h_bytes(&h, ver->M, (size_t) hl);
        h_bytes(&h, ver->session_key, (size_t) ver->session_key_len);
        ret = h_finish(&h, ver->H_AMK);
    }
    if (!ret) {
        ver->bytes_B = mpi_to_bytes(&B, 0, &ver->len_B);
        if (bytes_B) *bytes_B = ver->bytes_B;
        if (len_B) *len_B = ver->len_B;
    } else {
        /* unusable: never authenticates */
        ver->session_key_len = 0;
        memset(ver->M, 0xff, sizeof(ver->M));
    }

    mbedtls_mpi_free(&v);
    mbedtls_mpi_free(&A);
    mbedtls_mpi_free(&b);
    mbedtls_mpi_free(&B);
    mbedtls_mpi_free(&u);
    mbedtls_mpi_free(&S);
    mbedtls_mpi_free(&tmp);
    group_free(&grp);
    return ver;
}

void srp_verifier_delete(struct SRPVerifier *ver) {
    if (!ver) return;
    free(ver->username);
    free(ver->bytes_B);
    memset(ver, 0, sizeof(*ver));
    free(ver);
}

int srp_verifier_is_authenticated(struct SRPVerifier *ver) {
    return ver ? ver->authenticated : 0;
}

const char *srp_verifier_get_username(struct SRPVerifier *ver) {
    return ver ? ver->username : NULL;
}

const unsigned char *srp_verifier_get_session_key(struct SRPVerifier *ver, int *key_length) {
    if (key_length) *key_length = ver ? ver->session_key_len : 0;
    return ver && ver->session_key_len ? ver->session_key : NULL;
}

int srp_verifier_get_session_key_length(struct SRPVerifier *ver) {
    return ver ? ver->session_key_len : 0;
}

void srp_verifier_verify_session(struct SRPVerifier *ver, const unsigned char *user_M,
                                 const unsigned char **bytes_HAMK) {
    *bytes_HAMK = NULL;
    if (!ver || !ver->session_key_len || !user_M) return;
    if (memcmp(ver->M, user_M, (size_t) hash_len(ver->alg)) == 0) {
        ver->authenticated = 1;
        *bytes_HAMK = ver->H_AMK;
    }
}
