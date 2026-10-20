/*
 * SwitchAirCast - platform layer
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "platform.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <sys/random.h>
#endif

/* Log lines are collected in memory and a background thread writes them a few times a
 * second. Writing and flushing every line to the SD card took several milliseconds, and
 * with debug_log the AirPlay threads spent most of their time waiting for it: YouTube
 * videos timed out while their playlists were read and mirroring connections failed.
 * A crash can lose the last LOG_WRITE_MS of the log. */
#define LOG_BUFFER_SIZE (256 * 1024)
#define LOG_WRITE_MS 200

/* log_mutex guards the buffers and the writer state; file_mutex guards the files and
 * is held while writing. Lock order: file_mutex, then log_mutex. */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;
static char log_bufs[2][LOG_BUFFER_SIZE];
static int log_active = 0;                 /* the buffer platform_log appends to */
static size_t log_len = 0;
static unsigned long log_dropped = 0;      /* lines that did not fit before the next write */
static bool log_writer_running = false;
static volatile bool log_writer_stop = false;
static pthread_t log_writer;
static FILE *log_file = NULL;
static FILE *history_file = NULL;   /* logs/log-<date>_<time>.txt, debug logging only */
static char data_dir[512];

/* Writes the lines collected so far to stdout and the log files; file_mutex must be held. */
static void write_pending_locked(void) {
    pthread_mutex_lock(&log_mutex);
    const char *buf = log_bufs[log_active];
    size_t len = log_len;
    unsigned long dropped = log_dropped;
    log_active = 1 - log_active;
    log_len = 0;
    log_dropped = 0;
    pthread_mutex_unlock(&log_mutex);
    if (len == 0 && dropped == 0) return;

    char note[96];
    int note_len = dropped ? snprintf(note, sizeof(note), "[log] %lu lines dropped (the log fell behind)\n", dropped) : 0;
    FILE *outs[] = { stdout, log_file, history_file };
    for (size_t i = 0; i < sizeof(outs) / sizeof(outs[0]); i++) {
        if (!outs[i]) continue;
        fwrite(buf, 1, len, outs[i]);
        if (note_len > 0) fwrite(note, 1, (size_t) note_len, outs[i]);
        fflush(outs[i]);
    }
}

static void *log_writer_thread(void *arg) {
    (void) arg;
    while (!log_writer_stop) {
        usleep(LOG_WRITE_MS * 1000);
        pthread_mutex_lock(&file_mutex);
        write_pending_locked();
        pthread_mutex_unlock(&file_mutex);
    }
    return NULL;
}

/* Keeps the previous run's log (log-previous.txt) so a crash can still be diagnosed
 * after SwitchAirCast is started again. */
static void open_log(void) {
    char path[600], prev[600];
    snprintf(path, sizeof(path), "%s/log.txt", data_dir);
    snprintf(prev, sizeof(prev), "%s/log-previous.txt", data_dir);
    remove(prev);
    rename(path, prev);
    pthread_mutex_lock(&file_mutex);
    log_file = fopen(path, "w");
    pthread_mutex_unlock(&file_mutex);
    log_writer_stop = false;
    bool running = pthread_create(&log_writer, NULL, log_writer_thread, NULL) == 0;
    pthread_mutex_lock(&log_mutex);
    log_writer_running = running;
    pthread_mutex_unlock(&log_mutex);
}

/* Stops the writer and writes what is left; later lines are written as they come. */
static void close_logs(void) {
    pthread_mutex_lock(&log_mutex);
    bool running = log_writer_running;
    log_writer_running = false;
    pthread_mutex_unlock(&log_mutex);
    if (running) {
        log_writer_stop = true;
        pthread_join(log_writer, NULL);
    }
    pthread_mutex_lock(&file_mutex);
    write_pending_locked();
    if (log_file) fclose(log_file);
    if (history_file) fclose(history_file);
    log_file = history_file = NULL;
    pthread_mutex_unlock(&file_mutex);
}

static void copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return;
    FILE *out = fopen(to, "wb");
    if (out) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
        fclose(out);
    }
    fclose(in);
}

/* Settings and the receiver identity (device_id: paired senders know the console by
 * it) move over from the folder used before the rename to SwitchAirCast. */
