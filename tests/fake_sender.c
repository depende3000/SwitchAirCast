/*
 * fake_sender - a minimal AirPlay screen-mirroring sender for testing SwitchAirCast
 * (or any UxPlay-based receiver) without an Apple device.
 *
 * It performs the same handshake an iPhone does (pair-setup, pair-verify,
 * FairPlay fp-setup, SETUP with an encrypted stream key, RECORD), then streams
 * a synthetic H.264 video (libx264) over the encrypted mirroring channel and,
 * with --audio, AAC-LC audio over encrypted RTP.
 *
 * FairPlay note: the sender side of FairPlay is not public, so the encrypted key
 * is random bytes and the sender computes the key the receiver will derive with
 * the receiver's own playfair code. This exercises the whole receive path, but
 * it is a self-consistency test, not proof of compatibility with Apple devices.
 *
 *   fake_sender <receiver-ip> [port=7000] [seconds=10] [--audio] [--width W --height H]
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <plist/plist.h>

#include "crypto.h"
#include "fairplay.h"
#include "logger.h"
#include "utils.h"

#define NTP_EPOCH_OFFSET 2208988800ULL

static const char *host;
static int rtsp_fd = -1;
static int cseq = 0;
static volatile bool ntp_running = true;

static void die(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

static uint64_t ntp_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t secs = (uint64_t) ts.tv_sec + NTP_EPOCH_OFFSET;
    uint64_t frac = ((uint64_t) ts.tv_nsec << 32) / 1000000000ULL;
    return (secs << 32) | frac;
}

static int tcp_connect(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (connect(fd, (struct sockaddr *) &a, sizeof(a)) < 0) {
        perror("connect");
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static void send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) die("send failed (receiver closed the connection?)");
        p += n;
        len -= (size_t) n;
    }
}

/* Sends an RTSP request and returns the status code; body is malloc'd into *resp. */
static int rtsp(const char *method, const char *url, const char *ctype, const void *body, size_t body_len,
                uint8_t **resp, size_t *resp_len) {
    char hdr[1024];
    int n = snprintf(hdr, sizeof(hdr),
                     "%s %s RTSP/1.0\r\nCSeq: %d\r\nUser-Agent: AirPlay/770.8.1\r\n"
                     "X-Apple-ProtocolVersion: 1\r\nContent-Length: %zu\r\n%s%s%s\r\n",
                     method, url, ++cseq, body_len, ctype ? "Content-Type: " : "", ctype ? ctype : "",
                     ctype ? "\r\n" : "");
    send_all(rtsp_fd, hdr, (size_t) n);
    if (body_len) send_all(rtsp_fd, body, body_len);

    char buf[8192];
    size_t got = 0;
    char *end = NULL;
    while (!end) {
        ssize_t r = recv(rtsp_fd, buf + got, sizeof(buf) - 1 - got, 0);
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
    size_t have = got - header_len;
    if (have > clen) have = clen;
    memcpy(data, buf + header_len, have);
    while (have < clen) {
        ssize_t r = recv(rtsp_fd, data + have, clen - have, 0);
        if (r <= 0) die("short response body");
        have += (size_t) r;
    }
    if (resp) {
        *resp = data;
        *resp_len = clen;
    } else {
        free(data);
    }
    return status;
}

static int rtsp_plist(const char *method, const char *url, plist_t dict, plist_t *out) {
    char *bin = NULL;
    uint32_t bin_len = 0;
    if (dict) plist_to_bin(dict, &bin, &bin_len);
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    int status = rtsp(method, url, dict ? "application/x-apple-binary-plist" : NULL, bin, bin_len, &resp, &resp_len);
    if (out) {
        *out = NULL;
        if (resp_len) plist_from_bin((const char *) resp, (uint32_t) resp_len, out);
    }
    free(resp);
    if (bin) plist_mem_free(bin);
    return status;
}

/* ---- NTP timing responder (the receiver polls the sender's clock) ---- */

static void *ntp_thread(void *arg) {
    int fd = *(int *) arg;
    while (ntp_running) {
        uint8_t req[128];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t n = recvfrom(fd, req, sizeof(req), 0, (struct sockaddr *) &from, &from_len);
        if (n < 32) continue;
        uint8_t resp[32] = { 0x80, 0xd3, 0x00, 0x07 };
        memcpy(resp + 8, req + 24, 8);  /* origin = their transmit time */
        uint64_t now = ntp_now();
        for (int i = 0; i < 8; i++) {
            resp[16 + i] = (uint8_t) (now >> (56 - 8 * i));
            resp[24 + i] = (uint8_t) (now >> (56 - 8 * i));
        }
        sendto(fd, resp, sizeof(resp), 0, (struct sockaddr *) &from, from_len);
    }
    return NULL;
}

static int udp_bind_any(unsigned short *port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(fd, (struct sockaddr *) &a, sizeof(a));
    socklen_t len = sizeof(a);
    getsockname(fd, (struct sockaddr *) &a, &len);
    *port = ntohs(a.sin_port);
    struct timeval tv = { 0, 200000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

/* ---- helpers ---- */

static void sha512_prefix(const char *salt, const uint8_t *data, int data_len, uint8_t out[16]) {
    uint8_t hash[64];
    sha_ctx_t *ctx = sha_init();
    sha_update(ctx, (const uint8_t *) salt, (int) strlen(salt));
    sha_update(ctx, data, data_len);
    sha_final(ctx, hash, NULL);
    sha_destroy(ctx);
    memcpy(out, hash, 16);
}

static void put_le32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t) (v >> (8 * i)); }
static void put_le64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t) (v >> (8 * i)); }
static void put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t) (v >> 8); p[1] = (uint8_t) v; }
static void put_be32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t) (v >> (24 - 8 * i)); }
static void put_lef(uint8_t *p, float f) { uint32_t v; memcpy(&v, &f, 4); put_le32(p, v); }

