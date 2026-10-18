/*
 * fake_mac_pair - plays a Mac pairing with a code before it sends video:
 * POST /pair-pin-start, then the three POST /pair-setup-pin steps (SRP-6a with the
 * code, then an exchange of ed25519 keys under AES-GCM). The code is read from the
 * receiver's log, standing in for the user reading it off the screen.
 *
 *   fake_mac_pair <receiver-ip> <port> <receiver log.txt> [--wrong-pin]
 *
 * Prints PASS when the receiver accepts the right code (or rejects a wrong one).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mbedtls/bignum.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha512.h>
#include <plist/plist.h>
#include <sodium.h>

#include "crypto.h"

static const char N_HEX[] =
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
    "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
    "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
    "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
    "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
    "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
    "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73";
#define NLEN 256
static const char USER[] = "7E:B7:12:F3:77:9B";

/* like a Mac's video connection: no CSeq, an X-Apple-Session-ID */
static int fd = -1;

static void die(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static void send_all(const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) die("send failed");
        p += n;
        len -= (size_t) n;
    }
}

/* POST url with an optional binary plist; returns the status and the reply plist */
static int post(const char *url, plist_t dict, plist_t *out) {
    char *bin = NULL;
    uint32_t bin_len = 0;
    if (dict) plist_to_bin(dict, &bin, &bin_len);
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
                     "POST %s RTSP/1.0\r\nUser-Agent: AirPlay/980.77.5\r\n"
                     "X-Apple-Device-ID: 0x7eb712f3779b\r\nX-Apple-Session-ID: 820ddf5a-0705-48fa-956b-fe3ca9db797c\r\n"
                     "Content-Length: %u\r\n%s\r\n",
                     url, bin_len, dict ? "Content-Type: application/x-apple-binary-plist\r\n" : "");
    send_all(hdr, (size_t) n);
    if (bin_len) send_all(bin, bin_len);
    if (bin) plist_mem_free(bin);

    char buf[8192];
    size_t got = 0;
    char *end = NULL;
    while (!end) {
        ssize_t r = recv(fd, buf + got, sizeof(buf) - 1 - got, 0);
        if (r <= 0) die("no response from receiver");
        got += (size_t) r;
        buf[got] = '\0';
        end = strstr(buf, "\r\n\r\n");
    }
    int status = 0;
    sscanf(buf, "RTSP/1.0 %d", &status);
    size_t clen = 0;
    char *cl = strcasestr(buf, "Content-Length:");
    if (cl && cl < end) clen = (size_t) strtoul(cl + 15, NULL, 10);
    size_t header_len = (size_t) (end - buf) + 4;
    uint8_t *data = malloc(clen + 1);
    size_t have = got - header_len < clen ? got - header_len : clen;
    memcpy(data, buf + header_len, have);
    while (have < clen) {
        ssize_t r = recv(fd, data + have, clen - have, 0);
        if (r <= 0) die("short response body");
        have += (size_t) r;
    }
    if (out) {
        *out = NULL;
        if (clen) plist_from_bin((const char *) data, (uint32_t) clen, out);
    }
    free(data);
    return status;
}

static void get_data(plist_t dict, const char *key, uint8_t **data, uint64_t *len) {
    plist_t node = dict ? plist_dict_get_item(dict, key) : NULL;
    if (!node || plist_get_node_type(node) != PLIST_DATA) die(key);
    plist_get_data_val(node, (char **) data, len);
}

/* ---- SRP client (sha1, unpadded numbers unless PAD) ---- */

static void sha1_parts(uint8_t out[20], const uint8_t *const *parts, const size_t *lens, int n) {
    mbedtls_sha1_context c;
    mbedtls_sha1_init(&c);
    mbedtls_sha1_starts_ret(&c);
    for (int i = 0; i < n; i++) mbedtls_sha1_update_ret(&c, parts[i], lens[i]);
    mbedtls_sha1_finish_ret(&c, out);
    mbedtls_sha1_free(&c);
}

static size_t num(const mbedtls_mpi *x, uint8_t *buf, size_t pad) {
    size_t len = pad ? pad : mbedtls_mpi_size(x);
    mbedtls_mpi_write_binary(x, buf, len);
    return len;
}

