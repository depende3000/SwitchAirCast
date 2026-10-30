/*
 * SwitchAirCast - DLNA / UPnP media renderer
 *
 * One thread does everything: it answers SSDP searches and announces the renderer on
 * 239.255.255.250:1900, serves the device and service descriptions and the SOAP control
 * requests over HTTP, and sends GENA events (LastChange) to subscribed control points.
 * Requests are handled one at a time with short timeouts; control points send few.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "dlna.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "platform.h"

#ifndef SWITCHCAST_VERSION
#define SWITCHCAST_VERSION "dev"
#endif

#define SSDP_ADDR "239.255.255.250"
#define SSDP_PORT 1900
#define SSDP_MAX_AGE 1800
/* the console may not hear multicast searches (see mdnsd.c), so announce often */
#define ANNOUNCE_INTERVAL_MS 30000
#define TICK_MS 250
#define IO_TIMEOUT_MS 3000
#define MAX_HEADER 16384
#define MAX_BODY (256 * 1024)
#define MAX_ART (4 * 1024 * 1024)
#define MAX_SUBS 8
#define SUB_TIMEOUT_S 1800
#define DLNA_STACK_SIZE (256 * 1024)

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#define SERVER_STRING "SwitchAirCast/" SWITCHCAST_VERSION " UPnP/1.0 DLNADOC/1.50"

/* what FFmpeg on the console can open; control points check this before sending */
#define SINK_PROTOCOLS                                                                                  \
    "http-get:*:video/mp4:*,http-get:*:video/x-m4v:*,http-get:*:video/quicktime:*,"                    \
    "http-get:*:video/x-matroska:*,http-get:*:video/webm:*,http-get:*:video/mpeg:*,"                   \
    "http-get:*:video/mp2t:*,http-get:*:video/vnd.dlna.mpeg-tts:*,http-get:*:video/x-msvideo:*,"       \
    "http-get:*:video/avi:*,http-get:*:video/x-flv:*,http-get:*:video/3gpp:*,"                         \
    "http-get:*:application/vnd.apple.mpegurl:*,http-get:*:application/x-mpegurl:*,"                   \
    "http-get:*:audio/mpeg:*,http-get:*:audio/mp3:*,http-get:*:audio/mp4:*,http-get:*:audio/x-m4a:*,"  \
    "http-get:*:audio/aac:*,http-get:*:audio/vnd.dlna.adts:*,http-get:*:audio/flac:*,"                 \
    "http-get:*:audio/x-flac:*,http-get:*:audio/ogg:*,http-get:*:audio/opus:*,http-get:*:audio/wav:*," \
    "http-get:*:audio/x-wav:*,http-get:*:audio/L16:*,http-get:*:audio/x-ms-wma:*"

/* ---- growing string buffer ---- */

typedef struct {
    char *s;
    size_t len, cap;
    bool oom;
} sbuf_t;

static void sb_free(sbuf_t *b) {
    free(b->s);
    memset(b, 0, sizeof(*b));
}

static bool sb_reserve(sbuf_t *b, size_t extra) {
    if (b->oom) return false;
    if (b->len + extra + 1 <= b->cap) return true;
    size_t cap = b->cap ? b->cap : 1024;
    while (cap < b->len + extra + 1) cap *= 2;
    char *s = realloc(b->s, cap);
    if (!s) {
        b->oom = true;
        return false;
    }
    b->s = s;
    b->cap = cap;
    b->s[b->len] = '\0';
    return true;
}

static void sb_addn(sbuf_t *b, const char *s, size_t n) {
    if (!sb_reserve(b, n)) return;
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
}

static void sb_add(sbuf_t *b, const char *s) {
    sb_addn(b, s, strlen(s));
}

static void sb_printf(sbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb_printf(sbuf_t *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0 || !sb_reserve(b, (size_t) n)) return;
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, (size_t) n + 1, fmt, ap);
    va_end(ap);
    b->len += (size_t) n;
}

/* XML text or attribute value */
static void sb_add_xml(sbuf_t *b, const char *s) {
    const char *run = s;
    for (; *s; s++) {
        const char *rep = NULL;
        switch (*s) {
        case '&': rep = "&amp;"; break;
        case '<': rep = "&lt;"; break;
        case '>': rep = "&gt;"; break;
        case '"': rep = "&quot;"; break;
        case '\'': rep = "&apos;"; break;
        default: continue;
        }
        sb_addn(b, run, (size_t) (s - run));
        sb_add(b, rep);
        run = s + 1;
    }
    sb_addn(b, run, (size_t) (s - run));
}

/* ---- small XML reader (enough for SOAP arguments and DIDL-Lite) ---- */

static void put_utf8(sbuf_t *b, unsigned long c) {
    char u[4];
    size_t n;
    if (c < 0x80) {
        u[0] = (char) c;
        n = 1;
    } else if (c < 0x800) {
        u[0] = (char) (0xC0 | (c >> 6));
        u[1] = (char) (0x80 | (c & 0x3F));
        n = 2;
    } else if (c < 0x10000) {
        u[0] = (char) (0xE0 | (c >> 12));
        u[1] = (char) (0x80 | ((c >> 6) & 0x3F));
        u[2] = (char) (0x80 | (c & 0x3F));
        n = 3;
    } else if (c < 0x110000) {
        u[0] = (char) (0xF0 | (c >> 18));
        u[1] = (char) (0x80 | ((c >> 12) & 0x3F));
        u[2] = (char) (0x80 | ((c >> 6) & 0x3F));
        u[3] = (char) (0x80 | (c & 0x3F));
        n = 4;
    } else {
        return;
    }
    sb_addn(b, u, n);
}

/* Decodes entities in s[0..n); returns a malloc'd string (NULL if out of memory). */
static char *xml_unescape(const char *s, size_t n) {
    sbuf_t b = { 0 };
    sb_reserve(&b, n);
    const char *end = s + n;
    while (s < end) {
        const char *amp = memchr(s, '&', (size_t) (end - s));
        if (!amp) {
            sb_addn(&b, s, (size_t) (end - s));
            break;
        }
        sb_addn(&b, s, (size_t) (amp - s));
        const char *semi = memchr(amp, ';', (size_t) (end - amp));
        if (!semi || semi - amp > 10) {
            sb_addn(&b, "&", 1);
            s = amp + 1;
            continue;
        }
        size_t len = (size_t) (semi - amp - 1);
        const char *ent = amp + 1;
        if (len == 3 && !strncmp(ent, "amp", 3)) sb_add(&b, "&");
        else if (len == 2 && !strncmp(ent, "lt", 2)) sb_add(&b, "<");
        else if (len == 2 && !strncmp(ent, "gt", 2)) sb_add(&b, ">");
        else if (len == 4 && !strncmp(ent, "quot", 4)) sb_add(&b, "\"");
        else if (len == 4 && !strncmp(ent, "apos", 4)) sb_add(&b, "'");
        else if (len > 1 && ent[0] == '#') {
            unsigned long c = (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent + 2, NULL, 16) : strtoul(ent + 1, NULL, 10);
            put_utf8(&b, c);
        } else {
            sb_addn(&b, amp, (size_t) (semi - amp + 1));
        }
        s = semi + 1;
    }
    if (b.oom) {
        sb_free(&b);
        return NULL;
    }
    if (!b.s) return calloc(1, 1);
    return b.s;
}

/* Content of the first element whose name without a namespace prefix is `name`
 * (CurrentURI, dc:title, upnp:albumArtURI...); NULL if there is none. */
static const char *find_element(const char *xml, const char *name, size_t *len) {
    size_t nlen = strlen(name);
    for (const char *p = strchr(xml, '<'); p; p = strchr(p + 1, '<')) {
        if (p[1] == '/' || p[1] == '?' || p[1] == '!') continue;
        const char *tag = p + 1, *q = tag;
        while (*q && !isspace((unsigned char) *q) && *q != '>' && *q != '/') q++;
        size_t tlen = (size_t) (q - tag);
        const char *colon = memchr(tag, ':', tlen);
        const char *local = colon ? colon + 1 : tag;
        if ((size_t) (tag + tlen - local) != nlen || strncmp(local, name, nlen)) continue;
        const char *gt = strchr(q, '>');
        if (!gt) return NULL;
        if (gt[-1] == '/') {
            *len = 0;
            return gt + 1;
        }
        const char *content = gt + 1;
        for (const char *e = strstr(content, "</"); e; e = strstr(e + 2, "</")) {
            if (!strncmp(e + 2, tag, tlen) && (e[2 + tlen] == '>' || isspace((unsigned char) e[2 + tlen]))) {
                *len = (size_t) (e - content);
                return content;
            }
        }
        return NULL;
    }
    return NULL;
}