static void migrate_old_data(const char *old_dir) {
    static const char *const files[] = { "config.ini", "device_id" };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char from[600], to[600];
        struct stat st;
        snprintf(to, sizeof(to), "%s/%s", data_dir, files[i]);
        if (stat(to, &st) == 0) continue;
        snprintf(from, sizeof(from), "%s/%s", old_dir, files[i]);
        copy_file(from, to);
    }
}

#ifdef __SWITCH__

static bool have_nifm = false;
static bool have_set = false;
static bool have_ssl = false;
static bool have_setsys = false;
static bool have_lbl = false;
static volatile bool backlight_off = false;
static volatile unsigned wake_count = 0;
static AppletHookCookie applet_cookie;
static bool sixaxis_started = false;
static HidSixAxisSensorHandle sixaxis_handle;

/* Never leave the panel dark when the HOME menu, a wake-up or a dock change takes over. */
static void applet_hook(AppletHookType hook, void *param) {
    (void) param;
    bool restore = hook == AppletHookType_OnOperationMode || hook == AppletHookType_OnResume ||
                   (hook == AppletHookType_OnFocusState && appletGetFocusState() != AppletFocusState_InFocus);
    if (!restore) return;
    if (backlight_off && have_lbl) lblSwitchBacklightOn(0);
    backlight_off = false;
    wake_count++;
}
static bool have_pl = false;
static int nxlink_fd = -1;

/* UxPlay runs several threads that block in select()/recv() at the same time
 * (HTTP server, mirror stream, audio RTP, NTP, mDNS). Every blocking call holds
 * one BSD session, so the default of 3 would serialize them. The larger TCP
 * receive window keeps the mirroring stream from stalling on bitrate spikes. */
static const SocketInitConfig socket_configs[] = {
    {
        .tcp_tx_buf_size = 0x8000,
        .tcp_rx_buf_size = 0x40000,
        .tcp_tx_buf_max_size = 0x40000,
        .tcp_rx_buf_max_size = 0x100000,
        .udp_tx_buf_size = 0x2400,
        .udp_rx_buf_size = 0x10000,
        .sb_efficiency = 8,
        .num_bsd_sessions = 12,
        .bsd_service_type = BsdServiceType_User,
    },
    {
        .tcp_tx_buf_size = 0x8000,
        .tcp_rx_buf_size = 0x20000,
        .tcp_tx_buf_max_size = 0x40000,
        .tcp_rx_buf_max_size = 0x80000,
        .udp_tx_buf_size = 0x2400,
        .udp_rx_buf_size = 0xA500,
        .sb_efficiency = 4,
        .num_bsd_sessions = 8,
        .bsd_service_type = BsdServiceType_User,
    },
};

