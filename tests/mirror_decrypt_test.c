/*
 * Checks that the mirror stream decryptor matches one continuous AES-CTR keystream
 * however the stream is cut into packets, including packets shorter than the
 * keystream left over from the previous packet (tiny frames of a static screen).
 * Each output buffer is exactly packet-sized, so an overrun shows up under ASan.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto.h"
#include "mirror_buffer.h"
#include "raop_buffer.h"

#define STREAM_LEN 200000

int main(void) {
    unsigned char aeskey_audio[RAOP_AESKEY_LEN];
    for (int i = 0; i < RAOP_AESKEY_LEN; i++) aeskey_audio[i] = (unsigned char) (i * 7 + 3);
    uint64_t stream_id = 0x1234567890ULL;

    /* the sender's side: the same key derivation as mirror_buffer_init_aes */
    unsigned char key[64] = {0}, iv[64] = {0};
    snprintf((char *) key, sizeof(key), "AirPlayStreamKey%" PRIu64, stream_id);
    snprintf((char *) iv, sizeof(iv), "AirPlayStreamIV%" PRIu64, stream_id);
    sha_ctx_t *sha = sha_init();
    sha_update(sha, key, (int) strlen((char *) key));
    sha_update(sha, aeskey_audio, RAOP_AESKEY_LEN);
    sha_final(sha, key, NULL);
    sha_reset(sha);
    sha_update(sha, iv, (int) strlen((char *) iv));
    sha_update(sha, aeskey_audio, RAOP_AESKEY_LEN);
    sha_final(sha, iv, NULL);
    sha_destroy(sha);

    unsigned char *plain = malloc(STREAM_LEN), *cipher = malloc(STREAM_LEN);
    srand(42);
    for (int i = 0; i < STREAM_LEN; i++) plain[i] = (unsigned char) rand();
    aes_ctx_t *enc = aes_ctr_init(key, iv);
    aes_ctr_decrypt(enc, plain, cipher, STREAM_LEN);
    aes_ctr_destroy(enc);

    mirror_buffer_t *mb = mirror_buffer_init(NULL, aeskey_audio);
    mirror_buffer_init_aes(mb, &stream_id);

    int pos = 0, packets = 0, tiny = 0;
    while (pos < STREAM_LEN) {
        int r = rand() % 4;
        int len = r == 0 ? 1 + rand() % 15 : r == 1 ? 16 * (1 + rand() % 8) : 1 + rand() % 3000;
        if (len > STREAM_LEN - pos) len = STREAM_LEN - pos;
        unsigned char *in = malloc((size_t) len), *out = malloc((size_t) len);
        memcpy(in, cipher + pos, (size_t) len);
        mirror_buffer_decrypt(mb, in, out, len);
        if (memcmp(out, plain + pos, (size_t) len) != 0) {
            fprintf(stderr, "FAIL: packet %d (offset %d, %d bytes) decrypted wrongly\n", packets, pos, len);
            return 1;
        }
        free(in);
        free(out);
        if (len < 16) tiny++;
        pos += len;
        packets++;
    }
    mirror_buffer_destroy(mb);
    free(plain);
    free(cipher);
    printf("mirror_decrypt_test: OK (%d packets, %d shorter than a block)\n", packets, tiny);
    return 0;
}