/* splits Annex-B data into NAL units */
static int next_nal(const uint8_t *buf, int len, int *pos, const uint8_t **nal, int *nal_len) {
    int i = *pos;
    while (i + 3 <= len && !(buf[i] == 0 && buf[i + 1] == 0 && (buf[i + 2] == 1 || (i + 3 < len && buf[i + 2] == 0 && buf[i + 3] == 1)))) i++;
    if (i + 3 > len) return 0;
    i += (buf[i + 2] == 1) ? 3 : 4;
    int start = i;
    while (i + 3 <= len && !(buf[i] == 0 && buf[i + 1] == 0 && (buf[i + 2] == 1 || (i + 3 < len && buf[i + 2] == 0 && buf[i + 3] == 1)))) i++;
    int end = (i + 3 > len) ? len : i;
    *nal = buf + start;
    *nal_len = end - start;
    *pos = end;
    return 1;
}

static void fill_frame(AVFrame *f, int n) {
    for (int y = 0; y < f->height; y++) {
        uint8_t *row = f->data[0] + y * f->linesize[0];
        for (int x = 0; x < f->width; x++) row[x] = (uint8_t) (16 + ((x + y + n * 4) & 0xff) * 219 / 255);
    }
    for (int y = 0; y < f->height / 2; y++) {
        for (int x = 0; x < f->width / 2; x++) {
            f->data[1][y * f->linesize[1] + x] = (uint8_t) (128 + 100 * ((x / 32) % 2 ? 1 : -1));
            f->data[2][y * f->linesize[2] + x] = (uint8_t) (128 + (n % 60) - 30);
        }
    }
    /* bouncing white square */
    int sx = (n * 9) % (f->width - 120), sy = (n * 5) % (f->height - 120);
    for (int y = sy; y < sy + 120; y++) memset(f->data[0] + y * f->linesize[0] + sx, 235, 120);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <receiver-ip> [port] [seconds] [--audio] [--width W --height H]\n", argv[0]);
        return 2;
    }
    host = argv[1];
    int port = 7000, seconds = 10, width = 1280, height = 720;
    bool with_audio = false;
    int positional = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--audio")) with_audio = true;
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) height = atoi(argv[++i]);
        else if (positional++ == 0) port = atoi(argv[i]);
        else seconds = atoi(argv[i]);
    }

    rtsp_fd = tcp_connect(host, port);
    if (rtsp_fd < 0) die("cannot connect to receiver");

    /* /info */
    plist_t info = NULL;
    int st = rtsp_plist("GET", "/info", NULL, &info);
    if (st != 200 || !info) die("GET /info failed");
    char *model = NULL, *name = NULL;
    plist_get_string_val(plist_dict_get_item(info, "model"), &model);
    plist_get_string_val(plist_dict_get_item(info, "name"), &name);
    printf("info: name=\"%s\" model=%s\n", name ? name : "?", model ? model : "?");
    plist_t displays = plist_dict_get_item(info, "displays");
    if (displays && plist_array_get_size(displays) > 0) {
        uint64_t w = 0, h = 0, fps = 0;
        plist_t d0 = plist_array_get_item(displays, 0);
        plist_get_uint_val(plist_dict_get_item(d0, "width"), &w);
        plist_get_uint_val(plist_dict_get_item(d0, "height"), &h);
        plist_get_uint_val(plist_dict_get_item(d0, "maxFPS"), &fps);
        printf("info: display %" PRIu64 "x%" PRIu64 " maxFPS %" PRIu64 "\n", w, h, fps);
    }
    plist_free(info);

    /* pair-setup: exchange Ed25519 public keys */
    int r = 0;
    ed25519_key_t *ed_ours = ed25519_key_generate("fake-sender-device", "", &r);
    uint8_t ed_pub[32];
    ed25519_key_get_raw(ed_pub, ed_ours);
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    st = rtsp("POST", "/pair-setup", "application/octet-stream", ed_pub, 32, &resp, &resp_len);
    if (st != 200 || resp_len != 32) die("pair-setup failed");
    ed25519_key_t *ed_theirs = ed25519_key_from_raw(resp);
    free(resp);
    printf("pair-setup: ok\n");

    /* pair-verify step 1: X25519 exchange, check the receiver's signature */
    x25519_key_t *x_ours = x25519_key_generate();
    uint8_t x_pub[32];
    x25519_key_get_raw(x_pub, x_ours);
    uint8_t pv1[68] = { 1, 0, 0, 0 };
    memcpy(pv1 + 4, x_pub, 32);
    memcpy(pv1 + 36, ed_pub, 32);
    st = rtsp("POST", "/pair-verify", "application/octet-stream", pv1, sizeof(pv1), &resp, &resp_len);
    if (st != 200 || resp_len != 96) die("pair-verify 1 failed");
    x25519_key_t *x_theirs = x25519_key_from_raw(resp);
    uint8_t secret[32];
    x25519_derive_secret(secret, x_ours, x_theirs);
    uint8_t key[16], iv[16];
    sha512_prefix("Pair-Verify-AES-Key", secret, 32, key);
    sha512_prefix("Pair-Verify-AES-IV", secret, 32, iv);
    aes_ctx_t *pv_aes = aes_ctr_init(key, iv);
    uint8_t sig[64], msg[64];
    aes_ctr_decrypt(pv_aes, resp + 32, sig, 64);
    memcpy(msg, resp, 32);
    memcpy(msg + 32, x_pub, 32);
    if (!ed25519_verify(sig, 64, msg, 64, ed_theirs)) die("receiver's pair-verify signature is invalid");
    free(resp);

    /* pair-verify step 2: send our signature, encrypted further along the same CTR stream */
    uint8_t x_theirs_raw[32];
    x25519_key_get_raw(x_theirs_raw, x_theirs);
    memcpy(msg, x_pub, 32);
    memcpy(msg + 32, x_theirs_raw, 32);
    ed25519_sign(sig, 64, msg, 64, ed_ours);
    uint8_t pv2[68] = { 0, 0, 0, 0 };
    aes_ctr_encrypt(pv_aes, sig, pv2 + 4, 64);
    aes_ctr_destroy(pv_aes);
    st = rtsp("POST", "/pair-verify", "application/octet-stream", pv2, sizeof(pv2), NULL, NULL);
    if (st != 200) die("pair-verify 2 failed (signature rejected)");
    printf("pair-verify: ok (signatures verified both ways)\n");

    /* FairPlay */
    logger_t *logger = logger_init();
    fairplay_t *fp = fairplay_init(logger);
    uint8_t fp1[16] = { 'F', 'P', 'L', 'Y', 3, 1, 1, 0, 0, 0, 0, 4, 2, 0, 2, 0xbb };
    uint8_t fp1_local[142];
    fairplay_setup(fp, fp1, fp1_local);
    st = rtsp("POST", "/fp-setup", "application/octet-stream", fp1, 16, &resp, &resp_len);
    if (st != 200 || resp_len != 142 || memcmp(resp, fp1_local, 142)) die("fp-setup 1 failed");
    free(resp);
    uint8_t fp2[164] = { 'F', 'P', 'L', 'Y', 3, 1, 3, 0, 0, 0, 0, 0x98 };
    get_random_bytes(fp2 + 13, 151);
    fp2[12] = fp1[14];  /* FairPlay mode, as in the first message */
    uint8_t fp2_local[32];
    fairplay_handshake(fp, fp2, fp2_local);
    st = rtsp("POST", "/fp-setup", "application/octet-stream", fp2, 164, &resp, &resp_len);
    if (st != 200 || resp_len != 32 || memcmp(resp, fp2_local, 32)) die("fp-setup 2 failed");
    free(resp);
    printf("fp-setup: ok\n");

    /* stream key: the receiver decrypts ekey with FairPlay, then hashes it with the pairing secret */
    uint8_t ekey[72], eiv[16], fp_key[16], aeskey[16];
    get_random_bytes(ekey, sizeof(ekey));
    get_random_bytes(eiv, sizeof(eiv));
    fairplay_decrypt(fp, ekey, fp_key);
    {
        uint8_t hash[64];
        sha_ctx_t *ctx = sha_init();
        sha_update(ctx, fp_key, 16);
        sha_update(ctx, secret, 32);
        sha_final(ctx, hash, NULL);
        sha_destroy(ctx);
        memcpy(aeskey, hash, 16);
    }

    {
        char *k1 = utils_hex_to_string(fp_key, 16), *k2 = utils_hex_to_string(aeskey, 16);
        printf("keys: fairplay %s, session %s\n", k1, k2);
        free(k1);
        free(k2);
    }

    unsigned short ntp_port = 0;
    int ntp_fd = udp_bind_any(&ntp_port);
    pthread_t ntp_tid;
    pthread_create(&ntp_tid, NULL, ntp_thread, &ntp_fd);

    plist_t setup1 = plist_new_dict();
    plist_dict_set_item(setup1, "ekey", plist_new_data((const char *) ekey, 72));
    plist_dict_set_item(setup1, "eiv", plist_new_data((const char *) eiv, 16));
    plist_dict_set_item(setup1, "deviceID", plist_new_string("AA:BB:CC:DD:EE:01"));
    plist_dict_set_item(setup1, "model", plist_new_string("iPhone15,2"));
    plist_dict_set_item(setup1, "name", plist_new_string("Fake iPhone"));
    plist_dict_set_item(setup1, "timingProtocol", plist_new_string("NTP"));
    plist_dict_set_item(setup1, "timingPort", plist_new_uint(ntp_port));
    plist_t out = NULL;
    st = rtsp_plist("SETUP", "rtsp://fake/1", setup1, &out);
    plist_free(setup1);
    if (st != 200 || !out) die("SETUP (session) failed");
    uint64_t timing_port = 0;
    plist_get_uint_val(plist_dict_get_item(out, "timingPort"), &timing_port);
    plist_free(out);
    printf("SETUP session: ok (receiver timing port %" PRIu64 ")\n", timing_port);

    /* mirroring stream */
    uint64_t stream_id = 0x1234567890ULL;
    plist_t setup2 = plist_new_dict();
    plist_t streams = plist_new_array();
    plist_t s110 = plist_new_dict();
    plist_dict_set_item(s110, "type", plist_new_uint(110));
    plist_dict_set_item(s110, "streamConnectionID", plist_new_uint(stream_id));
    plist_array_append_item(streams, s110);
    plist_dict_set_item(setup2, "streams", streams);
    st = rtsp_plist("SETUP", "rtsp://fake/1", setup2, &out);
    plist_free(setup2);
    if (st != 200 || !out) die("SETUP (mirroring) failed");
    uint64_t data_port = 0;
    plist_get_uint_val(plist_dict_get_item(plist_array_get_item(plist_dict_get_item(out, "streams"), 0), "dataPort"), &data_port);
    plist_free(out);
    printf("SETUP mirroring: data port %" PRIu64 "\n", data_port);

    /* audio stream (AAC-LC, ct=4) */
    int audio_fd = -1;
    struct sockaddr_in audio_addr = { 0 }, control_addr = { 0 };
    AVCodecContext *aenc = NULL;
    aes_ctx_t *audio_aes = NULL;
    if (with_audio) {
        unsigned short ctrl_port = 0;
        int ctrl_fd = udp_bind_any(&ctrl_port);
        plist_t setup3 = plist_new_dict();
        plist_t astreams = plist_new_array();
        plist_t s96 = plist_new_dict();
        plist_dict_set_item(s96, "type", plist_new_uint(96));
        plist_dict_set_item(s96, "ct", plist_new_uint(4));
        plist_dict_set_item(s96, "spf", plist_new_uint(1024));
        plist_dict_set_item(s96, "controlPort", plist_new_uint(ctrl_port));
        plist_dict_set_item(s96, "audioFormat", plist_new_uint(0x400000));
        plist_dict_set_item(s96, "isMedia", plist_new_bool(1));
        plist_array_append_item(astreams, s96);
        plist_dict_set_item(setup3, "streams", astreams);
        st = rtsp_plist("SETUP", "rtsp://fake/1", setup3, &out);
        plist_free(setup3);
        if (st != 200 || !out) die("SETUP (audio) failed");
        plist_t as0 = plist_array_get_item(plist_dict_get_item(out, "streams"), 0);
        uint64_t adport = 0, acport = 0;
        plist_get_uint_val(plist_dict_get_item(as0, "dataPort"), &adport);
        plist_get_uint_val(plist_dict_get_item(as0, "controlPort"), &acport);
        plist_free(out);
        printf("SETUP audio: data port %" PRIu64 ", control port %" PRIu64 "\n", adport, acport);
        audio_fd = ctrl_fd;
        audio_addr.sin_family = control_addr.sin_family = AF_INET;
        inet_pton(AF_INET, host, &audio_addr.sin_addr);
        control_addr.sin_addr = audio_addr.sin_addr;
        audio_addr.sin_port = htons((uint16_t) adport);
        control_addr.sin_port = htons((uint16_t) acport);

        const AVCodec *ac = avcodec_find_encoder(AV_CODEC_ID_AAC);
        aenc = avcodec_alloc_context3(ac);
        aenc->sample_rate = 44100;
        aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&aenc->ch_layout, 2);
        aenc->bit_rate = 128000;
        if (avcodec_open2(aenc, ac, NULL) < 0) die("AAC encoder");
        audio_aes = aes_cbc_init(aeskey, eiv, AES_ENCRYPT);
    }

    st = rtsp("RECORD", "rtsp://fake/1", NULL, NULL, 0, NULL, NULL);
    if (st != 200) die("RECORD failed");

    int data_fd = tcp_connect(host, (int) data_port);
    if (data_fd < 0) die("cannot connect to mirroring data port");

    /* video key: SHA-512("AirPlayStreamKey<id>" + aeskey), same for the IV */
    char salt[64];
    uint8_t vkey[16], viv[16];
    snprintf(salt, sizeof(salt), "AirPlayStreamKey%" PRIu64, stream_id);
    sha512_prefix(salt, aeskey, 16, vkey);
    snprintf(salt, sizeof(salt), "AirPlayStreamIV%" PRIu64, stream_id);
    sha512_prefix(salt, aeskey, 16, viv);
    aes_ctx_t *video_aes = aes_ctr_init(vkey, viv);

    const AVCodec *vc = avcodec_find_encoder_by_name("libx264");
    if (!vc) die("libx264 encoder not available");
    AVCodecContext *venc = avcodec_alloc_context3(vc);
    venc->width = width;
    venc->height = height;
    venc->pix_fmt = AV_PIX_FMT_YUV420P;
    venc->time_base = (AVRational) { 1, 30 };
    venc->framerate = (AVRational) { 30, 1 };
    venc->gop_size = 60;
    venc->max_b_frames = 0;
    venc->bit_rate = 4000000;
    venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    av_opt_set(venc->priv_data, "preset", "ultrafast", 0);
    av_opt_set(venc->priv_data, "tune", "zerolatency", 0);
    av_opt_set(venc->priv_data, "profile", "high", 0);
    if (avcodec_open2(venc, vc, NULL) < 0) die("cannot open libx264");

    /* SPS/PPS from the Annex-B extradata -> avcC record */
    const uint8_t *sps = NULL, *pps = NULL, *nal;
    int sps_len = 0, pps_len = 0, nal_len, pos = 0;
    while (next_nal(venc->extradata, venc->extradata_size, &pos, &nal, &nal_len)) {
        if ((nal[0] & 0x1f) == 7) { sps = nal; sps_len = nal_len; }
        if ((nal[0] & 0x1f) == 8) { pps = nal; pps_len = nal_len; }
    }
    if (!sps || !pps) die("no SPS/PPS from encoder");
    int avcc_len = 11 + sps_len + pps_len;
    uint8_t *avcc = malloc((size_t) avcc_len);
    avcc[0] = 1; avcc[1] = sps[1]; avcc[2] = sps[2]; avcc[3] = sps[3]; avcc[4] = 0xff; avcc[5] = 0xe1;
    put_be16(avcc + 6, (uint16_t) sps_len);
    memcpy(avcc + 8, sps, (size_t) sps_len);
    avcc[8 + sps_len] = 1;
    put_be16(avcc + 9 + sps_len, (uint16_t) pps_len);
    memcpy(avcc + 11 + sps_len, pps, (size_t) pps_len);

    AVFrame *frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = width;
    frame->height = height;
    av_frame_get_buffer(frame, 0);
    AVPacket *pkt = av_packet_alloc();

    AVFrame *aframe = NULL;
    uint16_t aseq = 1;
    uint32_t artp = 1000;
    double aphase = 0.0;
    if (with_audio) {
        aframe = av_frame_alloc();
        aframe->format = AV_SAMPLE_FMT_FLTP;
        aframe->nb_samples = aenc->frame_size;
        aframe->sample_rate = 44100;
        av_channel_layout_default(&aframe->ch_layout, 2);
        av_frame_get_buffer(aframe, 0);
        /* initial sync on the control port: needed before the receiver dequeues audio */
        uint8_t sync[20] = { 0x90, 0xd4, 0x00, 0x07 };
        put_be32(sync + 4, artp);
        uint64_t now = ntp_now();
        put_be32(sync + 8, (uint32_t) (now >> 32));
        put_be32(sync + 12, (uint32_t) now);
        put_be32(sync + 16, artp + 7497);
        sendto(audio_fd, sync, sizeof(sync), 0, (struct sockaddr *) &control_addr, sizeof(control_addr));
    }

    int total = seconds * 30;
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    time_t last_feedback = time(NULL);
    double audio_sent_s = 0.0;
    size_t video_bytes = 0;
    for (int n = 0; n < total; n++) {
        av_frame_make_writable(frame);
        fill_frame(frame, n);
        frame->pts = n;
        if (avcodec_send_frame(venc, frame) < 0) die("encode");
        while (avcodec_receive_packet(venc, pkt) == 0) {
            uint64_t ts = ntp_now();
            bool key_frame = pkt->flags & AV_PKT_FLAG_KEY;
            uint8_t header[128];
            if (key_frame) {
                memset(header, 0, sizeof(header));
                put_le32(header, (uint32_t) avcc_len);
                header[4] = 0x01; header[5] = 0x00; header[6] = 0x16; header[7] = 0x01;
                put_le64(header + 8, ts);
                put_lef(header + 16, (float) width); put_lef(header + 20, (float) height);
                put_lef(header + 40, (float) width); put_lef(header + 44, (float) height);
                put_lef(header + 48, (float) width); put_lef(header + 52, (float) height);
                put_lef(header + 56, (float) width); put_lef(header + 60, (float) height);
                send_all(data_fd, header, sizeof(header));
                send_all(data_fd, avcc, (size_t) avcc_len);
            }
            /* Annex-B -> 4-byte length prefixed, then encrypt with the running CTR stream */
            uint8_t *payload = malloc((size_t) pkt->size + 64);
            int plen = 0;
            pos = 0;
            while (next_nal(pkt->data, pkt->size, &pos, &nal, &nal_len)) {
                int type = nal[0] & 0x1f;
                if (type == 7 || type == 8 || type == 9) continue;
                payload[plen] = (uint8_t) (nal_len >> 24); payload[plen + 1] = (uint8_t) (nal_len >> 16);
                payload[plen + 2] = (uint8_t) (nal_len >> 8); payload[plen + 3] = (uint8_t) nal_len;
                memcpy(payload + plen + 4, nal, (size_t) nal_len);
                plen += 4 + nal_len;
            }
            aes_ctr_encrypt(video_aes, payload, payload, plen);
            memset(header, 0, sizeof(header));
            put_le32(header, (uint32_t) plen);
            header[4] = 0x00; header[5] = key_frame ? 0x10 : 0x00;
            put_le64(header + 8, ts);
            send_all(data_fd, header, sizeof(header));
            send_all(data_fd, payload, (size_t) plen);
            video_bytes += (size_t) plen;
            free(payload);
            av_packet_unref(pkt);
        }

        /* audio: keep the stream 100 ms ahead of the video clock */
        double t = (double) (n + 1) / 30.0;
        while (with_audio && audio_sent_s < t + 0.1) {
            av_frame_make_writable(aframe);
            for (int i = 0; i < aframe->nb_samples; i++) {
                float s = 0.2f * (float) sin(aphase);
                aphase += 2.0 * 3.14159265358979 * 440.0 / 44100.0;
                ((float *) aframe->data[0])[i] = s;
                ((float *) aframe->data[1])[i] = s;
            }
            avcodec_send_frame(aenc, aframe);
            while (avcodec_receive_packet(aenc, pkt) == 0) {
                uint8_t rtp[2048];
                rtp[0] = 0x80; rtp[1] = aseq == 1 ? 0xe0 : 0x60;
                put_be16(rtp + 2, aseq++);
                put_be32(rtp + 4, artp);
                put_be32(rtp + 8, 0x12345678);
                int enc_len = pkt->size / 16 * 16;
                aes_cbc_encrypt(audio_aes, pkt->data, rtp + 12, enc_len);
                aes_cbc_reset(audio_aes);
                memcpy(rtp + 12 + enc_len, pkt->data + enc_len, (size_t) (pkt->size - enc_len));
                sendto(audio_fd, rtp, (size_t) (12 + pkt->size), 0, (struct sockaddr *) &audio_addr, sizeof(audio_addr));
                av_packet_unref(pkt);
            }
            artp += (uint32_t) aframe->nb_samples;
            audio_sent_s += (double) aframe->nb_samples / 44100.0;
        }

        if (time(NULL) - last_feedback >= 2) {
            last_feedback = time(NULL);
            if (rtsp("POST", "/feedback", NULL, NULL, 0, NULL, NULL) != 200) die("feedback rejected");
        }

        /* pace to 30 fps */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double) (now.tv_sec - start.tv_sec) + (double) (now.tv_nsec - start.tv_nsec) / 1e9;
        if (t > elapsed) usleep((useconds_t) ((t - elapsed) * 1e6));
    }
    printf("streamed %d frames (%.1f MB video)%s\n", total, (double) video_bytes / 1e6, with_audio ? " + AAC audio" : "");

    plist_t teardown = plist_new_dict();
    plist_t tstreams = plist_new_array();
    plist_t t110 = plist_new_dict();
    plist_dict_set_item(t110, "type", plist_new_uint(110));
    plist_array_append_item(tstreams, t110);
    if (with_audio) {
        plist_t t96 = plist_new_dict();
        plist_dict_set_item(t96, "type", plist_new_uint(96));
        plist_array_append_item(tstreams, t96);
    }
    plist_dict_set_item(teardown, "streams", tstreams);
    st = rtsp_plist("TEARDOWN", "rtsp://fake/1", teardown, NULL);
    plist_free(teardown);
    printf("TEARDOWN: %d\n", st);
    close(data_fd);
    ntp_running = false;
    pthread_join(ntp_tid, NULL);
    close(rtsp_fd);
    printf("PASS\n");
    return 0;
}