bool platform_init(void) {
    snprintf(data_dir, sizeof(data_dir), "sdmc:/config/SwitchAirCast");
    mkdir("sdmc:/config", 0777);
    mkdir(data_dir, 0777);
    migrate_old_data("sdmc:/config/SwitchCast");
    open_log();

    /* applet mode (launched from the Album) has far less memory and fewer threads */
    u64 mem_total = 0, mem_used = 0;
    svcGetInfo(&mem_total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    AppletType type = appletGetAppletType();
    platform_log("%s mode, memory %llu MB (%llu MB used)",
                 (type == AppletType_Application || type == AppletType_SystemApplication) ? "application" : "applet",
                 (unsigned long long) (mem_total >> 20), (unsigned long long) (mem_used >> 20));

    Result rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    for (size_t i = 0; i < sizeof(socket_configs) / sizeof(socket_configs[0]); i++) {
        rc = socketInitialize(&socket_configs[i]);
        if (R_SUCCEEDED(rc)) {
            platform_log("sockets: using config %zu (%u BSD sessions)", i,
                         socket_configs[i].num_bsd_sessions);
            break;
        }
        platform_log("sockets: config %zu failed (0x%x)", i, rc);
    }
    if (R_FAILED(rc)) {
        rc = socketInitializeDefault();
        if (R_FAILED(rc)) {
            platform_log("sockets: default config failed (0x%x)", rc);
            return false;
        }
        platform_log("sockets: using libnx default config");
    }

    /* forward stdout/stderr to `nxlink -s` when launched from a PC */
    nxlink_fd = nxlinkStdio();

    have_nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
    have_set = R_SUCCEEDED(setInitialize());
    have_setsys = R_SUCCEEDED(setsysInitialize());
    /* FFmpeg's https (AirPlay video segments) runs on the system SSL service */
    have_ssl = R_SUCCEEDED(sslInitialize(3));
    if (!have_ssl) platform_log("ssl service unavailable: https video will not play");
    have_pl = R_SUCCEEDED(plInitialize(PlServiceType_User));
    have_lbl = R_SUCCEEDED(lblInitialize());
    appletHook(&applet_cookie, applet_hook, NULL);
    if (!have_pl) {
        platform_log("pl service unavailable: no system font");
    }
    return true;
}

void platform_exit(void) {
    platform_set_keep_awake(false);
    platform_set_backlight(true);
    appletUnhook(&applet_cookie);
    if (sixaxis_started) hidStopSixAxisSensor(sixaxis_handle);
    if (have_lbl) lblExit();
    if (have_pl) plExit();
    if (have_ssl) sslExit();
    if (have_setsys) setsysExit();
    if (have_set) setExit();
    if (have_nifm) nifmExit();
    if (nxlink_fd >= 0) close(nxlink_fd);
    socketExit();
    close_logs();
}

bool platform_get_ip(char *buf, size_t len) {
    u32 ip = 0;
    if (have_nifm) {
        /* a LAN without internet access is fine for AirPlay, so only the address matters */
        if (R_FAILED(nifmGetCurrentIpAddress(&ip))) ip = 0;
    } else {
        ip = (u32) gethostid();
    }
    if (ip == 0 || ip == htonl(INADDR_LOOPBACK)) {
        snprintf(buf, len, "-");
        return false;
    }
    struct in_addr addr = { .s_addr = ip };
    inet_ntop(AF_INET, &addr, buf, len);
    return true;
}

void platform_get_device_name(char *buf, size_t len) {
    SetSysDeviceNickName nick;
    memset(&nick, 0, sizeof(nick));
    if (have_setsys && R_SUCCEEDED(setsysGetDeviceNickname(&nick)) && nick.nickname[0]) {
        snprintf(buf, len, "%s", nick.nickname);
    } else {
        snprintf(buf, len, "Nintendo Switch");
    }
}

void platform_get_language_tag(char *buf, size_t len) {
    u64 code = 0;
    char tag[9] = { 0 };
    if (have_set && R_SUCCEEDED(setGetSystemLanguage(&code))) memcpy(tag, &code, sizeof(code));
    snprintf(buf, len, "%s", tag[0] ? tag : "en");
}

bool platform_is_oled(void) {
    SetSysProductModel model = SetSysProductModel_Invalid;
    return have_setsys && R_SUCCEEDED(setsysGetProductModel(&model)) && model == SetSysProductModel_Aula;
}

bool platform_is_applet_mode(void) {
    AppletType type = appletGetAppletType();
    return type != AppletType_Application && type != AppletType_SystemApplication;
}

bool platform_is_handheld(void) {
    return appletGetOperationMode() == AppletOperationMode_Handheld;
}

void platform_set_backlight(bool on) {
    if (on == !backlight_off) return;
    if (!on && !platform_is_handheld()) return;
    if (have_lbl) {
        if (on) lblSwitchBacklightOn(0);
        else lblSwitchBacklightOff(0);
    }
    backlight_off = !on;
}

unsigned platform_screen_wake_count(void) {
    return wake_count;
}

int platform_motion_rotation(void) {
    static int rotation = 0;
    if (!platform_is_handheld()) return -1;
    if (!sixaxis_started) {
        if (R_FAILED(hidGetSixAxisSensorHandles(&sixaxis_handle, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld)) ||
            R_FAILED(hidStartSixAxisSensor(sixaxis_handle))) {
            return -1;
        }
        sixaxis_started = true;
    }
    HidSixAxisSensorState st;
    if (hidGetSixAxisSensorStates(sixaxis_handle, &st, 1) == 0) return rotation;
    float x = st.acceleration.x, y = st.acceleration.y;
    /* gravity along the long edge means the console is held upright; hysteresis avoids flapping */
    int next = rotation;
    if (fabsf(x) > 0.7f && fabsf(x) > fabsf(y) + 0.35f) next = x > 0 ? 270 : 90;  /* the opposite mapping turned the picture upside down on an OLED */
    else if (fabsf(y) > 0.7f && fabsf(y) > fabsf(x) + 0.35f) next = 0;
    if (next != rotation) {
        platform_log("motion: rotation %d (acceleration %.2f %.2f %.2f)", next, x, y, st.acceleration.z);
        rotation = next;
    }
    return rotation;
}

void platform_get_languages(char *buf, size_t len) {
    u64 code = 0;
    char lang[9] = { 0 };
    if (have_set && R_SUCCEEDED(setGetSystemLanguage(&code))) {
        /* the u64 holds a language tag such as "es-419" */
        memcpy(lang, &code, sizeof(code));
        char *dash = strchr(lang, '-');
        if (dash) *dash = '\0';
    }
    if (lang[0] && strcmp(lang, "en")) {
        snprintf(buf, len, "%s:en", lang);
    } else {
        snprintf(buf, len, "en");
    }
}

bool platform_get_font(const void **data, size_t *size) {
    PlFontData font;
    if (!have_pl || R_FAILED(plGetSharedFontByType(&font, PlSharedFontType_Standard))) {
        return false;
    }
    *data = font.address;
    *size = font.size;
    return true;
}

void platform_set_keep_awake(bool on) {
    static bool state = false;
    if (state == on) return;
    if (R_SUCCEEDED(appletSetMediaPlaybackState(on))) {
        state = on;
    }
}

void platform_random(void *buf, size_t len) {
    randomGet(buf, len);
}

#else /* desktop build */

static void *font_data = NULL;

bool platform_init(void) {
    const char *dir = getenv("SWITCHCAST_DATA_DIR");
    if (dir && *dir) {
        snprintf(data_dir, sizeof(data_dir), "%s", dir);
    } else {
        const char *home = getenv("HOME");
        snprintf(data_dir, sizeof(data_dir), "%s/.config/switchcast", home ? home : ".");
        char parent[512];
        snprintf(parent, sizeof(parent), "%s/.config", home ? home : ".");
        mkdir(parent, 0755);
    }
    mkdir(data_dir, 0755);
    open_log();
    return true;
}

void platform_exit(void) {
    free(font_data);
    font_data = NULL;
    close_logs();
}

bool platform_get_ip(char *buf, size_t len) {
    /* the address of the interface that routes to the mDNS group */
    uint32_t ip = 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0) {
        struct sockaddr_in remote = { 0 }, local = { 0 };
        socklen_t local_len = sizeof(local);
        remote.sin_family = AF_INET;
        remote.sin_port = htons(5353);
        inet_pton(AF_INET, "224.0.0.251", &remote.sin_addr);
        if (connect(fd, (struct sockaddr *) &remote, sizeof(remote)) == 0 &&
            getsockname(fd, (struct sockaddr *) &local, &local_len) == 0) {
            ip = local.sin_addr.s_addr;
        }
        close(fd);
    }
    if (ip == 0) {
        snprintf(buf, len, "-");
        return false;
    }
    struct in_addr addr = { .s_addr = ip };
    inet_ntop(AF_INET, &addr, buf, len);
    return true;
}