static const char *read_pin(const char *log_path, char pin[8]) {
    FILE *f = fopen(log_path, "r");
    if (!f) return NULL;
    char line[512];
    pin[0] = '\0';
    while (fgets(line, sizeof(line), f)) {
        char *p = strstr(line, "showing AirPlay code ");
        if (p) sscanf(p + strlen("showing AirPlay code "), "%7s", pin);
    }
    fclose(f);
    return pin[0] ? pin : NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) die("usage: fake_mac_pair <ip> <port> <log.txt> [--wrong-pin]");
    bool wrong = argc > 4 && !strcmp(argv[4], "--wrong-pin");
    if (sodium_init() < 0) die("sodium_init");

    fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) atoi(argv[2]));
    inet_pton(AF_INET, argv[1], &a.sin_addr);
    if (connect(fd, (struct sockaddr *) &a, sizeof(a)) < 0) die("connect");

    if (post("/pair-pin-start", NULL, NULL) != 200) die("pair-pin-start was not accepted");
    usleep(300 * 1000);
    char pin[8];
    if (!read_pin(argv[3], pin)) die("the receiver did not show a code");
    printf("receiver shows code %s\n", pin);
    if (wrong) pin[0] = pin[0] == '9' ? '0' : (char) (pin[0] + 1);

    /* step 1: user name -> salt and B */
    plist_t req = plist_new_dict(), res = NULL;
    plist_dict_set_item(req, "method", plist_new_string("pin"));
    plist_dict_set_item(req, "user", plist_new_string(USER));
    if (post("/pair-setup-pin", req, &res) != 200) die("pair-setup-pin step 1");
    plist_free(req);
    uint8_t *salt, *Bb;
    uint64_t salt_len, B_len;
    get_data(res, "salt", &salt, &salt_len);
    get_data(res, "pk", &Bb, &B_len);
    plist_free(res);

    mbedtls_mpi N, g, k, x, v, aa, A, B, u, S, t1, t2;
    mbedtls_mpi *all[] = { &N, &g, &k, &x, &v, &aa, &A, &B, &u, &S, &t1, &t2 };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) mbedtls_mpi_init(all[i]);
    mbedtls_mpi_read_string(&N, 16, N_HEX);
    mbedtls_mpi_lset(&g, 2);
    uint8_t bufN[NLEN], bufg[NLEN], bufA[NLEN], bufB[NLEN], bufS[NLEN], d[20], inner[20];
    size_t lN = num(&N, bufN, 0), lg;

    /* k = H(N | PAD(g)) */
    lg = num(&g, bufg, NLEN);
    sha1_parts(d, (const uint8_t *[]) { bufN, bufg }, (size_t[]) { lN, lg }, 2);
    mbedtls_mpi_read_binary(&k, d, 20);
    /* x = H(s | H(I:P)) */
    sha1_parts(inner, (const uint8_t *[]) { (const uint8_t *) USER, (const uint8_t *) ":", (const uint8_t *) pin },
               (size_t[]) { strlen(USER), 1, strlen(pin) }, 3);
    sha1_parts(d, (const uint8_t *[]) { salt, inner }, (size_t[]) { salt_len, 20 }, 2);
    mbedtls_mpi_read_binary(&x, d, 20);
    /* A = g^a */
    uint8_t secret[32];
    randombytes_buf(secret, sizeof(secret));
    mbedtls_mpi_read_binary(&aa, secret, sizeof(secret));
    mbedtls_mpi_exp_mod(&A, &g, &aa, &N, NULL);
    mbedtls_mpi_read_binary(&B, Bb, B_len);
    /* u = H(PAD(A) | PAD(B)) */
    size_t lA = num(&A, bufA, NLEN), lB = num(&B, bufB, NLEN);
    sha1_parts(d, (const uint8_t *[]) { bufA, bufB }, (size_t[]) { lA, lB }, 2);
    mbedtls_mpi_read_binary(&u, d, 20);
    /* S = (B - k*g^x) ^ (a + u*x) */
    mbedtls_mpi_exp_mod(&v, &g, &x, &N, NULL);
    mbedtls_mpi_mul_mpi(&t1, &k, &v);
    mbedtls_mpi_sub_mpi(&t1, &B, &t1);
    mbedtls_mpi_mod_mpi(&t1, &t1, &N);
    mbedtls_mpi_mul_mpi(&t2, &u, &x);
    mbedtls_mpi_add_mpi(&t2, &t2, &aa);
    mbedtls_mpi_exp_mod(&S, &t1, &t2, &N, NULL);
    size_t lS = num(&S, bufS, 0);
    uint8_t K[40];
    sha1_parts(K, (const uint8_t *[]) { bufS, (const uint8_t *) "\0\0\0\0" }, (size_t[]) { lS, 4 }, 2);
    sha1_parts(K + 20, (const uint8_t *[]) { bufS, (const uint8_t *) "\0\0\0\1" }, (size_t[]) { lS, 4 }, 2);
    /* M = H(H(N) xor H(g) | H(I) | s | A | B | K) */
    uint8_t hN[20], hg[20], hI[20], M[20], hamk[20];
    size_t lg1 = num(&g, bufg, 0);
    sha1_parts(hN, (const uint8_t *[]) { bufN }, (size_t[]) { lN }, 1);
    sha1_parts(hg, (const uint8_t *[]) { bufg }, (size_t[]) { lg1 }, 1);
    sha1_parts(hI, (const uint8_t *[]) { (const uint8_t *) USER }, (size_t[]) { strlen(USER) }, 1);
    for (int i = 0; i < 20; i++) hN[i] ^= hg[i];
    lA = num(&A, bufA, 0);
    lB = num(&B, bufB, 0);
    sha1_parts(M, (const uint8_t *[]) { hN, hI, salt, bufA, bufB, K },
               (size_t[]) { 20, 20, salt_len, lA, lB, 40 }, 6);
    sha1_parts(hamk, (const uint8_t *[]) { bufA, M, K }, (size_t[]) { lA, 20, 40 }, 3);
    if (getenv("FAKE_MAC_DEBUG")) {
        fprintf(stderr, "a=");
        for (int i = 0; i < 32; i++) fprintf(stderr, "%02x", secret[i]);
        fprintf(stderr, "\nsalt=");
        for (uint64_t i = 0; i < salt_len; i++) fprintf(stderr, "%02x", salt[i]);
        fprintf(stderr, "\nB=");
        for (uint64_t i = 0; i < B_len; i++) fprintf(stderr, "%02x", Bb[i]);
        fprintf(stderr, "\npin=%s\nM=", pin);
        for (int i = 0; i < 20; i++) fprintf(stderr, "%02x", M[i]);
        fprintf(stderr, "\n");
    }

    /* step 2: A and M -> the receiver's proof */
    req = plist_new_dict();
    plist_dict_set_item(req, "pk", plist_new_data((const char *) bufA, lA));
    plist_dict_set_item(req, "proof", plist_new_data((const char *) M, 20));
    int status = post("/pair-setup-pin", req, &res);
    plist_free(req);
    if (wrong) {
        if (status != 470) die("a wrong code was not rejected with 470");
        printf("PASS (wrong code rejected)\n");
        return 0;
    }
    if (status != 200) die("pair-setup-pin step 2: the receiver rejected the right code");
    uint8_t *proof;
    uint64_t proof_len;
    get_data(res, "proof", &proof, &proof_len);
    plist_free(res);
    if (proof_len != 20 || memcmp(proof, hamk, 20)) die("the receiver's proof does not match");

    /* step 3: our ed25519 public key under AES-GCM keyed from K, the receiver answers with its own */
    uint8_t aes_key[16], aes_iv[16], h512[64], pk[32], sk[64], epk[32], tag[16];
    mbedtls_sha512_context c;
    const char *labels[2] = { "Pair-Setup-AES-Key", "Pair-Setup-AES-IV" };
    for (int i = 0; i < 2; i++) {
        mbedtls_sha512_init(&c);
        mbedtls_sha512_starts_ret(&c, 0);
        mbedtls_sha512_update_ret(&c, (const uint8_t *) labels[i], strlen(labels[i]));
        mbedtls_sha512_update_ret(&c, K, 40);
        mbedtls_sha512_finish_ret(&c, h512);
        mbedtls_sha512_free(&c);
        memcpy(i ? aes_iv : aes_key, h512, 16);
    }
    aes_iv[15]++;
    crypto_sign_keypair(pk, sk);
    if (gcm_encrypt(pk, 32, epk, aes_key, aes_iv, tag) != 32) die("gcm_encrypt");
    req = plist_new_dict();
    plist_dict_set_item(req, "epk", plist_new_data((const char *) epk, 32));
    plist_dict_set_item(req, "authTag", plist_new_data((const char *) tag, 16));
    if (post("/pair-setup-pin", req, &res) != 200) die("pair-setup-pin step 3");
    plist_free(req);
    uint8_t *repk, *rtag, server_pk[32];
    uint64_t repk_len, rtag_len;
    get_data(res, "epk", &repk, &repk_len);
    get_data(res, "authTag", &rtag, &rtag_len);
    aes_iv[15]++;
    if (repk_len != 32 || rtag_len != 16 || gcm_decrypt(repk, 32, server_pk, aes_key, aes_iv, rtag) != 32) {
        die("could not authenticate the receiver's key");
    }
    printf("paired; receiver key %02x%02x%02x%02x...\n", server_pk[0], server_pk[1], server_pk[2], server_pk[3]);
    printf("PASS\n");
    close(fd);
    return 0;
}
