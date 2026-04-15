/**
 * ACAP 3 parameter bridge for the ZeroTier userspace VPN.
 *
 * Responsibilities:
 *  1. Read ZeroTier parameters from the ACAP parameter store (axparameter).
 *  2. Write them to CONFIG_FILE so the proxy binary can read them.
 *  3. Launch the proxy binary (zerotier-userspace) as a child process.
 *  4. On any parameter change: rewrite CONFIG_FILE and send SIGUSR1 to the
 *     child so it reloads without dropping the tunnel unnecessarily.
 *  5. Watchdog: if the child exits unexpectedly, restart it.
 *
 * Runs as root on ACAP 3 cameras (AXIS OS 9.x / 10.x).
 */

#include <axsdk/axparameter.h>
#include <glib-unix.h>
#include <stdbool.h>
#include <syslog.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#define APP_NAME        "ZeroTier_VPN"
#define CONFIG_FILE     "/usr/local/packages/ZeroTier_VPN/config.txt"
#define ZT_BINARY       "/usr/local/packages/ZeroTier_VPN/lib/zerotier-userspace"
#define PLANET_FILE     "/usr/local/packages/ZeroTier_VPN/localdata/planet"

static pid_t zt_pid = -1;

/* ── base64 decoder ──────────────────────────────────────────────── */

static const signed char b64_table[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-2,-2,-2,-2,-2,-1,-1, /* whitespace = -2 */
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -2,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63, /* '+' '/' */
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-3,-1,-1, /* '0'-'9', '=' = -3 */
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
};

static ssize_t base64_decode(const char *src, unsigned char **out) {
    size_t src_len = strlen(src);
    size_t out_max = (src_len / 4) * 3 + 3;
    unsigned char *buf = malloc(out_max);
    if (!buf) return -1;

    size_t out_pos = 0;
    uint32_t accum = 0;
    int bits = 0;

    for (size_t i = 0; i < src_len; i++) {
        signed char v = b64_table[(unsigned char)src[i]];
        if (v == -2) continue;
        if (v == -3) break;
        if (v < 0)  { free(buf); return -1; }

        accum = (accum << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            buf[out_pos++] = (unsigned char)((accum >> bits) & 0xFF);
        }
    }

    *out = buf;
    return (ssize_t)out_pos;
}

/* ── child process management ────────────────────────────────────── */

static void stop_proxy(void) {
    if (zt_pid <= 0)
        return;

    if (kill(zt_pid, SIGTERM) == 0)
        waitpid(zt_pid, NULL, 0);

    zt_pid = -1;
}

static void start_proxy(void) {
    stop_proxy();

    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "fork failed: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        /* child */
        execl(ZT_BINARY, "zerotier-userspace", CONFIG_FILE, NULL);
        syslog(LOG_ERR, "execl %s failed: %s", ZT_BINARY, strerror(errno));
        _exit(1);
    }
    zt_pid = pid;
    syslog(LOG_INFO, "zerotier-userspace started (pid %d)", zt_pid);
}

static void reload_proxy(void) {
    if (zt_pid > 0 && kill(zt_pid, 0) == 0) {
        /* process alive — ask it to reload */
        kill(zt_pid, SIGUSR1);
    } else {
        /* not running (first start or crashed) */
        start_proxy();
    }
}

/* Watchdog: check the child every 60 s and restart if it has died. */
static gboolean watchdog_cb(gpointer G_GNUC_UNUSED data) {
    if (zt_pid > 0) {
        int status;
        pid_t ret = waitpid(zt_pid, &status, WNOHANG);
        if (ret == zt_pid) {
            syslog(LOG_WARNING, "zerotier-userspace exited (status %d), restarting",
                   WEXITSTATUS(status));
            zt_pid = -1;
            start_proxy();
        }
    }
    return G_SOURCE_CONTINUE;
}

/* ── config file ─────────────────────────────────────────────────── */