/* Unescaped content of an element as a malloc'd string, NULL if absent. */
static char *xml_get(const char *xml, const char *name) {
    size_t len;
    const char *v = xml ? find_element(xml, name, &len) : NULL;
    return v ? xml_unescape(v, len) : NULL;
}

static void xml_get_buf(const char *xml, const char *name, char *out, size_t outlen) {
    char *v = xml_get(xml, name);
    snprintf(out, outlen, "%s", v ? v : "");
    free(v);
}

/* ---- helpers ---- */

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

static const char *ci_find(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        if (!strncasecmp(hay, needle, n)) return hay;
    }
    return NULL;
}

/* A header's value from a block of "Name: value\r\n" lines (case-insensitive name). */
static bool header_value(const char *msg, const char *name, char *out, size_t outlen) {
    size_t n = strlen(name);
    for (const char *line = msg; line && *line;) {
        if (!strncasecmp(line, name, n) && line[n] == ':') {
            const char *v = line + n + 1;
            while (*v == ' ' || *v == '\t') v++;
            const char *e = v;
            while (*e && *e != '\r' && *e != '\n') e++;
            while (e > v && (e[-1] == ' ' || e[-1] == '\t')) e--;
            size_t len = (size_t) (e - v) < outlen - 1 ? (size_t) (e - v) : outlen - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return true;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    out[0] = '\0';
    return false;
}

static void format_time(char *buf, size_t len, double s) {
    long t = s > 0 ? (long) s : 0;
    snprintf(buf, len, "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
}

/* "H+:MM:SS[.F]", "MM:SS" or plain seconds */
static bool parse_time(const char *s, double *out) {
    double t = 0;
    int parts = 0;
    while (*s == ' ' || *s == '+') s++;
    for (;;) {
        char *end;
        double v = strtod(s, &end);
        if (end == s || v < 0) return false;
        t = t * 60.0 + v;
        if (++parts == 3 || *end != ':') break;
        s = end + 1;
    }
    *out = t;
    return true;
}

static void set_timeouts(int fd, int ms) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *) &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *) &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

static bool send_all(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, data, len, SEND_FLAGS);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        data += n;
        len -= (size_t) n;
    }
    return true;
}

/* http://host[:port]/path */
static bool parse_http_url(const char *url, char *host, size_t hostlen, int *port, char *path, size_t pathlen) {
    if (strncasecmp(url, "http://", 7)) return false;
    const char *h = url + 7;
    const char *slash = strchr(h, '/');
    const char *hend = slash ? slash : h + strlen(h);
    const char *colon = memchr(h, ':', (size_t) (hend - h));
    const char *name_end = colon ? colon : hend;
    if (name_end == h || (size_t) (name_end - h) >= hostlen) return false;
    memcpy(host, h, (size_t) (name_end - h));
    host[name_end - h] = '\0';
    *port = colon ? atoi(colon + 1) : 80;
    if (*port <= 0 || *port > 65535) return false;
    snprintf(path, pathlen, "%s", slash ? slash : "/");
    return true;
}

static int connect_timeout(const char *host, int port, int ms) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return -1;
        addr.sin_addr = ((struct sockaddr_in *) res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = connect(fd, (struct sockaddr *) &addr, sizeof(addr));
    if (r < 0 && errno == EINPROGRESS) {
        struct pollfd p = { fd, POLLOUT, 0 };
        int err = 0;
        socklen_t elen = sizeof(err);
        if (poll(&p, 1, ms) == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *) &err, &elen) == 0 && !err) r = 0;
    }
    if (r < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    set_timeouts(fd, ms);
    return fd;
}

/* ---- state ---- */

typedef struct {
    char *uri;
    char *meta;   /* DIDL-Lite as sent */
    char title[256];
    char artist[256];
    char album[256];
    char art[1024];
} media_t;

typedef enum { SVC_AVT, SVC_RCS, SVC_CM, SVC_COUNT } service_id_t;

typedef struct {
    bool used;
    service_id_t svc;
    char sid[48];
    char host[64];
    int port;
    char path[256];
    uint32_t seq;
    uint64_t expires_ms;
    bool initial_pending;
    int failures;
} sub_t;

static dlna_callbacks_t cb;
static pthread_t thread;
static bool thread_running = false;
static volatile bool quit = false;
static int http_fd = -1, ssdp_fd = -1;
static int http_port = 0;
static char friendly_name[128];
static char udn[64];
static char ip[64];

/* everything below is used by the DLNA thread only */
static media_t cur, next;
static unsigned media_serial = 0;   /* changes when cur or next changes (for events) */
static char client[128];
static double stopped_seek = 0.0;   /* Seek while stopped: where Play starts */
static bool muted = false;
static int volume_before_mute = 100;
static bool art_pending = false;
static sub_t subs[MAX_SUBS];
static char last_avt_key[160];
static int last_volume = -1;
static bool last_muted = false;
static uint64_t next_announce_ms = 0;
static int announce_burst = 0;

static void media_clear(media_t *m) {
    free(m->uri);
    free(m->meta);
    memset(m, 0, sizeof(*m));
}

static void media_set(media_t *m, char *uri, char *meta) {
    media_clear(m);
    m->uri = uri;
    m->meta = meta;
    if (meta && *meta) {
        xml_get_buf(meta, "title", m->title, sizeof(m->title));
        xml_get_buf(meta, "artist", m->artist, sizeof(m->artist));
        if (!m->artist[0]) xml_get_buf(meta, "creator", m->artist, sizeof(m->artist));
        xml_get_buf(meta, "album", m->album, sizeof(m->album));
        xml_get_buf(meta, "albumArtURI", m->art, sizeof(m->art));
    }
    media_serial++;
}

/* ---- playback ---- */

static void get_playback(dlna_playback_t *pb) {
    memset(pb, 0, sizeof(*pb));
    if (cb.get_playback) cb.get_playback(pb);
}

static void start_current(double start) {
    dlna_media_t m = {
        .url = cur.uri,
        .start = start,
        .title = cur.title,
        .artist = cur.artist,
        .album = cur.album,
        .art_url = cur.art,
        .client = client,
    };
    platform_log("dlna: play %s (start at %.1f s)%s%s", cur.uri, start, cur.title[0] ? ", " : "", cur.title);
    stopped_seek = 0.0;
    art_pending = cur.art[0] != '\0';
    if (cb.play) cb.play(&m);
}

static const char *state_name(const dlna_playback_t *pb) {
    if (!cur.uri) return "NO_MEDIA_PRESENT";
    switch (pb->state) {
    case DLNA_TRANSITIONING: return "TRANSITIONING";
    case DLNA_PLAYING: return "PLAYING";
    case DLNA_PAUSED: return "PAUSED_PLAYBACK";
    default: return "STOPPED";
    }
}

static const char *status_name(const dlna_playback_t *pb) {
    return pb->failed && pb->state == DLNA_STOPPED ? "ERROR_OCCURRED" : "OK";
}

static const char *transport_actions(const dlna_playback_t *pb) {
    if (!cur.uri) return "";
    switch (pb->state) {
    case DLNA_PLAYING: return next.uri ? "Pause,Stop,Seek,Next" : "Pause,Stop,Seek";
    case DLNA_PAUSED: return next.uri ? "Play,Stop,Seek,Next" : "Play,Stop,Seek";
    case DLNA_TRANSITIONING: return "Stop";
    default: return next.uri ? "Play,Seek,Next" : "Play,Seek";
    }
}

static int current_volume(void) {
    if (muted) return volume_before_mute;
    return cb.get_volume ? cb.get_volume() : 100;
}

/* ---- descriptions ---- */

typedef struct {
    const char *name;
    const char *type;
    bool events;
    const char *allowed;   /* comma-separated allowed values, or NULL */
    const char *range;     /* "min,max,step", or NULL */
} var_t;

typedef struct {
    const char *name;
    /* comma-separated arguments: 'i' or 'o' (direction), name, ':', related state variable */
    const char *args;
} action_t;

typedef struct {
    const char *id;     /* AVTransport, also the URL path */
    const char *type;
    const var_t *vars;
    int n_vars;
    const action_t *actions;
    int n_actions;
} service_t;

