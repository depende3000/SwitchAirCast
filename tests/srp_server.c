/*
 * Command-line access to the SRP verifier for tests/srp_test.py, which plays the
 * client side of Apple's pair-setup-pin with an independent implementation.
 *
 *   srp_server step1 <user> <pin> <b-hex>
 *       prints: salt verifier B (hex, one per line)
 *   srp_server step2 <user> <salt-hex> <verifier-hex> <A-hex> <b-hex> <M-hex>
 *       prints: ok|fail, then HAMK and K (hex)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "srp.h"

static unsigned char *from_hex(const char *hex, int *len) {
    size_t n = strlen(hex) / 2;
    unsigned char *out = malloc(n ? n : 1);
    for (size_t i = 0; i < n; i++) sscanf(hex + 2 * i, "%2hhx", &out[i]);
    *len = (int) n;
    return out;
}

static void print_hex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
    printf("\n");
}

int main(int argc, char **argv) {
    if (argc == 5 && !strcmp(argv[1], "step1")) {
        const unsigned char *s, *v, *B;
        int len_s, len_v, len_B, len_b;
        srp_create_salted_verification_key(SRP_SHA1, SRP_NG_2048, argv[2], (const unsigned char *) argv[3],
                                           (int) strlen(argv[3]), &s, &len_s, &v, &len_v, NULL, NULL);
        unsigned char *b = from_hex(argv[4], &len_b);
        srp_create_server_ephemeral_key(SRP_SHA1, SRP_NG_2048, v, len_v, b, len_b, &B, &len_B, NULL, NULL, 1);
        if (!s || !v || !B) return 1;
        print_hex(s, len_s);
        print_hex(v, len_v);
        print_hex(B, len_B);
        return 0;
    }
    if (argc == 8 && !strcmp(argv[1], "step2")) {
        int len_s, len_v, len_A, len_b, len_M, len_B, len_K;
        unsigned char *s = from_hex(argv[3], &len_s), *v = from_hex(argv[4], &len_v);
        unsigned char *A = from_hex(argv[5], &len_A), *b = from_hex(argv[6], &len_b);
        unsigned char *M = from_hex(argv[7], &len_M);
        const unsigned char *B, *HAMK;
        struct SRPVerifier *ver = srp_verifier_new(SRP_SHA1, SRP_NG_2048, argv[2], s, len_s, v, len_v, A, len_A,
                                                   b, len_b, &B, &len_B, NULL, NULL, 1);
        srp_verifier_verify_session(ver, M, &HAMK);
        if (!srp_verifier_is_authenticated(ver)) {
            printf("fail\n");
            srp_verifier_delete(ver);
            return 0;
        }
        const unsigned char *K = srp_verifier_get_session_key(ver, &len_K);
        printf("ok\n");
        print_hex(HAMK, 20);
        print_hex(K, len_K);
        srp_verifier_delete(ver);
        return 0;
    }
    fprintf(stderr, "usage: see the comment at the top of tests/srp_server.c\n");
    return 2;
}