static bool update_planet_file(AXParameter *handle) {
    GError *error = NULL;
    gchar *b64 = NULL;

    if (!ax_parameter_get(handle, "PlanetFile", &b64, &error)) {
        if (error) { g_error_free(error); error = NULL; }
        b64 = g_strdup("");
    }

    gchar *trimmed = g_strstrip(b64);

    if (trimmed[0] == '\0') {
        bool changed = (access(PLANET_FILE, F_OK) == 0);
        if (changed) {
            if (remove(PLANET_FILE) != 0)
                syslog(LOG_WARNING, "could not remove planet file: %s", strerror(errno));
            else
                syslog(LOG_INFO, "custom planet file removed — using built-in planet");
        }
        g_free(b64);
        return changed;
    }

    unsigned char *decoded = NULL;
    ssize_t decoded_len = base64_decode(trimmed, &decoded);
    g_free(b64);

    if (decoded_len < 0) {
        syslog(LOG_ERR, "PlanetFile: base64 decode failed");
        return false;
    }
    if (decoded_len < 4) {
        syslog(LOG_ERR, "PlanetFile: decoded data too short (%zd bytes)", decoded_len);
        free(decoded);
        return false;
    }

    bool changed = true;
    FILE *existing = fopen(PLANET_FILE, "rb");
    if (existing) {
        fseek(existing, 0, SEEK_END);
        long existing_len = ftell(existing);
        if (existing_len == decoded_len) {
            rewind(existing);
            unsigned char *existing_buf = malloc((size_t)existing_len);
            if (existing_buf &&
                fread(existing_buf, 1, (size_t)existing_len, existing) == (size_t)existing_len &&
                memcmp(existing_buf, decoded, (size_t)decoded_len) == 0) {
                changed = false;
            }
            free(existing_buf);
        }
        fclose(existing);
    }

    if (changed) {
        char dir[256];
        snprintf(dir, sizeof(dir), "%s", PLANET_FILE);
        char *slash = strrchr(dir, '/');
        if (slash) {
            *slash = '\0';
            mkdir(dir, 0755);
        }

        FILE *f = fopen(PLANET_FILE, "wb");
        if (f) {
            fwrite(decoded, 1, (size_t)decoded_len, f);
            fclose(f);
            chmod(PLANET_FILE, 0600);
            syslog(LOG_INFO, "custom planet file written (%zd bytes)", decoded_len);
        } else {
            syslog(LOG_ERR, "cannot write planet file: %s", strerror(errno));
            changed = false;
        }
    }

    free(decoded);
    return changed;
}

static void update_config_file(AXParameter *handle) {
    GError *error = NULL;
    gchar *network_id = NULL;

    if (!ax_parameter_get(handle, "NetworkID", &network_id, &error)) {
        if (error) { g_error_free(error); error = NULL; }
        network_id = g_strdup("");
    }

    FILE *f = fopen(CONFIG_FILE, "w");
    if (f) {
        fprintf(f, "network_id=%s\n", network_id ? network_id : "");
        fclose(f);
        chmod(CONFIG_FILE, 0600);
        syslog(LOG_INFO, "config updated (network_id=%s)",
               (network_id && *network_id) ? network_id : "(empty)");
    } else {
        syslog(LOG_ERR, "cannot open config file: %s", strerror(errno));
    }

    g_free(network_id);
}

/* ── ACAP parameter callback ─────────────────────────────────────── */

static void parameter_changed(const gchar *name, const gchar G_GNUC_UNUSED *value,
                               gpointer handle_void_ptr) {
    AXParameter *handle = handle_void_ptr;

    const char *short_name = name;
    const char *prefix = "root." APP_NAME ".";
    if (strncmp(name, prefix, strlen(prefix)) == 0)
        short_name = name + strlen(prefix);

    syslog(LOG_INFO, "parameter changed: %s", short_name);

    if (strcmp(short_name, "PlanetFile") == 0) {
        bool changed = update_planet_file(handle);
        update_config_file(handle);
        if (changed) {
            syslog(LOG_INFO, "planet file changed — doing full proxy restart");
            stop_proxy();
            start_proxy();
        }
    } else {
        update_config_file(handle);
        reload_proxy();
    }
}

/* ── signal handler ──────────────────────────────────────────────── */

static gboolean signal_handler(gpointer loop) {
    syslog(LOG_INFO, "stopping");
    stop_proxy();
    g_main_loop_quit((GMainLoop *)loop);
    return G_SOURCE_REMOVE;
}

/* ── main ────────────────────────────────────────────────────────── */

int main(void) {
    GError *error = NULL;

    openlog(APP_NAME, LOG_PID, LOG_USER);
    syslog(LOG_INFO, "starting");

    AXParameter *handle = ax_parameter_new(APP_NAME, &error);
    if (!handle) {
        syslog(LOG_ERR, "ax_parameter_new: %s",
               error ? error->message : "unknown");
        if (error) g_error_free(error);
        return 1;
    }

    update_planet_file(handle);
    update_config_file(handle);
    start_proxy();

    /* Register callbacks for every parameter */
    const char *params[] = { "NetworkID", "PlanetFile" };
    for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
        if (!ax_parameter_register_callback(handle, params[i],
                                            parameter_changed, handle, &error)) {
            syslog(LOG_WARNING, "register callback %s: %s",
                   params[i], error ? error->message : "unknown");
            if (error) { g_error_free(error); error = NULL; }
        }
    }

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    g_unix_signal_add(SIGTERM, signal_handler, loop);
    g_unix_signal_add(SIGINT,  signal_handler, loop);

    /* watchdog every 60 s */
    g_timeout_add_seconds(60, watchdog_cb, NULL);

    syslog(LOG_INFO, "running — waiting for parameter changes");
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    ax_parameter_free(handle);
    return 0;
}