static const var_t avt_vars[] = {
    { "TransportState", "string", false, "STOPPED,PLAYING,PAUSED_PLAYBACK,TRANSITIONING,NO_MEDIA_PRESENT", NULL },
    { "TransportStatus", "string", false, "OK,ERROR_OCCURRED", NULL },
    { "PlaybackStorageMedium", "string", false, "NONE,NETWORK", NULL },
    { "RecordStorageMedium", "string", false, "NOT_IMPLEMENTED", NULL },
    { "PossiblePlaybackStorageMedia", "string", false, NULL, NULL },
    { "PossibleRecordStorageMedia", "string", false, NULL, NULL },
    { "CurrentPlayMode", "string", false, "NORMAL", NULL },
    { "TransportPlaySpeed", "string", false, "1", NULL },
    { "RecordMediumWriteStatus", "string", false, "NOT_IMPLEMENTED", NULL },
    { "CurrentRecordQualityMode", "string", false, "NOT_IMPLEMENTED", NULL },
    { "PossibleRecordQualityModes", "string", false, NULL, NULL },
    { "NumberOfTracks", "ui4", false, NULL, "0,1,1" },
    { "CurrentTrack", "ui4", false, NULL, "0,1,1" },
    { "CurrentTrackDuration", "string", false, NULL, NULL },
    { "CurrentMediaDuration", "string", false, NULL, NULL },
    { "CurrentTrackMetaData", "string", false, NULL, NULL },
    { "CurrentTrackURI", "string", false, NULL, NULL },
    { "AVTransportURI", "string", false, NULL, NULL },
    { "AVTransportURIMetaData", "string", false, NULL, NULL },
    { "NextAVTransportURI", "string", false, NULL, NULL },
    { "NextAVTransportURIMetaData", "string", false, NULL, NULL },
    { "RelativeTimePosition", "string", false, NULL, NULL },
    { "AbsoluteTimePosition", "string", false, NULL, NULL },
    { "RelativeCounterPosition", "i4", false, NULL, NULL },
    { "AbsoluteCounterPosition", "i4", false, NULL, NULL },
    { "CurrentTransportActions", "string", false, NULL, NULL },
    { "LastChange", "string", true, NULL, NULL },
    { "A_ARG_TYPE_SeekMode", "string", false, "REL_TIME,ABS_TIME,TRACK_NR", NULL },
    { "A_ARG_TYPE_SeekTarget", "string", false, NULL, NULL },
    { "A_ARG_TYPE_InstanceID", "ui4", false, NULL, NULL },
};

#define ID "iInstanceID:A_ARG_TYPE_InstanceID"
static const action_t avt_actions[] = {
    { "SetAVTransportURI", ID ",iCurrentURI:AVTransportURI,iCurrentURIMetaData:AVTransportURIMetaData" },
    { "SetNextAVTransportURI", ID ",iNextURI:NextAVTransportURI,iNextURIMetaData:NextAVTransportURIMetaData" },
    { "GetMediaInfo", ID ",oNrTracks:NumberOfTracks,oMediaDuration:CurrentMediaDuration,oCurrentURI:AVTransportURI,"
                         "oCurrentURIMetaData:AVTransportURIMetaData,oNextURI:NextAVTransportURI,"
                         "oNextURIMetaData:NextAVTransportURIMetaData,oPlayMedium:PlaybackStorageMedium,"
                         "oRecordMedium:RecordStorageMedium,oWriteStatus:RecordMediumWriteStatus" },
    { "GetTransportInfo", ID ",oCurrentTransportState:TransportState,oCurrentTransportStatus:TransportStatus,"
                             "oCurrentSpeed:TransportPlaySpeed" },
    { "GetPositionInfo", ID ",oTrack:CurrentTrack,oTrackDuration:CurrentTrackDuration,"
                            "oTrackMetaData:CurrentTrackMetaData,oTrackURI:CurrentTrackURI,"
                            "oRelTime:RelativeTimePosition,oAbsTime:AbsoluteTimePosition,"
                            "oRelCount:RelativeCounterPosition,oAbsCount:AbsoluteCounterPosition" },
    { "GetDeviceCapabilities", ID ",oPlayMedia:PossiblePlaybackStorageMedia,oRecMedia:PossibleRecordStorageMedia,"
                                  "oRecQualityModes:PossibleRecordQualityModes" },
    { "GetTransportSettings", ID ",oPlayMode:CurrentPlayMode,oRecQualityMode:CurrentRecordQualityMode" },
    { "Stop", ID },
    { "Play", ID ",iSpeed:TransportPlaySpeed" },
    { "Pause", ID },
    { "Seek", ID ",iUnit:A_ARG_TYPE_SeekMode,iTarget:A_ARG_TYPE_SeekTarget" },
    { "Next", ID },
    { "Previous", ID },
    { "SetPlayMode", ID ",iNewPlayMode:CurrentPlayMode" },
    { "GetCurrentTransportActions", ID ",oActions:CurrentTransportActions" },
};

static const var_t rcs_vars[] = {
    { "PresetNameList", "string", false, NULL, NULL },
    { "LastChange", "string", true, NULL, NULL },
    { "Mute", "boolean", false, NULL, NULL },
    { "Volume", "ui2", false, NULL, "0,100,1" },
    { "A_ARG_TYPE_Channel", "string", false, "Master", NULL },
    { "A_ARG_TYPE_InstanceID", "ui4", false, NULL, NULL },
    { "A_ARG_TYPE_PresetName", "string", false, "FactoryDefaults", NULL },
};

#define CH ",iChannel:A_ARG_TYPE_Channel"
static const action_t rcs_actions[] = {
    { "ListPresets", ID ",oCurrentPresetNameList:PresetNameList" },
    { "SelectPreset", ID ",iPresetName:A_ARG_TYPE_PresetName" },
    { "GetMute", ID CH ",oCurrentMute:Mute" },
    { "SetMute", ID CH ",iDesiredMute:Mute" },
    { "GetVolume", ID CH ",oCurrentVolume:Volume" },
    { "SetVolume", ID CH ",iDesiredVolume:Volume" },
};

static const var_t cm_vars[] = {
    { "SourceProtocolInfo", "string", true, NULL, NULL },
    { "SinkProtocolInfo", "string", true, NULL, NULL },
    { "CurrentConnectionIDs", "string", true, NULL, NULL },
    { "A_ARG_TYPE_ConnectionStatus", "string", false, "OK,ContentFormatMismatch,InsufficientBandwidth,UnreliableChannel,Unknown", NULL },
    { "A_ARG_TYPE_ConnectionManager", "string", false, NULL, NULL },
    { "A_ARG_TYPE_Direction", "string", false, "Input,Output", NULL },
    { "A_ARG_TYPE_ProtocolInfo", "string", false, NULL, NULL },
    { "A_ARG_TYPE_ConnectionID", "i4", false, NULL, NULL },
    { "A_ARG_TYPE_AVTransportID", "i4", false, NULL, NULL },
    { "A_ARG_TYPE_RcsID", "i4", false, NULL, NULL },
};

static const action_t cm_actions[] = {
    { "GetProtocolInfo", "oSource:SourceProtocolInfo,oSink:SinkProtocolInfo" },
    { "GetCurrentConnectionIDs", "oConnectionIDs:CurrentConnectionIDs" },
    { "GetCurrentConnectionInfo", "iConnectionID:A_ARG_TYPE_ConnectionID,oRcsID:A_ARG_TYPE_RcsID,"
                                  "oAVTransportID:A_ARG_TYPE_AVTransportID,oProtocolInfo:A_ARG_TYPE_ProtocolInfo,"
                                  "oPeerConnectionManager:A_ARG_TYPE_ConnectionManager,"
                                  "oPeerConnectionID:A_ARG_TYPE_ConnectionID,oDirection:A_ARG_TYPE_Direction,"
                                  "oStatus:A_ARG_TYPE_ConnectionStatus" },
};

#define COUNT(a) ((int) (sizeof(a) / sizeof((a)[0])))

static const service_t services[SVC_COUNT] = {
    [SVC_AVT] = { "AVTransport", "urn:schemas-upnp-org:service:AVTransport:1", avt_vars, COUNT(avt_vars),
                  avt_actions, COUNT(avt_actions) },
    [SVC_RCS] = { "RenderingControl", "urn:schemas-upnp-org:service:RenderingControl:1", rcs_vars, COUNT(rcs_vars),
                  rcs_actions, COUNT(rcs_actions) },
    [SVC_CM] = { "ConnectionManager", "urn:schemas-upnp-org:service:ConnectionManager:1", cm_vars, COUNT(cm_vars),
                 cm_actions, COUNT(cm_actions) },
};