void platform_get_device_name(char *buf, size_t len) {
    char host[128] = { 0 };
    if (gethostname(host, sizeof(host) - 1) == 0 && host[0]) {
        snprintf(buf, len, "SwitchAirCast (%s)", host);
    } else {
        snprintf(buf, len, "SwitchAirCast");
    }
}

void platform_get_language_tag(char *buf, size_t len) {
    /* LANG=es_MX.UTF-8 -> "es-MX" */
    const char *env = getenv("LANG");
    char tag[16] = { 0 };
    for (size_t i = 0; env && env[i] && env[i] != '.' && i < sizeof(tag) - 1; i++) {
        tag[i] = env[i] == '_' ? '-' : env[i];
    }
    snprintf(buf, len, "%s", tag[0] && strcmp(tag, "C") ? tag : "en");
}

bool platform_is_oled(void) {
    return getenv("SWITCHCAST_OLED") != NULL;
}

bool platform_is_applet_mode(void) {
    return getenv("SWITCHCAST_APPLET") != NULL;
}

bool platform_is_handheld(void) {
    return getenv("SWITCHCAST_HANDHELD") != NULL;
}

void platform_set_backlight(bool on) {
    (void) on;
}

unsigned platform_screen_wake_count(void) {
    return 0;
}

int platform_motion_rotation(void) {
    return -1;
}

