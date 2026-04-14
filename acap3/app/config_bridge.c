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
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#define APP_NAME        "ZeroTier_VPN"
#define CONFIG_FILE     "/usr/local/packages/ZeroTier_VPN/config.txt"
#define ZT_BINARY       "/usr/local/packages/ZeroTier_VPN/lib/zerotier-userspace"

static pid_t zt_pid = -1;

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

static void parameter_changed(const gchar *name, const gchar *value,
                               gpointer handle_void_ptr) {
    AXParameter *handle = handle_void_ptr;

    /* strip "root.ZeroTier_VPN." prefix for the log */
    const char *short_name = name;
    const char *prefix = "root." APP_NAME ".";
    if (strncmp(name, prefix, strlen(prefix)) == 0)
        short_name = name + strlen(prefix);

    syslog(LOG_INFO, "parameter changed: %s = %s", short_name, value);

    update_config_file(handle);
    reload_proxy();
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

    update_config_file(handle);
    start_proxy();

    /* Register callbacks for every parameter */
    const char *params[] = { "NetworkID" };
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