#define DEVICE_TYPE "urn:schemas-upnp-org:device:MediaRenderer:1"

static void build_device_description(sbuf_t *b) {
    sb_add(b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
              "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" xmlns:dlna=\"urn:schemas-dlna-org:device-1-0\">\n"
              "<specVersion><major>1</major><minor>0</minor></specVersion>\n"
              "<device>\n"
              "<deviceType>" DEVICE_TYPE "</deviceType>\n"
              "<friendlyName>");
    sb_add_xml(b, friendly_name);
    sb_add(b, "</friendlyName>\n"
              "<manufacturer>SwitchAirCast</manufacturer>\n"
              "<manufacturerURL>https://github.com/depende3000/SwitchAirCast</manufacturerURL>\n"
              "<modelDescription>AirPlay and DLNA receiver for Nintendo Switch</modelDescription>\n"
              "<modelName>SwitchAirCast</modelName>\n"
              "<modelNumber>" SWITCHCAST_VERSION "</modelNumber>\n"
              "<UDN>");
    sb_add(b, udn);
    sb_add(b, "</UDN>\n<dlna:X_DLNADOC>DMR-1.50</dlna:X_DLNADOC>\n<serviceList>\n");
    for (int i = 0; i < SVC_COUNT; i++) {
        const service_t *s = &services[i];
        sb_printf(b,
                  "<service><serviceType>%s</serviceType><serviceId>urn:upnp-org:serviceId:%s</serviceId>"
                  "<SCPDURL>/%s.xml</SCPDURL><controlURL>/%s/control</controlURL>"
                  "<eventSubURL>/%s/event</eventSubURL></service>\n",
                  s->type, s->id, s->id, s->id, s->id);
    }
    sb_add(b, "</serviceList>\n</device>\n</root>\n");
}

static void build_scpd(sbuf_t *b, const service_t *s) {
    sb_add(b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
              "<scpd xmlns=\"urn:schemas-upnp-org:service-1-0\">\n"
              "<specVersion><major>1</major><minor>0</minor></specVersion>\n<actionList>\n");
    for (int i = 0; i < s->n_actions; i++) {
        const action_t *a = &s->actions[i];
        sb_printf(b, "<action><name>%s</name><argumentList>", a->name);
        for (const char *p = a->args; p && *p;) {
            const char *end = strchr(p, ',');
            if (!end) end = p + strlen(p);
            const char *colon = memchr(p, ':', (size_t) (end - p));
            if (colon) {
                sb_printf(b, "<argument><name>%.*s</name><direction>%s</direction>"
                             "<relatedStateVariable>%.*s</relatedStateVariable></argument>",
                          (int) (colon - p - 1), p + 1, *p == 'o' ? "out" : "in", (int) (end - colon - 1), colon + 1);
            }
            p = *end ? end + 1 : end;
        }
        sb_add(b, "</argumentList></action>\n");
    }
    sb_add(b, "</actionList>\n<serviceStateTable>\n");
    for (int i = 0; i < s->n_vars; i++) {
        const var_t *v = &s->vars[i];
        sb_printf(b, "<stateVariable sendEvents=\"%s\"><name>%s</name><dataType>%s</dataType>", v->events ? "yes" : "no",
                  v->name, v->type);
        if (v->allowed) {
            sb_add(b, "<allowedValueList>");
            for (const char *p = v->allowed; *p;) {
                const char *end = strchr(p, ',');
                if (!end) end = p + strlen(p);
                sb_printf(b, "<allowedValue>%.*s</allowedValue>", (int) (end - p), p);
                p = *end ? end + 1 : end;
            }
            sb_add(b, "</allowedValueList>");
        }
        if (v->range) {
            int lo = 0, hi = 0, step = 1;
            sscanf(v->range, "%d,%d,%d", &lo, &hi, &step);
            sb_printf(b, "<allowedValueRange><minimum>%d</minimum><maximum>%d</maximum><step>%d</step></allowedValueRange>",
                      lo, hi, step);
        }
        sb_add(b, "</stateVariable>\n");
    }
    sb_add(b, "</serviceStateTable>\n</scpd>\n");
}

/* ---- events ---- */

static void build_avt_lastchange(sbuf_t *b) {
    dlna_playback_t pb;
    get_playback(&pb);
    char dur[32];
    format_time(dur, sizeof(dur), pb.duration);
    const char *uri = cur.uri ? cur.uri : "", *meta = cur.meta ? cur.meta : "";
    sb_add(b, "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\"><InstanceID val=\"0\">");
    sb_printf(b, "<TransportState val=\"%s\"/><TransportStatus val=\"%s\"/>", state_name(&pb), status_name(&pb));
    sb_printf(b, "<CurrentTransportActions val=\"%s\"/>", transport_actions(&pb));
    sb_printf(b, "<TransportPlaySpeed val=\"1\"/><CurrentPlayMode val=\"NORMAL\"/>"
                 "<PlaybackStorageMedium val=\"%s\"/>", cur.uri ? "NETWORK" : "NONE");
    sb_printf(b, "<NumberOfTracks val=\"%d\"/><CurrentTrack val=\"%d\"/>", cur.uri ? 1 : 0, cur.uri ? 1 : 0);
    sb_printf(b, "<CurrentTrackDuration val=\"%s\"/><CurrentMediaDuration val=\"%s\"/>", dur, dur);
    static const char *const names[] = { "AVTransportURI", "CurrentTrackURI", "AVTransportURIMetaData",
                                         "CurrentTrackMetaData", "NextAVTransportURI", "NextAVTransportURIMetaData" };
    const char *values[] = { uri, uri, meta, meta, next.uri ? next.uri : "", next.meta ? next.meta : "" };
    for (int i = 0; i < 6; i++) {
        sb_printf(b, "<%s val=\"", names[i]);
        sb_add_xml(b, values[i]);
        sb_add(b, "\"/>");
    }
    sb_add(b, "</InstanceID></Event>");
}

static void build_rcs_lastchange(sbuf_t *b) {
    sb_printf(b, "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\"><InstanceID val=\"0\">"
                 "<Volume channel=\"Master\" val=\"%d\"/><Mute channel=\"Master\" val=\"%d\"/>"
                 "<PresetNameList val=\"FactoryDefaults\"/></InstanceID></Event>",
              current_volume(), muted ? 1 : 0);
}