void platform_get_languages(char *buf, size_t len) {
    char lang[16] = { 0 };
    const char *env = getenv("LANG");
    if (env) {
        for (size_t i = 0; i < sizeof(lang) - 1 && env[i] >= 'a' && env[i] <= 'z'; i++) lang[i] = env[i];
    }
    if (lang[0] && strcmp(lang, "en")) {
        snprintf(buf, len, "%s:en", lang);
    } else {
        snprintf(buf, len, "en");
    }
}

bool platform_get_font(const void **data, size_t *size) {
    static const char *candidates[] = {
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
    };
    const char *env = getenv("SWITCHCAST_FONT");
    for (int i = -1; i < (int) (sizeof(candidates) / sizeof(candidates[0])); i++) {
        const char *path = i < 0 ? env : candidates[i];
        if (!path) continue;
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0) {
            free(font_data);
            font_data = malloc((size_t) n);
            if (font_data && fread(font_data, 1, (size_t) n, f) == (size_t) n) {
                fclose(f);
                *data = font_data;
                *size = (size_t) n;
                return true;
            }
        }
        fclose(f);
    }
    return false;
}

void platform_set_keep_awake(bool on) {
    (void) on;
}

void platform_random(void *buf, size_t len) {
    unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = getrandom(p, len, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        p += n;
        len -= (size_t) n;
    }
}

#endif

const char *platform_data_dir(void) {
    return data_dir;
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(char *const *) a, *(char *const *) b);
}

/* Deletes the oldest log-*.txt files in dir until at most `keep` remain. The names
 * carry the date and time, so they sort oldest first. */
static void prune_log_history(const char *dir, int keep) {
    DIR *d = opendir(dir);
    if (!d) return;
    char *names[256];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < (int) (sizeof(names) / sizeof(names[0]))) {
        size_t len = strlen(e->d_name);
        if (len > 8 && !strncmp(e->d_name, "log-", 4) && !strcmp(e->d_name + len - 4, ".txt")) {
            names[n] = strdup(e->d_name);
            if (names[n]) n++;
        }
    }
    closedir(d);
    qsort(names, (size_t) n, sizeof(names[0]), compare_names);
    for (int i = 0; i < n; i++) {
        if (i < n - keep) {
            char path[1024];
            snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
            remove(path);
        }
        free(names[i]);
    }
}

void platform_keep_log_history(int keep) {
    if (keep < 1 || history_file) return;
    char dir[600], path[700], stamp[32], log_path[600];
    snprintf(dir, sizeof(dir), "%s/logs", data_dir);
    mkdir(dir, 0777);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm);
    snprintf(path, sizeof(path), "%s/log-%s.txt", dir, stamp);
    snprintf(log_path, sizeof(log_path), "%s/log.txt", data_dir);
    prune_log_history(dir, keep - 1);

    /* start with what this run logged before the settings were read */
    pthread_mutex_lock(&file_mutex);
    write_pending_locked();
    copy_file(log_path, path);
    history_file = fopen(path, "a");
    pthread_mutex_unlock(&file_mutex);
    platform_log("log: also writing %s (the newest %d are kept)", path, keep);
}

void platform_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    char out[1100];
    int n = snprintf(out, sizeof(out), "[%5ld.%03ld] %s\n", (long) ts.tv_sec, ts.tv_nsec / 1000000L, line);
    if (n < 0) return;
    if ((size_t) n >= sizeof(out)) {
        n = (int) sizeof(out) - 1;
        out[n - 1] = '\n';
    }

    pthread_mutex_lock(&log_mutex);
    if (log_len + (size_t) n <= LOG_BUFFER_SIZE) {
        memcpy(log_bufs[log_active] + log_len, out, (size_t) n);
        log_len += (size_t) n;
    } else {
        log_dropped++;
    }
    bool running = log_writer_running;
    pthread_mutex_unlock(&log_mutex);

    /* before the writer starts and after it stops, write at once */
    if (!running) {
        pthread_mutex_lock(&file_mutex);
        write_pending_locked();
        pthread_mutex_unlock(&file_mutex);
    }
}
