/*
 * fake_youtube - plays the role of the YouTube app casting a video with AirPlay, to
 * test SwitchAirCast's AirPlay video support without an iPhone.
 *
 * It opens the reverse (PTTH) event channel, sends POST /play with an mlhls:// master
 * playlist, answers the receiver's FCUP playlist requests from a local HLS folder
 * (rewriting segment URLs to segment_base_url, e.g. a local `python3 -m http.server`),
 * then polls /playback-info and exercises pause, resume, scrub and stop.
 *
 *   fake_youtube <receiver-ip> <port> <hls-dir> <segment-base-url>
 *
 * The folder must contain master.m3u8 whose URIs start with mlhls://localhost/.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include <arpa/inet.h>
#include <math.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <plist/plist.h>

#define SESSION_ID "8F2D1C4A-1111-4C5B-9E3A-2A7D4F6B9C01"
#define PREFIX "mlhls://localhost"

static const char *hls_dir;
static const char *segment_base;
static int failures = 0;

static void check(bool ok, const char *what) {
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) failures++;
}

static int tcp_connect(const char *ip, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    if (connect(fd, (struct sockaddr *) &a, sizeof(a)) < 0) {
        perror("connect");
        exit(1);
    }
    return fd;
}

static void set_timeout(int fd, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static void send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            fprintf(stderr, "send failed\n");
            exit(1);
        }
        p += n;
        len -= (size_t) n;
    }
}

/* Reads one HTTP message (request or response). Returns the start line in first_line,
 * the body (malloc'd) in body. Returns false on timeout/close. */
static bool read_message(int fd, char *first_line, size_t fl_len, char **body, size_t *body_len) {
    char buf[16384];
    size_t got = 0;
    char *end = NULL;
    while (!end) {
        ssize_t r = recv(fd, buf + got, sizeof(buf) - 1 - got, 0);
        if (r <= 0) return false;
        got += (size_t) r;
        buf[got] = '\0';
        end = strstr(buf, "\r\n\r\n");
    }
    char *eol = strstr(buf, "\r\n");
    size_t n = (size_t) (eol - buf) < fl_len - 1 ? (size_t) (eol - buf) : fl_len - 1;
    memcpy(first_line, buf, n);
    first_line[n] = '\0';
    size_t clen = 0;
    char *cl = strcasestr(buf, "Content-Length:");
    if (cl && cl < end) clen = strtoul(cl + 15, NULL, 10);
    size_t header_len = (size_t) (end - buf) + 4;
    *body = malloc(clen + 1);
    size_t have = got - header_len;
    if (have > clen) have = clen;
    memcpy(*body, buf + header_len, have);
    while (have < clen) {
        ssize_t r = recv(fd, *body + have, clen - have, 0);
        if (r <= 0) return false;
        have += (size_t) r;
    }
    (*body)[clen] = '\0';
    *body_len = clen;
    return true;
}

/* Sends a request on the forward channel; returns the status code (-1 if the receiver
 * closed the connection), body in *body. */
static int try_request(int fd, const char *method, const char *url, const char *ctype, const void *data, size_t len,
                       char **body, size_t *body_len) {
    char hdr[1024];
    int n = snprintf(hdr, sizeof(hdr),
                     "%s %s HTTP/1.1\r\nX-Apple-Session-ID: " SESSION_ID "\r\nUser-Agent: AirPlay/770.8.1\r\n"
                     "Content-Length: %zu\r\n%s%s%s\r\n",
                     method, url, len, ctype ? "Content-Type: " : "", ctype ? ctype : "", ctype ? "\r\n" : "");
    if (send(fd, hdr, (size_t) n, MSG_NOSIGNAL) != n) return -1;
    if (len) send_all(fd, data, len);
    char line[256];
    char *b = NULL;
    size_t bl = 0;
    if (!read_message(fd, line, sizeof(line), &b, &bl)) {
        free(b);
        return -1;
    }
    int status = 0;
    sscanf(line, "HTTP/1.1 %d", &status);
    if (body) {
        *body = b;
        *body_len = bl;
    } else {
        free(b);
    }
    return status;
}

static int request(int fd, const char *method, const char *url, const char *ctype, const void *data, size_t len,
                   char **body, size_t *body_len) {
    int st = try_request(fd, method, url, ctype, data, len, body, body_len);
    if (st < 0) {
        fprintf(stderr, "no response to %s %s\n", method, url);
        exit(1);
    }
    return st;
}

static int request_plist(int fd, const char *method, const char *url, plist_t dict) {
    char *bin = NULL;
    uint32_t len = 0;
    plist_to_bin(dict, &bin, &len);
    int st = request(fd, method, url, "application/x-apple-binary-plist", bin, len, NULL, NULL);
    plist_mem_free(bin);
    return st;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *d = malloc((size_t) n + 1);
    if (fread(d, 1, (size_t) n, f) != (size_t) n) n = 0;
    d[n] = '\0';
    fclose(f);
    *len = (size_t) n;
    return d;
}