static void build_propertyset(sbuf_t *b, service_id_t svc) {
    sb_add(b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
              "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">");
    if (svc == SVC_CM) {
        sb_add(b, "<e:property><SourceProtocolInfo></SourceProtocolInfo></e:property>"
                  "<e:property><SinkProtocolInfo>" SINK_PROTOCOLS "</SinkProtocolInfo></e:property>"
                  "<e:property><CurrentConnectionIDs>0</CurrentConnectionIDs></e:property>");
    } else {
        sbuf_t change = { 0 };
        if (svc == SVC_AVT) build_avt_lastchange(&change);
        else build_rcs_lastchange(&change);
        sb_add(b, "<e:property><LastChange>");
        if (change.s) sb_add_xml(b, change.s);
        sb_add(b, "</LastChange></e:property>");
        sb_free(&change);
    }
    sb_add(b, "</e:propertyset>");
}

static void send_event(sub_t *s, const char *body) {
    sbuf_t req = { 0 };
    sb_printf(&req,
              "NOTIFY %s HTTP/1.1\r\nHOST: %s:%d\r\nCONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
              "NT: upnp:event\r\nNTS: upnp:propchange\r\nSID: %s\r\nSEQ: %u\r\nCONTENT-LENGTH: %zu\r\n"
              "Connection: close\r\n\r\n",
              s->path, s->host, s->port, s->sid, (unsigned) s->seq, strlen(body));
    sb_add(&req, body);
    bool ok = false;
    int fd = req.oom ? -1 : connect_timeout(s->host, s->port, 1000);
    if (fd >= 0) {
        ok = send_all(fd, req.s, req.len);
        char reply[256];
        if (ok) (void) recv(fd, reply, sizeof(reply), 0);
        close(fd);
    }
    sb_free(&req);
    /* SEQ wraps to 1, never back to 0 (that is the initial event) */
    s->seq = s->seq == UINT32_MAX ? 1 : s->seq + 1;
    if (ok) {
        s->failures = 0;
    } else if (++s->failures >= 3) {
        platform_log("dlna: dropping event subscription %s (%s:%d unreachable)", s->sid, s->host, s->port);
        s->used = false;
    }
}

static void notify_service(service_id_t svc) {
    sbuf_t body = { 0 };
    for (int i = 0; i < MAX_SUBS; i++) {
        if (!subs[i].used || subs[i].svc != svc) continue;
        if (!body.s) build_propertyset(&body, svc);
        if (body.oom) break;
        send_event(&subs[i], body.s);
    }
    sb_free(&body);
}

/* ---- HTTP server ---- */

typedef struct {
    char method[16];
    char path[512];
    char *headers;      /* "Name: value\r\n" lines */
    char *body;
    size_t body_len;
    char peer[64];
} http_req_t;

static void send_response(int fd, const char *status, const char *type, const char *extra, const char *body,
                          size_t len) {
    sbuf_t h = { 0 };
    sb_printf(&h, "HTTP/1.1 %s\r\nServer: " SERVER_STRING "\r\nContent-Length: %zu\r\nConnection: close\r\n", status, len);
    if (type) sb_printf(&h, "Content-Type: %s\r\n", type);
    if (extra) sb_add(&h, extra);
    sb_add(&h, "\r\n");
    if (!h.oom) {
        send_all(fd, h.s, h.len);
        if (len) send_all(fd, body, len);
    }
    sb_free(&h);
}

static void send_xml(int fd, const char *status, const sbuf_t *b, const char *extra) {
    if (b->oom) {
        send_response(fd, "500 Internal Server Error", NULL, NULL, NULL, 0);
        return;
    }
    send_response(fd, status, "text/xml; charset=\"utf-8\"", extra, b->s ? b->s : "", b->len);
}

/* ---- SOAP actions ---- */

typedef struct {
    sbuf_t out;      /* response arguments */
    int error;       /* UPnP error code, 0 = success */
    const char *error_desc;
} soap_t;

static void out_arg(soap_t *s, const char *name, const char *value) {
    sb_printf(&s->out, "<%s>", name);
    sb_add_xml(&s->out, value ? value : "");
    sb_printf(&s->out, "</%s>", name);
}

static void out_int(soap_t *s, const char *name, long v) {
    sb_printf(&s->out, "<%s>%ld</%s>", name, v, name);
}

static void fail(soap_t *s, int code, const char *desc) {
    s->error = code;
    s->error_desc = desc;
}

static bool is_playable_uri(const char *uri) {
    return !strncasecmp(uri, "http://", 7) || !strncasecmp(uri, "https://", 8);
}

static void avt_action(const char *action, const char *body, soap_t *s) {
    char arg[64];
    xml_get_buf(body, "InstanceID", arg, sizeof(arg));
    if (arg[0] && strcmp(arg, "0")) {
        fail(s, 718, "Invalid InstanceID");
        return;
    }
    dlna_playback_t pb;
    get_playback(&pb);
    char t[32];

    if (!strcmp(action, "SetAVTransportURI")) {
        char *uri = xml_get(body, "CurrentURI");
        char *meta = xml_get(body, "CurrentURIMetaData");
        if (!uri || !*uri) {
            /* an empty URI clears the media */
            free(uri);
            free(meta);
            if (cur.uri && cb.stop) cb.stop();
            media_clear(&cur);
            media_clear(&next);
            media_serial++;
            return;
        }
        if (!is_playable_uri(uri)) {
            free(uri);
            free(meta);
            fail(s, 714, "Illegal MIME-type");
            return;
        }
        media_set(&cur, uri, meta);
        media_clear(&next);
        stopped_seek = 0.0;
        platform_log("dlna: media from %s: %s", client, cur.uri);
        /* while playing, the new media starts right away */
        if (pb.state != DLNA_STOPPED) start_current(0.0);
    } else if (!strcmp(action, "SetNextAVTransportURI")) {
        char *uri = xml_get(body, "NextURI");
        char *meta = xml_get(body, "NextURIMetaData");
        if (!uri || !*uri) {
            free(uri);
            free(meta);
            media_clear(&next);
            media_serial++;
        } else if (!is_playable_uri(uri)) {
            free(uri);
            free(meta);
            fail(s, 714, "Illegal MIME-type");
        } else {
            media_set(&next, uri, meta);
        }
    } else if (!strcmp(action, "Play")) {
        if (pb.state == DLNA_PAUSED) {
            if (cb.set_paused) cb.set_paused(false);
        } else if (pb.state == DLNA_STOPPED) {
            if (!cur.uri) fail(s, 701, "Transition not available");
            else start_current(stopped_seek);
        }
    } else if (!strcmp(action, "Pause")) {
        if (pb.state == DLNA_STOPPED) fail(s, 701, "Transition not available");
        else if (cb.set_paused) cb.set_paused(true);
    } else if (!strcmp(action, "Stop")) {
        stopped_seek = 0.0;
        if (pb.state != DLNA_STOPPED && cb.stop) cb.stop();
    } else if (!strcmp(action, "Seek")) {
        char unit[32], target[64];
        double pos;
        xml_get_buf(body, "Unit", unit, sizeof(unit));
        xml_get_buf(body, "Target", target, sizeof(target));
        if (!strcmp(unit, "REL_TIME") || !strcmp(unit, "ABS_TIME")) {
            if (!parse_time(target, &pos)) {
                fail(s, 711, "Illegal seek target");
            } else if (pb.state == DLNA_STOPPED) {
                stopped_seek = pos;
            } else if (cb.seek) {
                cb.seek(pos);
            }
        } else if (!strcmp(unit, "TRACK_NR")) {
            if (atoi(target) != 1) fail(s, 711, "Illegal seek target");
            else if (pb.state == DLNA_STOPPED) stopped_seek = 0.0;
            else if (cb.seek) cb.seek(0.0);
        } else {
            fail(s, 710, "Seek mode not supported");
        }
    } else if (!strcmp(action, "Next")) {
        if (!next.uri) {
            fail(s, 701, "Transition not available");
        } else {
            media_t n = next;
            memset(&next, 0, sizeof(next));
            media_clear(&cur);
            cur = n;
            media_serial++;
            start_current(0.0);
        }
    } else if (!strcmp(action, "Previous")) {
        fail(s, 701, "Transition not available");
    } else if (!strcmp(action, "SetPlayMode")) {
        xml_get_buf(body, "NewPlayMode", arg, sizeof(arg));
        if (strcmp(arg, "NORMAL")) fail(s, 712, "Play mode not supported");
    } else if (!strcmp(action, "GetMediaInfo")) {
        format_time(t, sizeof(t), pb.duration);
        out_int(s, "NrTracks", cur.uri ? 1 : 0);
        out_arg(s, "MediaDuration", t);
        out_arg(s, "CurrentURI", cur.uri);
        out_arg(s, "CurrentURIMetaData", cur.meta);
        out_arg(s, "NextURI", next.uri);
        out_arg(s, "NextURIMetaData", next.meta);
        out_arg(s, "PlayMedium", cur.uri ? "NETWORK" : "NONE");
        out_arg(s, "RecordMedium", "NOT_IMPLEMENTED");
        out_arg(s, "WriteStatus", "NOT_IMPLEMENTED");
    } else if (!strcmp(action, "GetTransportInfo")) {
        out_arg(s, "CurrentTransportState", state_name(&pb));
        out_arg(s, "CurrentTransportStatus", status_name(&pb));
        out_arg(s, "CurrentSpeed", "1");
    } else if (!strcmp(action, "GetPositionInfo")) {
        char rel[32];
        format_time(t, sizeof(t), pb.duration);
        format_time(rel, sizeof(rel), pb.state == DLNA_STOPPED ? 0.0 : pb.position);
        out_int(s, "Track", cur.uri ? 1 : 0);
        out_arg(s, "TrackDuration", t);
        out_arg(s, "TrackMetaData", cur.meta);
        out_arg(s, "TrackURI", cur.uri);
        out_arg(s, "RelTime", rel);
        out_arg(s, "AbsTime", rel);
        out_int(s, "RelCount", 2147483647);
        out_int(s, "AbsCount", 2147483647);
    } else if (!strcmp(action, "GetDeviceCapabilities")) {
        out_arg(s, "PlayMedia", "NETWORK");
        out_arg(s, "RecMedia", "NOT_IMPLEMENTED");
        out_arg(s, "RecQualityModes", "NOT_IMPLEMENTED");
    } else if (!strcmp(action, "GetTransportSettings")) {
        out_arg(s, "PlayMode", "NORMAL");
        out_arg(s, "RecQualityMode", "NOT_IMPLEMENTED");
    } else if (!strcmp(action, "GetCurrentTransportActions")) {
        out_arg(s, "Actions", transport_actions(&pb));
    } else {
        fail(s, 401, "Invalid Action");
    }
}

static void rcs_action(const char *action, const char *body, soap_t *s) {
    char arg[64];
    xml_get_buf(body, "InstanceID", arg, sizeof(arg));
    if (arg[0] && strcmp(arg, "0")) {
        fail(s, 702, "Invalid InstanceID");
        return;
    }
    if (!strcmp(action, "GetVolume")) {
        out_int(s, "CurrentVolume", current_volume());
    } else if (!strcmp(action, "SetVolume")) {
        xml_get_buf(body, "DesiredVolume", arg, sizeof(arg));
        char *end = NULL;
        long v = strtol(arg, &end, 10);
        if (!arg[0] || *end || v < 0 || v > 100) {
            fail(s, 402, "Invalid Args");
            return;
        }
        muted = false;
        if (cb.set_volume) cb.set_volume((int) v);
    } else if (!strcmp(action, "GetMute")) {
        out_int(s, "CurrentMute", muted ? 1 : 0);
    } else if (!strcmp(action, "SetMute")) {
        xml_get_buf(body, "DesiredMute", arg, sizeof(arg));
        bool m = !strcmp(arg, "1") || !strcasecmp(arg, "true") || !strcasecmp(arg, "yes");
        if (m && !muted) {
            volume_before_mute = cb.get_volume ? cb.get_volume() : 100;
            if (cb.set_volume) cb.set_volume(0);
        } else if (!m && muted && cb.set_volume) {
            cb.set_volume(volume_before_mute);
        }
        muted = m;
    } else if (!strcmp(action, "ListPresets")) {
        out_arg(s, "CurrentPresetNameList", "FactoryDefaults");
    } else if (!strcmp(action, "SelectPreset")) {
        xml_get_buf(body, "PresetName", arg, sizeof(arg));
        if (strcmp(arg, "FactoryDefaults")) fail(s, 701, "Invalid Name");
    } else {
        fail(s, 401, "Invalid Action");
    }
}

static void cm_action(const char *action, const char *body, soap_t *s) {
    if (!strcmp(action, "GetProtocolInfo")) {
        out_arg(s, "Source", "");
        out_arg(s, "Sink", SINK_PROTOCOLS);
    } else if (!strcmp(action, "GetCurrentConnectionIDs")) {
        out_arg(s, "ConnectionIDs", "0");
    } else if (!strcmp(action, "GetCurrentConnectionInfo")) {
        char arg[32];
        xml_get_buf(body, "ConnectionID", arg, sizeof(arg));
        if (strcmp(arg, "0")) {
            fail(s, 706, "Invalid connection reference");
            return;
        }
        out_int(s, "RcsID", 0);
        out_int(s, "AVTransportID", 0);
        out_arg(s, "ProtocolInfo", "");
        out_arg(s, "PeerConnectionManager", "");
        out_int(s, "PeerConnectionID", -1);
        out_arg(s, "Direction", "Input");
        out_arg(s, "Status", "OK");
    } else {
        fail(s, 401, "Invalid Action");
    }
}

/* A name for the control point from its User-Agent ("BubbleUPnP/4.2 ..."). */
static void client_name(const char *ua, const char *peer, char *out, size_t len) {
    if (ci_find(ua, "Windows")) {
        snprintf(out, len, "Windows");
        return;
    }
    size_t n = 0;
    while (ua[n] && ua[n] != '/' && ua[n] != ' ' && ua[n] != '(' && ua[n] != ';') n++;
    /* generic HTTP libraries say nothing about the app */
    if (!n || !strncasecmp(ua, "Dalvik", n) || !strncasecmp(ua, "okhttp", n) || !strncasecmp(ua, "Java", n) ||
        !strncasecmp(ua, "UPnP", n) || !strncasecmp(ua, "Linux", n)) {
        snprintf(out, len, "%s", ci_find(ua, "Android") ? "Android" : peer);
        return;
    }
    snprintf(out, len, "%.*s", (int) n, ua);
}

static void handle_soap(int fd, const http_req_t *r, service_id_t svc) {
    char soapaction[256], ua[256];
    header_value(r->headers, "SOAPACTION", soapaction, sizeof(soapaction));
    header_value(r->headers, "User-Agent", ua, sizeof(ua));
    char *action = strchr(soapaction, '#');
    if (action) {
        action++;
        char *q = strchr(action, '"');
        if (q) *q = '\0';
    }
    if (!action || !*action || !r->body) {
        send_response(fd, "400 Bad Request", NULL, NULL, NULL, 0);
        return;
    }
    if (svc == SVC_AVT && (!strcmp(action, "SetAVTransportURI") || !strcmp(action, "Play"))) {
        client_name(ua, r->peer, client, sizeof(client));
    }

    soap_t s = { { 0 }, 0, NULL };
    if (svc == SVC_AVT) avt_action(action, r->body, &s);
    else if (svc == SVC_RCS) rcs_action(action, r->body, &s);
    else cm_action(action, r->body, &s);

    if (strcmp(action, "GetPositionInfo") && strcmp(action, "GetTransportInfo") && strcmp(action, "GetVolume") &&
        strcmp(action, "GetMute") && strcmp(action, "GetMediaInfo")) {
        platform_log("dlna: %s from %s -> %d", action, r->peer, s.error);
    }

    sbuf_t b = { 0 };
    sb_add(&b, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
               "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
               "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>");
    if (s.error) {
        sb_printf(&b, "<s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>"
                      "<UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>%d</errorCode>"
                      "<errorDescription>%s</errorDescription></UPnPError></detail></s:Fault>",
                  s.error, s.error_desc);
    } else {
        sb_printf(&b, "<u:%sResponse xmlns:u=\"%s\">", action, services[svc].type);
        if (s.out.s) sb_add(&b, s.out.s);
        sb_printf(&b, "</u:%sResponse>", action);
    }
    sb_add(&b, "</s:Body></s:Envelope>");
    send_xml(fd, s.error ? "500 Internal Server Error" : "200 OK", &b, "EXT:\r\n");
    sb_free(&b);
    sb_free(&s.out);
}

/* ---- event subscriptions ---- */

static void new_sid(char *buf, size_t len) {
    unsigned char r[16];
    platform_random(r, sizeof(r));
    snprintf(buf, len, "uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2],
             r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
}

static int parse_timeout(const char *v) {
    if (!strncasecmp(v, "Second-", 7)) {
        int n = atoi(v + 7);
        if (n > 0) return n < 60 ? 60 : n > SUB_TIMEOUT_S ? SUB_TIMEOUT_S : n;
    }
    return SUB_TIMEOUT_S;
}

static sub_t *find_sub(const char *sid, service_id_t svc) {
    for (int i = 0; i < MAX_SUBS; i++) {
        if (subs[i].used && subs[i].svc == svc && !strcmp(subs[i].sid, sid)) return &subs[i];
    }
    return NULL;
}

static void handle_subscribe(int fd, const http_req_t *r, service_id_t svc) {
    char sid[64], callback[512], nt[64], tmo[64], extra[160];
    header_value(r->headers, "SID", sid, sizeof(sid));
    header_value(r->headers, "CALLBACK", callback, sizeof(callback));
    header_value(r->headers, "NT", nt, sizeof(nt));
    header_value(r->headers, "TIMEOUT", tmo, sizeof(tmo));
    int timeout = parse_timeout(tmo);

    if (!strcmp(r->method, "UNSUBSCRIBE")) {
        sub_t *s = find_sub(sid, svc);
        if (s) s->used = false;
        send_response(fd, s ? "200 OK" : "412 Precondition Failed", NULL, NULL, NULL, 0);
        return;
    }
    sub_t *s;
    if (sid[0]) {
        /* renewal */
        if (callback[0] || nt[0] || !(s = find_sub(sid, svc))) {
            send_response(fd, sid[0] && (callback[0] || nt[0]) ? "400 Bad Request" : "412 Precondition Failed", NULL,
                          NULL, NULL, 0);
            return;
        }
    } else {
        char url[512];
        char *lt = strchr(callback, '<'), *gt = lt ? strchr(lt, '>') : NULL;
        if (strcmp(nt, "upnp:event") || !lt || !gt) {
            send_response(fd, "412 Precondition Failed", NULL, NULL, NULL, 0);
            return;
        }
        snprintf(url, sizeof(url), "%.*s", (int) (gt - lt - 1), lt + 1);
        sub_t tmp;
        memset(&tmp, 0, sizeof(tmp));
        if (!parse_http_url(url, tmp.host, sizeof(tmp.host), &tmp.port, tmp.path, sizeof(tmp.path))) {
            send_response(fd, "412 Precondition Failed", NULL, NULL, NULL, 0);
            return;
        }
        /* a free slot, or the one that expires first */
        s = &subs[0];
        for (int i = 0; i < MAX_SUBS; i++) {
            if (!subs[i].used) {
                s = &subs[i];
                break;
            }
            if (subs[i].expires_ms < s->expires_ms) s = &subs[i];
        }
        *s = tmp;
        s->used = true;
        s->svc = svc;
        new_sid(s->sid, sizeof(s->sid));
        s->initial_pending = true;
        platform_log("dlna: %s subscribed to %s events", r->peer, services[svc].id);
    }
    s->expires_ms = now_ms() + (uint64_t) timeout * 1000ULL;
    snprintf(extra, sizeof(extra), "SID: %s\r\nTIMEOUT: Second-%d\r\n", s->sid, timeout);
    send_response(fd, "200 OK", NULL, extra, NULL, 0);
}

/* ---- request dispatch ---- */

static bool service_path(const char *path, const char *suffix, service_id_t *svc) {
    for (int i = 0; i < SVC_COUNT; i++) {
        size_t n = strlen(services[i].id);
        if (path[0] == '/' && !strncmp(path + 1, services[i].id, n) && !strcmp(path + 1 + n, suffix)) {
            *svc = (service_id_t) i;
            return true;
        }
    }
    return false;
}

static void dispatch(int fd, http_req_t *r) {
    /* an absolute URL in the request line */
    char *path = r->path;
    if (!strncasecmp(path, "http://", 7)) {
        path = strchr(path + 7, '/');
        if (!path) path = "/";
    }
    char *query = strchr(path, '?');
    if (query) *query = '\0';

    service_id_t svc;
    sbuf_t b = { 0 };
    if (!strcmp(r->method, "GET") || !strcmp(r->method, "HEAD")) {
        if (!strcmp(path, "/") || !strcmp(path, "/description.xml")) {
            build_device_description(&b);
        } else if (service_path(path, ".xml", &svc)) {
            build_scpd(&b, &services[svc]);
        } else {
            send_response(fd, "404 Not Found", NULL, NULL, NULL, 0);
            return;
        }
        if (!strcmp(r->method, "HEAD")) b.len = 0;
        send_xml(fd, "200 OK", &b, NULL);
        sb_free(&b);
    } else if (!strcmp(r->method, "POST") && service_path(path, "/control", &svc)) {
        handle_soap(fd, r, svc);
    } else if ((!strcmp(r->method, "SUBSCRIBE") || !strcmp(r->method, "UNSUBSCRIBE")) &&
               service_path(path, "/event", &svc)) {
        handle_subscribe(fd, r, svc);
    } else {
        send_response(fd, "405 Method Not Allowed", NULL, NULL, NULL, 0);
    }
}

static void handle_connection(int fd, const struct sockaddr_in *peer) {
    set_timeouts(fd, IO_TIMEOUT_MS);
    http_req_t r;
    memset(&r, 0, sizeof(r));
    inet_ntop(AF_INET, &peer->sin_addr, r.peer, sizeof(r.peer));

    char *buf = malloc(MAX_HEADER + 1);
    size_t len = 0;
    char *end = NULL;
    if (!buf) return;
    while (!end && len < MAX_HEADER) {
        ssize_t n = recv(fd, buf + len, MAX_HEADER - len, 0);
        if (n <= 0) goto out;
        len += (size_t) n;
        buf[len] = '\0';
        end = strstr(buf, "\r\n\r\n");
    }
    if (!end) goto out;
    char *body_start = end + 4;
    end[2] = '\0';   /* headers end with their last "\r\n" */
    if (sscanf(buf, "%15s %511s", r.method, r.path) != 2) goto out;
    r.headers = strstr(buf, "\r\n");
    if (!r.headers) goto out;
    r.headers += 2;

    char value[64];
    size_t clen = 0;
    if (header_value(r.headers, "Content-Length", value, sizeof(value))) clen = (size_t) strtoul(value, NULL, 10);
    if (clen > MAX_BODY) {
        send_response(fd, "413 Payload Too Large", NULL, NULL, NULL, 0);
        goto out;
    }
    if (header_value(r.headers, "Expect", value, sizeof(value)) && !strcasecmp(value, "100-continue")) {
        static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
        send_all(fd, cont, sizeof(cont) - 1);
    }
    if (clen > 0) {
        r.body = malloc(clen + 1);
        if (!r.body) goto out;
        size_t have = len - (size_t) (body_start - buf);
        if (have > clen) have = clen;
        memcpy(r.body, body_start, have);
        while (have < clen) {
            ssize_t n = recv(fd, r.body + have, clen - have, 0);
            if (n <= 0) goto out;
            have += (size_t) n;
        }
        r.body[clen] = '\0';
        r.body_len = clen;
    }
    dispatch(fd, &r);

out:
    free(r.body);
    free(buf);
}

/* ---- SSDP ---- */

#define N_TYPES 6

static const char *nt_type(int i) {
    static const char *const types[N_TYPES] = { "upnp:rootdevice", NULL, DEVICE_TYPE,
                                                "urn:schemas-upnp-org:service:AVTransport:1",
                                                "urn:schemas-upnp-org:service:RenderingControl:1",
                                                "urn:schemas-upnp-org:service:ConnectionManager:1" };
    return i == 1 ? udn : types[i];
}

static void usn_for(int i, char *buf, size_t len) {
    if (i == 1) snprintf(buf, len, "%s", udn);
    else snprintf(buf, len, "%s::%s", udn, nt_type(i));
}

static void ssdp_send(const char *msg, const struct sockaddr_in *to) {
    sendto(ssdp_fd, msg, strlen(msg), 0, (const struct sockaddr *) to, sizeof(*to));
}

static void ssdp_notify(bool alive) {
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(SSDP_PORT);
    inet_pton(AF_INET, SSDP_ADDR, &to.sin_addr);
    for (int i = 0; i < N_TYPES; i++) {
        char msg[768], usn[256];
        usn_for(i, usn, sizeof(usn));
        if (alive) {
            snprintf(msg, sizeof(msg),
                     "NOTIFY * HTTP/1.1\r\nHOST: " SSDP_ADDR ":1900\r\nCACHE-CONTROL: max-age=%d\r\n"
                     "LOCATION: http://%s:%d/description.xml\r\nNT: %s\r\nNTS: ssdp:alive\r\n"
                     "SERVER: " SERVER_STRING "\r\nUSN: %s\r\n\r\n",
                     SSDP_MAX_AGE, ip, http_port, nt_type(i), usn);
        } else {
            snprintf(msg, sizeof(msg),
                     "NOTIFY * HTTP/1.1\r\nHOST: " SSDP_ADDR ":1900\r\nNT: %s\r\nNTS: ssdp:byebye\r\nUSN: %s\r\n\r\n",
                     nt_type(i), usn);
        }
        ssdp_send(msg, &to);
    }
}

static void handle_ssdp(void) {
    char buf[2048];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(ssdp_fd, buf, sizeof(buf) - 1, 0, (struct sockaddr *) &from, &flen);
    if (n <= 0) return;
    buf[n] = '\0';
    if (strncmp(buf, "M-SEARCH * HTTP/1.1", 19)) return;
    char man[64], st[256];
    header_value(buf, "MAN", man, sizeof(man));
    header_value(buf, "ST", st, sizeof(st));
    if (!strstr(man, "ssdp:discover") || !st[0] || !strcmp(ip, "-")) return;
    bool all = !strcmp(st, "ssdp:all");
    for (int i = 0; i < N_TYPES; i++) {
        if (!all && strcmp(st, nt_type(i))) continue;
        char msg[768], usn[256];
        usn_for(i, usn, sizeof(usn));
        snprintf(msg, sizeof(msg),
                 "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=%d\r\nEXT:\r\n"
                 "LOCATION: http://%s:%d/description.xml\r\nSERVER: " SERVER_STRING "\r\nST: %s\r\nUSN: %s\r\n"
                 "Content-Length: 0\r\n\r\n",
                 SSDP_MAX_AGE, ip, http_port, nt_type(i), usn);
        ssdp_send(msg, &from);
    }
}

static void set_multicast_interface(void) {
    struct in_addr iface;
    if (inet_pton(AF_INET, ip, &iface) == 1) {
        setsockopt(ssdp_fd, IPPROTO_IP, IP_MULTICAST_IF, (const char *) &iface, sizeof(iface));
    }
}

static int open_ssdp_socket(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    unsigned char ttl = 4, loop = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, (const char *) &one, sizeof(one));
#endif
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, (const char *) &ttl, sizeof(ttl));
    setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, (const char *) &loop, sizeof(loop));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SSDP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        platform_log("dlna: cannot bind SSDP port %d (errno %d)", SSDP_PORT, errno);
        close(fd);
        return -1;
    }
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    inet_pton(AF_INET, SSDP_ADDR, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, (const char *) &mreq, sizeof(mreq)) < 0) {
        /* searches are not heard, but the periodic announcements still work */
        platform_log("dlna: cannot join the SSDP group (errno %d), announcing only", errno);
    }
    return fd;
}

