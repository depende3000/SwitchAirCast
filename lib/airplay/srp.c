/**
 * SwitchAirCast - SRP stub
 *
 * UxPlay only uses SRP-6a for the optional on-screen PIN pairing mode
 * (pair-setup-pin). SwitchAirCast advertises legacy pairing without a PIN,
 * so these entry points only need to fail cleanly if a client asks for it:
 * srp_new_user() then returns -3 and srp_validate_proof() returns -1.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include <stddef.h>
#include "srp.h"

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
}

void srp_create_server_ephemeral_key(SRP_HashAlgorithm alg, SRP_NGType ng_type,
                                     const unsigned char *bytes_v, int len_v,
                                     const unsigned char *bytes_b, int len_b,
                                     const unsigned char **bytes_B, int *len_B,
                                     const char *n_hex, const char *g_hex,
                                     int rfc5054_compat) {
    *bytes_B = NULL;
    *len_B = 0;
}

struct SRPVerifier *srp_verifier_new(SRP_HashAlgorithm alg, SRP_NGType ng_type, const char *username,
                                     const unsigned char *bytes_s, int len_s,
                                     const unsigned char *bytes_v, int len_v,
                                     const unsigned char *bytes_A, int len_A,
                                     const unsigned char *bytes_b, int len_b,
                                     const unsigned char **bytes_B, int *len_B,
                                     const char *n_hex, const char *g_hex,
                                     int rfc5054_compat) {
    if (bytes_B) *bytes_B = NULL;
    if (len_B) *len_B = 0;
    return NULL;
}

void srp_verifier_delete(struct SRPVerifier *ver) {
    (void) ver;
}

int srp_verifier_is_authenticated(struct SRPVerifier *ver) {
    (void) ver;
    return 0;
}

const char *srp_verifier_get_username(struct SRPVerifier *ver) {
    (void) ver;
    return NULL;
}

const unsigned char *srp_verifier_get_session_key(struct SRPVerifier *ver, int *key_length) {
    (void) ver;
    if (key_length) *key_length = 0;
    return NULL;
}

int srp_verifier_get_session_key_length(struct SRPVerifier *ver) {
    (void) ver;
    return 0;
}

void srp_verifier_verify_session(struct SRPVerifier *ver, const unsigned char *user_M,
                                 const unsigned char **bytes_HAMK) {
    (void) ver;
    (void) user_M;
    if (bytes_HAMK) *bytes_HAMK = NULL;
}
