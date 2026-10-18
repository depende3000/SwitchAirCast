#!/usr/bin/env python3
"""Checks the SRP verifier (lib/airplay/srp.c) against an independent client.

The client follows Apple's pair-setup-pin as implemented by pyatv's legacy
AirPlay pairing (srptools with SHA-1, the RFC 5054 2048-bit group and
K = H(S | 00000000) | H(S | 00000001)).

usage: tests/srp_test.py build/linux/srp_server
"""

import hashlib
import secrets
import subprocess
import sys

N = int(
    "AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4"
    "A099ED8193E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF60"
    "95179A163AB3661A05FBD5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF"
    "747359D041D5C33EA71D281E446B14773BCA97B43A23FB801676BD207A436C6481F1D2B907"
    "8717461A5B9D32E688F87748544523B524B0D57D5EA77A2775D2ECFA032CFBDBF52FB37861"
    "60279004E57AE6AF874E7303CE53299CCC041C7BC308D82A5698F3A8D0C38271AE35F8E9DB"
    "FBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F9E4AFF73", 16)
g = 2
LEN = 256


def is_probable_prime(n, rounds=24):
    if n % 2 == 0:
        return False
    d, r = n - 1, 0
    while d % 2 == 0:
        d //= 2
        r += 1
    for _ in range(rounds):
        a = secrets.randbelow(n - 3) + 2
        x = pow(a, d, n)
        if x in (1, n - 1):
            continue
        for _ in range(r - 1):
            x = pow(x, 2, n)
            if x == n - 1:
                break
        else:
            return False
    return True


def b(n, pad=0):
    raw = n.to_bytes(max(1, (n.bit_length() + 7) // 8), "big")
    return raw.rjust(pad, b"\0")


def H(*parts):
    h = hashlib.sha1()
    for p in parts:
        h.update(p)
    return h.digest()


def run(server, *args):
    return subprocess.run([server, *args], capture_output=True, text=True, check=True).stdout.split()


def one_round(server, user, pin, wrong_pin=None):
    b_secret = secrets.token_bytes(32)
    salt_hex, v_hex, B_hex = run(server, "step1", user, pin, b_secret.hex())
    salt, B = bytes.fromhex(salt_hex), int(B_hex, 16)

    # client side
    a = int.from_bytes(secrets.token_bytes(32), "big")
    A = pow(g, a, N)
    k = int.from_bytes(H(b(N), b(g, LEN)), "big")
    u = int.from_bytes(H(b(A, LEN), b(B, LEN)), "big")
    x = int.from_bytes(H(salt, H(user.encode(), b":", (wrong_pin or pin).encode())), "big")
    S = pow((B - k * pow(g, x, N)) % N, a + u * x, N)
    K = H(b(S), b"\0\0\0\0") + H(b(S), b"\0\0\0\1")
    hN, hg = H(b(N)), H(b(g))
    M = H(bytes(i ^ j for i, j in zip(hN, hg)), H(user.encode()), salt, b(A), b(B), K)
    HAMK = H(b(A), M, K)

    out = run(server, "step2", user, salt_hex, v_hex, b(A).hex(), b_secret.hex(), M.hex())
    if wrong_pin:
        return out == ["fail"]
    return out == ["ok", HAMK.hex(), K.hex()]


def main():
    server = sys.argv[1]
    assert is_probable_prime(N) and is_probable_prime((N - 1) // 2), "N is not the RFC 5054 safe prime"
    for i in range(20):
        assert one_round(server, "7E:B7:12:F3:77:9B", "%04d" % secrets.randbelow(10000)), "round %d failed" % i
    assert one_round(server, "7E:B7:12:F3:77:9B", "1234", wrong_pin="4321"), "a wrong PIN was accepted"
    print("srp_test: OK (20 pairings, wrong PIN rejected)")


main()