/* ---- cover art ---- */

/* Plain-HTTP GET (cover art from a media server on the network). */
static bool http_get(const char *url, char **data, int *len) {
    char host[128], path[1024];
    int port;
    if (!parse_http_url(url, host, sizeof(host), &port, path, sizeof(path))) return false;
    int fd = connect_timeout(host, port, 2000);
    if (fd < 0) return false;
    char req[1400];
    snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: " SERVER_STRING "\r\n\r\n", path, host,
             port);
    sbuf_t b = { 0 };
    bool ok = send_all(fd, req, strlen(req));
    while (ok) {
        char chunk[8192];
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n < 0) ok = false;
        if (n <= 0) break;
        if (b.len + (size_t) n > MAX_ART) ok = false;
        else sb_addn(&b, chunk, (size_t) n);
    }
    close(fd);
    char *body = ok && b.s && !b.oom ? strstr(b.s, "\r\n\r\n") : NULL;
    if (!body || strncmp(b.s, "HTTP/1.", 7) || atoi(b.s + 9) != 200) {
        sb_free(&b);
        return false;
    }
    body += 4;
    *len = (int) (b.len - (size_t) (body - b.s));
    *data = malloc((size_t) *len + 1);
    if (*data) memcpy(*data, body, (size_t) *len);
    sb_free(&b);
    return *data != NULL && *len > 0;
}