/* media playlists get absolute segment URLs, as YouTube's do */
static char *rewrite_media_playlist(const char *pl, const char *rel_dir) {
    size_t cap = strlen(pl) * 4 + 4096;
    char *out = malloc(cap);
    char *o = out;
    const char *line = pl;
    while (*line) {
        const char *eol = strchr(line, '\n');
        size_t n = eol ? (size_t) (eol - line) : strlen(line);
        if (n && line[0] != '#') {
            o += sprintf(o, "%s/%s/%.*s\n", segment_base, rel_dir, (int) n, line);
        } else if (!strncmp(line, "#EXT-X-MAP:URI=\"", 16)) {
            const char *q = strchr(line + 16, '"');
            o += sprintf(o, "#EXT-X-MAP:URI=\"%s/%s/%.*s\"%.*s\n", segment_base, rel_dir, (int) (q - line - 16),
                         line + 16, (int) (n - (size_t) (q - line)), q + 1);
        } else {
            memcpy(o, line, n);
            o += n;
            *o++ = '\n';
        }
        line += n + (eol ? 1 : 0);
    }
    *o = '\0';
    return out;
}

/* answers FCUP requests on the reverse channel until the receiver stops asking */
static int serve_playlists(int rev, int fwd) {
    int served = 0;
    for (;;) {
        char line[256];
        char *body = NULL;
        size_t body_len = 0;
        set_timeout(rev, served ? 1500 : 8000);
        if (!read_message(rev, line, sizeof(line), &body, &body_len)) break;
        send_all(rev, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n", 38);
        plist_t ev = NULL;
        plist_from_xml(body, (uint32_t) body_len, &ev);
        free(body);
        plist_t reqd = ev ? plist_dict_get_item(ev, "request") : NULL;
        char *url = NULL;
        uint64_t rid = 0;
        plist_get_string_val(plist_dict_get_item(reqd, "FCUP_Response_URL"), &url);
        plist_get_uint_val(plist_dict_get_item(reqd, "FCUP_Response_RequestID"), &rid);
        if (!url || strncmp(url, PREFIX "/", strlen(PREFIX) + 1)) {
            fprintf(stderr, "unexpected event %s\n", line);
            plist_free(ev);
            continue;
        }
        const char *rel = url + strlen(PREFIX) + 1;
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", hls_dir, rel);
        size_t len = 0;
        char *data = read_file(path, &len);
        int status = data ? 200 : 404;
        if (data && strcmp(rel, "master.m3u8")) {
            char dir[256];
            snprintf(dir, sizeof(dir), "%s", rel);
            char *slash = strrchr(dir, '/');
            if (slash) *slash = '\0';
            char *rw = rewrite_media_playlist(data, dir);
            free(data);
            data = rw;
            len = strlen(rw);
        }
        printf("FCUP request %llu: %s -> %d (%zu bytes)\n", (unsigned long long) rid, rel, status, len);
        plist_t act = plist_new_dict();
        plist_dict_set_item(act, "type", plist_new_string("unhandledURLResponse"));
        plist_t params = plist_new_dict();
        plist_dict_set_item(params, "FCUP_Response_URL", plist_new_string(url));
        plist_dict_set_item(params, "FCUP_Response_Data", plist_new_data(data ? data : "", data ? len : 0));
        plist_dict_set_item(params, "FCUP_Response_StatusCode", plist_new_uint((uint64_t) status));
        plist_dict_set_item(params, "FCUP_Response_RequestID", plist_new_uint(rid));
        plist_dict_set_item(act, "params", params);
        request_plist(fwd, "POST", "/action", act);
        plist_free(act);
        plist_free(ev);
        plist_mem_free(url);
        free(data);
        served++;
    }
    return served;
}

typedef struct {
    bool valid;
    double duration, position, rate;
} info_t;

static info_t playback_info(int fwd) {
    info_t info = { 0 };
    char *body = NULL;
    size_t len = 0;
    request(fwd, "GET", "/playback-info", NULL, NULL, 0, &body, &len);
    plist_t p = NULL;
    if (len) plist_from_xml(body, (uint32_t) len, &p);
    if (p) {
        plist_get_real_val(plist_dict_get_item(p, "duration"), &info.duration);
        plist_get_real_val(plist_dict_get_item(p, "position"), &info.position);
        plist_get_real_val(plist_dict_get_item(p, "rate"), &info.rate);
        info.valid = true;
        plist_free(p);
    }
    free(body);
    return info;
}

static info_t poll_until(int fwd, double seconds, bool (*done)(const info_t *)) {
    info_t info = { 0 };
    for (double t = 0; t < seconds; t += 0.5) {
        info = playback_info(fwd);
        if (info.valid) printf("  playback-info: position %6.2f / %.2f, rate %.0f\n", info.position, info.duration, info.rate);
        else printf("  playback-info: (not ready)\n");
        if (done && done(&info)) break;
        usleep(500000);
    }
    return info;
}

static bool is_playing(const info_t *i) { return i->valid && i->rate == 1.0 && i->position > 1.0; }
static bool past_25(const info_t *i) { return i->valid && i->position >= 25.5; }

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <receiver-ip> <port> <hls-dir> <segment-base-url>\n", argv[0]);
        return 2;
    }
    const char *ip = argv[1];
    int port = atoi(argv[2]);
    hls_dir = argv[3];
    segment_base = argv[4];

    int fwd = tcp_connect(ip, port);
    int st = request(fwd, "GET", "/server-info", NULL, NULL, 0, NULL, NULL);
    check(st == 200, "GET /server-info");

    int rev = tcp_connect(ip, port);
    const char rq[] = "POST /reverse HTTP/1.1\r\nUpgrade: PTTH/1.0\r\nConnection: Upgrade\r\n"
                      "X-Apple-Purpose: event\r\nX-Apple-Session-ID: " SESSION_ID "\r\nContent-Length: 0\r\n\r\n";
    send_all(rev, rq, sizeof(rq) - 1);
    char line[256];
    char *body = NULL;
    size_t blen = 0;
    set_timeout(rev, 5000);
    bool got = read_message(rev, line, sizeof(line), &body, &blen);
    free(body);
    check(got && strstr(line, "101"), "reverse channel upgraded (101 Switching Protocols)");

    plist_t play = plist_new_dict();
    plist_dict_set_item(play, "Content-Location", plist_new_string(PREFIX "/master.m3u8"));
    plist_dict_set_item(play, "uuid", plist_new_string("6B7A2B10-2222-4E2A-8C1D-0F9E8D7C6B5A"));
    plist_dict_set_item(play, "Start-Position-Seconds", plist_new_real(0.0));
    plist_dict_set_item(play, "clientProcName", plist_new_string("YouTube"));
    st = request_plist(fwd, "POST", "/play", play);
    plist_free(play);
    check(st == 200, "POST /play");

    int served = serve_playlists(rev, fwd);
    /* master + v720 + v360 + audio; the VP9 variant must have been filtered out */
    check(served == 4, "receiver fetched master, two H.264 variants and audio (not VP9)");

    printf("waiting for playback to start\n");
    info_t i = poll_until(fwd, 20, is_playing);
    check(is_playing(&i), "video is playing");
    check(fabs(i.duration - 40.0) < 1.0, "duration is 40 s");

    st = request(fwd, "POST", "/rate?value=0.000000", NULL, NULL, 0, NULL, NULL);
    usleep(700000);
    info_t p1 = playback_info(fwd);
    sleep(2);
    info_t p2 = playback_info(fwd);
    printf("  paused at %.2f, two seconds later %.2f (rate %.0f)\n", p1.position, p2.position, p2.rate);
    check(st == 200 && p2.rate == 0.0 && fabs(p2.position - p1.position) < 0.15, "pause holds the position");

    request(fwd, "POST", "/rate?value=1.000000", NULL, NULL, 0, NULL, NULL);
    request(fwd, "POST", "/scrub?position=25.000000", NULL, NULL, 0, NULL, NULL);
    i = poll_until(fwd, 8, past_25);
    check(past_25(&i), "scrub to 25 s, playing on from there");

    st = request(fwd, "POST", "/stop", NULL, NULL, 0, NULL, NULL);
    check(st == 200, "POST /stop");
    usleep(500000);
    i = playback_info(fwd);
    check(!i.valid, "player stopped (no playback information)");

    /* a second video that starts near the end: plays out, then the receiver ends the session */
    play = plist_new_dict();
    plist_dict_set_item(play, "Content-Location", plist_new_string(PREFIX "/master.m3u8"));
    plist_dict_set_item(play, "uuid", plist_new_string("6B7A2B10-3333-4E2A-8C1D-0F9E8D7C6B5A"));
    plist_dict_set_item(play, "Start-Position-Seconds", plist_new_real(34.0));
    plist_dict_set_item(play, "clientProcName", plist_new_string("YouTube"));
    st = request_plist(fwd, "POST", "/play", play);
    plist_free(play);
    check(st == 200, "POST /play (second video, start at 34 s)");
    served = serve_playlists(rev, fwd);
    check(served == 4, "playlists fetched again for the second video");
    bool started_late = false, closed = false;
    for (int t = 0; t < 40 && !closed; t++) {
        char *b = NULL;
        size_t bl = 0;
        int r = try_request(fwd, "GET", "/playback-info", NULL, NULL, 0, &b, &bl);
        plist_t p = NULL;
        if (r > 0 && bl) plist_from_xml(b, (uint32_t) bl, &p);
        double pos = -1, dur = 0;
        if (p) {
            plist_get_real_val(plist_dict_get_item(p, "position"), &pos);
            plist_get_real_val(plist_dict_get_item(p, "duration"), &dur);
            plist_free(p);
            started_late |= pos >= 33.5;
        }
        printf("  playback-info -> %d, position %.2f / %.2f\n", r, pos, dur);
        free(b);
        closed = (r < 0);
        usleep(500000);
    }
    check(started_late, "second video started at its start position");
    check(closed, "receiver reported the end and closed the session");

    close(rev);
    close(fwd);
    printf("%s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