/* ---- main loop ---- */

static void tick(uint64_t now) {
    static uint64_t next_ip_check = 0;
    if (now >= next_ip_check) {
        next_ip_check = now + 2000;
        char fresh[64];
        if (!platform_get_ip(fresh, sizeof(fresh))) snprintf(fresh, sizeof(fresh), "-");
        if (strcmp(fresh, ip)) {
            snprintf(ip, sizeof(ip), "%s", fresh);
            if (strcmp(ip, "-")) {
                platform_log("dlna: renderer at http://%s:%d/description.xml", ip, http_port);
                set_multicast_interface();
                announce_burst = 3;
                next_announce_ms = now;
            }
        }
    }
    if (strcmp(ip, "-") && now >= next_announce_ms) {
        ssdp_notify(true);
        if (announce_burst > 0) {
            announce_burst--;
            next_announce_ms = now + 1500;
        } else {
            next_announce_ms = now + ANNOUNCE_INTERVAL_MS;
        }
    }

    if (art_pending && cb.set_coverart) {
        art_pending = false;
        char *data = NULL;
        int len = 0;
        if (http_get(cur.art, &data, &len)) cb.set_coverart(data, len);
        else platform_log("dlna: could not fetch cover art %s", cur.art);
        free(data);
    }

    dlna_playback_t pb;
    get_playback(&pb);
    /* the current track finished: continue with the queued one */
    if (cur.uri && pb.state == DLNA_STOPPED && pb.ended && next.uri) {
        media_t n = next;
        memset(&next, 0, sizeof(next));
        media_clear(&cur);
        cur = n;
        media_serial++;
        start_current(0.0);
        get_playback(&pb);
    }

    for (int i = 0; i < MAX_SUBS; i++) {
        sub_t *s = &subs[i];
        if (!s->used) continue;
        if (now > s->expires_ms) {
            s->used = false;
            continue;
        }
        if (s->initial_pending) {
            sbuf_t body = { 0 };
            build_propertyset(&body, s->svc);
            s->initial_pending = false;
            if (!body.oom) send_event(s, body.s);
            sb_free(&body);
        }
    }

    char key[sizeof(last_avt_key)];
    snprintf(key, sizeof(key), "%s|%s|%s|%u|%ld", state_name(&pb), status_name(&pb), transport_actions(&pb),
             media_serial, (long) pb.duration);
    if (strcmp(key, last_avt_key)) {
        snprintf(last_avt_key, sizeof(last_avt_key), "%s", key);
        notify_service(SVC_AVT);
    }
    int vol = current_volume();
    if (vol != last_volume || muted != last_muted) {
        last_volume = vol;
        last_muted = muted;
        notify_service(SVC_RCS);
    }
}

static void *dlna_thread(void *arg) {
    (void) arg;
    uint64_t next_tick = 0;
    while (!quit) {
        struct pollfd fds[2] = { { http_fd, POLLIN, 0 }, { ssdp_fd, POLLIN, 0 } };
        int n = poll(fds, ssdp_fd >= 0 ? 2 : 1, TICK_MS);
        if (quit) break;
        if (n > 0 && (fds[0].revents & POLLIN)) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int fd = accept(http_fd, (struct sockaddr *) &peer, &plen);
            if (fd >= 0) {
                handle_connection(fd, &peer);
                close(fd);
            }
        }
        if (n > 0 && ssdp_fd >= 0 && (fds[1].revents & POLLIN)) handle_ssdp();
        uint64_t now = now_ms();
        if (now >= next_tick) {
            next_tick = now + TICK_MS;
            tick(now);
        }
    }
    if (strcmp(ip, "-") && ssdp_fd >= 0) ssdp_notify(false);
    return NULL;
}

static int open_http_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *) &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool dlna_start(const char *name, const char *device_id, int port, const dlna_callbacks_t *cbs) {
    if (thread_running) return true;
    cb = *cbs;
    snprintf(friendly_name, sizeof(friendly_name), "%s", name);
    /* a stable UUID from the receiver's device id */
    unsigned b[6] = { 0 };
    sscanf(device_id, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    snprintf(udn, sizeof(udn), "uuid:53574143-0000-4000-8000-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4],
             b[5]);

    http_fd = open_http_socket(port);
    if (http_fd < 0 && port) {
        platform_log("dlna: port %d unavailable, using a dynamic port", port);
        http_fd = open_http_socket(0);
    }
    if (http_fd < 0) {
        platform_log("dlna: cannot open the HTTP port (errno %d)", errno);
        return false;
    }
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    getsockname(http_fd, (struct sockaddr *) &addr, &alen);
    http_port = ntohs(addr.sin_port);

    ssdp_fd = open_ssdp_socket();
    if (ssdp_fd < 0) {
        close(http_fd);
        http_fd = -1;
        return false;
    }

    snprintf(ip, sizeof(ip), "-");
    memset(subs, 0, sizeof(subs));
    last_avt_key[0] = '\0';
    last_volume = -1;
    muted = false;
    quit = false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, DLNA_STACK_SIZE);
    thread_running = pthread_create(&thread, &attr, dlna_thread, NULL) == 0;
    pthread_attr_destroy(&attr);
    if (!thread_running) {
        platform_log("dlna: could not start the thread");
        dlna_stop();
        return false;
    }
    platform_log("dlna: renderer \"%s\" (%s) on port %d", friendly_name, udn, http_port);
    return true;
}

void dlna_stop(void) {
    if (thread_running) {
        quit = true;
        pthread_join(thread, NULL);
        thread_running = false;
    }
    if (http_fd >= 0) close(http_fd);
    if (ssdp_fd >= 0) close(ssdp_fd);
    http_fd = ssdp_fd = -1;
    http_port = 0;
    media_clear(&cur);
    media_clear(&next);
    memset(subs, 0, sizeof(subs));
}

int dlna_port(void) {
    return http_port;
}
