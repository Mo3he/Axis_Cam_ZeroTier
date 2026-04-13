/**
 * ZeroTier userspace VPN proxy for Axis cameras (ACAP).
 *
 * Runs entirely in userspace via libzt (ZeroTier SDK + lwIP) — no kernel TUN
 * device, no CAP_NET_ADMIN, no root required.
 *
 * Network access model:
 *   - Transparent TCP port forwarding for common camera ports (80, 443, 554)
 *     → VPN peers can browse/stream directly to the ZeroTier IP with no config
 *   - SOCKS5 proxy on port 1080 → full access to any camera port without
 *     needing per-port forwarders; configure your browser/client once
 *
 * Config is read from CONFIG_FILE (written by the C ACAP binary).
 * Reloads on SIGUSR1 or when the config file modification time changes.
 */

#include <ZeroTierSockets.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define APP_NAME          "ZeroTier_VPN"
#define DEFAULT_CONFIG    "/usr/local/packages/ZeroTier_VPN/config.txt"
#define STATE_DIR         "/usr/local/packages/ZeroTier_VPN/localdata"
#define RELAY_BUF_SIZE    8192

/* Transparent port-forwarding: ZeroTier-IP:port → 127.0.0.1:port */
static const int FORWARD_PORTS[] = { 80, 443, 554 };
#define N_FORWARD_PORTS   (sizeof(FORWARD_PORTS) / sizeof(FORWARD_PORTS[0]))

/* SOCKS5 proxy port on the ZeroTier interface */
#define SOCKS5_PORT       1080

/* Reload flag set by SIGUSR1 handler */
static volatile sig_atomic_t reload_requested = 0;
static volatile sig_atomic_t shutdown_requested = 0;

/* Server socket FDs — closed on reload to unblock accept loops */
static atomic_int g_srv_fds[N_FORWARD_PORTS + 1]; /* +1 for SOCKS5 */
#define SOCKS5_SRV_IDX N_FORWARD_PORTS

static void close_server_sockets(void) {
    for (size_t i = 0; i < N_FORWARD_PORTS + 1; i++) {
        int fd = atomic_exchange(&g_srv_fds[i], -1);
        if (fd >= 0)
            zts_bsd_close(fd);
    }
}

/* Current network ID (0 = not joined) */
static uint64_t current_nwid = 0;

/* ── config ──────────────────────────────────────────────────────── */

typedef struct {
    char network_id[20];   /* 16-hex-char network ID */
} config_t;

static bool load_config(const char *path, config_t *cfg) {
    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    memset(cfg, 0, sizeof(*cfg));
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* strip newline */
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        nl = strchr(line, '\r');
        if (nl) *nl = '\0';

        /* skip blanks and comments */
        if (line[0] == '\0' || line[0] == '#')
            continue;

        char *eq = strchr(line, '=');
        if (!eq)
            continue;

        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        /* trim leading spaces from key and val */
        while (*key == ' ') key++;
        while (*val == ' ') val++;

        if (strcmp(key, "network_id") == 0) {
            snprintf(cfg->network_id, sizeof(cfg->network_id), "%s", val);
        }
    }
    fclose(f);
    return true;
}

/* ── relay (bidirectional forwarding) ────────────────────────────── */

typedef struct {
    int zt_fd;        /* libzt socket */
    int local_fd;     /* regular POSIX socket */
} relay_ctx_t;

/* Write all bytes, handling partial writes (POSIX socket) */
static ssize_t write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(fd, p, remaining);
        if (n <= 0) return n;
        p += n;
        remaining -= n;
    }
    return (ssize_t)len;
}

/* Write all bytes to a ZT socket */
static ssize_t zts_write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = zts_bsd_write(fd, p, remaining);
        if (n <= 0) return n;
        p += n;
        remaining -= n;
    }
    return (ssize_t)len;
}

/* Thread: read from ZT socket, write to local socket */
static void *zt_to_local(void *arg) {
    relay_ctx_t *ctx = arg;
    char buf[RELAY_BUF_SIZE];
    ssize_t n;
    while ((n = zts_bsd_read(ctx->zt_fd, buf, sizeof(buf))) > 0) {
        if (write_all(ctx->local_fd, buf, n) <= 0)
            break;
    }
    shutdown(ctx->local_fd, SHUT_WR);
    return NULL;
}

/* Thread: read from local socket, write to ZT socket */
static void *local_to_zt(void *arg) {
    relay_ctx_t *ctx = arg;
    char buf[RELAY_BUF_SIZE];
    ssize_t n;
    while ((n = read(ctx->local_fd, buf, sizeof(buf))) > 0) {
        if (zts_write_all(ctx->zt_fd, buf, n) <= 0)
            break;
    }
    zts_bsd_shutdown(ctx->zt_fd, ZTS_SHUT_WR);
    return NULL;
}

/* Run bidirectional relay between ZT and local sockets. Blocks until done. */
static void relay(int zt_fd, int local_fd) {
    relay_ctx_t ctx = { .zt_fd = zt_fd, .local_fd = local_fd };
    pthread_t t1, t2;

    if (pthread_create(&t1, NULL, zt_to_local, &ctx) != 0) {
        zts_bsd_close(zt_fd);
        close(local_fd);
        return;
    }
    if (pthread_create(&t2, NULL, local_to_zt, &ctx) != 0) {
        zts_bsd_close(zt_fd);
        close(local_fd);
        pthread_join(t1, NULL);
        return;
    }

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    zts_bsd_close(zt_fd);
    close(local_fd);
}

/* ── transparent port forwarder ──────────────────────────────────── */

typedef struct {
    int port;
    char zt_addr[ZTS_IP_MAX_STR_LEN];
} forwarder_ctx_t;

/* Handle a single forwarded connection */
static void *handle_forward(void *arg) {
    relay_ctx_t *ctx = arg;
    relay(ctx->zt_fd, ctx->local_fd);
    free(ctx);
    return NULL;
}

/* Accept loop for one forwarded port */
static void *port_forwarder(void *arg) {
    forwarder_ctx_t *fctx = arg;
    int port = fctx->port;

    /* Determine slot index for this port */
    int slot = -1;
    for (size_t i = 0; i < N_FORWARD_PORTS; i++) {
        if (FORWARD_PORTS[i] == port) { slot = (int)i; break; }
    }

    /* Create ZT listening socket */
    int srv = zts_bsd_socket(ZTS_AF_INET, ZTS_SOCK_STREAM, 0);
    if (srv < 0) {
        syslog(LOG_ERR, "proxy: socket for port %d failed", port);
        free(fctx);
        return NULL;
    }

    /* Register so reload can close us */
    if (slot >= 0)
        atomic_store(&g_srv_fds[slot], srv);

    /* Allow rebind after reload without waiting for TIME_WAIT to expire */
    int reuse = 1;
    zts_bsd_setsockopt(srv, ZTS_SOL_SOCKET, ZTS_SO_REUSEADDR, &reuse, sizeof(reuse));

    struct zts_sockaddr_in zaddr;
    memset(&zaddr, 0, sizeof(zaddr));
    zaddr.sin_family = ZTS_AF_INET;
    zaddr.sin_port = htons((uint16_t)port);
    zts_inet_pton(ZTS_AF_INET, fctx->zt_addr, &zaddr.sin_addr);

    /* Retry bind — lwIP may not have fully finished setting up the
       interface by the time zts_addr_is_assigned() returns true */
    {
        int bind_ok = 0;
        for (int attempt = 0; attempt < 10; attempt++) {
            if (zts_bsd_bind(srv, (struct zts_sockaddr *)&zaddr, sizeof(zaddr)) == 0) {
                bind_ok = 1;
                break;
            }
            syslog(LOG_WARNING, "proxy: bind port %d failed (attempt %d/10), retrying...",
                   port, attempt + 1);
            zts_util_delay(1000);
        }
        if (!bind_ok) {
            syslog(LOG_ERR, "proxy: bind port %d failed after 10 attempts", port);
            zts_bsd_close(srv);
            free(fctx);
            return NULL;
        }
    }

    if (zts_bsd_listen(srv, 16) < 0) {
        syslog(LOG_ERR, "proxy: listen port %d failed", port);
        zts_bsd_close(srv);
        free(fctx);
        return NULL;
    }

    syslog(LOG_INFO, "proxy: forwarding %s:%d → 127.0.0.1:%d",
           fctx->zt_addr, port, port);

    while (!shutdown_requested) {
        int client = zts_bsd_accept(srv, NULL, NULL);
        if (client < 0) {
            if (shutdown_requested || atomic_load(&g_srv_fds[slot < 0 ? 0 : slot]) != srv) break;
            syslog(LOG_WARNING, "proxy: accept on port %d: err %d", port, client);
            zts_util_delay(1000);
            continue;
        }

        /* Connect to localhost */
        int local = socket(AF_INET, SOCK_STREAM, 0);
        if (local < 0) {
            zts_bsd_close(client);
            continue;
        }

        struct sockaddr_in laddr;
        memset(&laddr, 0, sizeof(laddr));
        laddr.sin_family = AF_INET;
        laddr.sin_port = htons((uint16_t)port);
        laddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(local, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(local, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(local, (struct sockaddr *)&laddr, sizeof(laddr)) < 0) {
            zts_bsd_close(client);
            close(local);
            continue;
        }

        /* Spawn relay threads */
        relay_ctx_t *rctx = malloc(sizeof(*rctx));
        if (!rctx) {
            zts_bsd_close(client);
            close(local);
            continue;
        }
        rctx->zt_fd = client;
        rctx->local_fd = local;

        pthread_t thr;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thr, &attr, handle_forward, rctx) != 0) {
            zts_bsd_close(client);
            close(local);
            free(rctx);
        }
        pthread_attr_destroy(&attr);
    }

    zts_bsd_close(srv);
    free(fctx);
    return NULL;
}

/* ── SOCKS5 proxy ────────────────────────────────────────────────── */

typedef struct {
    int zt_fd;
} socks5_conn_t;

/**
 * Handle a single SOCKS5 CONNECT request (RFC 1928).
 * The destination host is always replaced with 127.0.0.1 so the proxy
 * only reaches local camera services — it cannot be used as an open proxy.
 */
static void *handle_socks5(void *arg) {
    socks5_conn_t *sc = arg;
    int zt_fd = sc->zt_fd;
    free(sc);

    unsigned char buf[257];

    /* Greeting: VER NMETHODS METHODS */
    if (zts_bsd_read(zt_fd, buf, 2) != 2 || buf[0] != 0x05) goto fail;
    int nmethods = buf[1];
    if (zts_bsd_read(zt_fd, buf, nmethods) != nmethods) goto fail;

    /* Reply: no authentication required */
    unsigned char reply_greeting[] = { 0x05, 0x00 };
    zts_bsd_write(zt_fd, reply_greeting, 2);

    /* Request: VER CMD RSV ATYP ... */
    if (zts_bsd_read(zt_fd, buf, 4) != 4) goto fail;
    if (buf[0] != 0x05 || buf[1] != 0x01) {
        /* only CONNECT supported */
        unsigned char err[] = { 0x05, 0x07, 0x00, 0x01, 0,0,0,0, 0,0 };
        zts_bsd_write(zt_fd, err, sizeof(err));
        goto fail;
    }

    uint16_t port = 0;
    switch (buf[3]) {
    case 0x01: /* IPv4 */
        if (zts_bsd_read(zt_fd, buf, 6) != 6) goto fail;
        port = ((uint16_t)buf[4] << 8) | buf[5];
        break;
    case 0x03: /* Domain name */
        if (zts_bsd_read(zt_fd, buf, 1) != 1) goto fail;
        {
            int name_len = buf[0];
            if (zts_bsd_read(zt_fd, buf, name_len + 2) != name_len + 2) goto fail;
            port = ((uint16_t)buf[name_len] << 8) | buf[name_len + 1];
        }
        break;
    case 0x04: /* IPv6 */
        if (zts_bsd_read(zt_fd, buf, 18) != 18) goto fail;
        port = ((uint16_t)buf[16] << 8) | buf[17];
        break;
    default: {
        unsigned char err[] = { 0x05, 0x08, 0x00, 0x01, 0,0,0,0, 0,0 };
        zts_bsd_write(zt_fd, err, sizeof(err));
        goto fail;
    }
    }

    /* Connect to localhost:port */
    int local = socket(AF_INET, SOCK_STREAM, 0);
    if (local < 0) {
        unsigned char err[] = { 0x05, 0x01, 0x00, 0x01, 0,0,0,0, 0,0 };
        zts_bsd_write(zt_fd, err, sizeof(err));
        goto fail;
    }

    struct sockaddr_in laddr;
    memset(&laddr, 0, sizeof(laddr));
    laddr.sin_family = AF_INET;
    laddr.sin_port = htons(port);
    laddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(local, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(local, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(local, (struct sockaddr *)&laddr, sizeof(laddr)) < 0) {
        close(local);
        unsigned char err[] = { 0x05, 0x04, 0x00, 0x01, 0,0,0,0, 0,0 };
        zts_bsd_write(zt_fd, err, sizeof(err));
        goto fail;
    }

    /* Success reply */
    unsigned char ok[] = {
        0x05, 0x00, 0x00, 0x01,
        127, 0, 0, 1,
        (unsigned char)(port >> 8), (unsigned char)(port & 0xFF)
    };
    zts_bsd_write(zt_fd, ok, sizeof(ok));

    /* Relay */
    relay(zt_fd, local);
    return NULL;

fail:
    zts_bsd_close(zt_fd);
    return NULL;
}

/* SOCKS5 accept loop */
static void *socks5_server(void *arg) {
    const char *zt_addr = arg;

    int srv = zts_bsd_socket(ZTS_AF_INET, ZTS_SOCK_STREAM, 0);
    if (srv < 0) {
        syslog(LOG_ERR, "socks5: socket failed");
        return NULL;
    }

    /* Register so reload can close us */
    atomic_store(&g_srv_fds[SOCKS5_SRV_IDX], srv);

    /* Allow rebind after reload without waiting for TIME_WAIT to expire */
    int reuse = 1;
    zts_bsd_setsockopt(srv, ZTS_SOL_SOCKET, ZTS_SO_REUSEADDR, &reuse, sizeof(reuse));

    struct zts_sockaddr_in zaddr;
    memset(&zaddr, 0, sizeof(zaddr));
    zaddr.sin_family = ZTS_AF_INET;
    zaddr.sin_port = htons(SOCKS5_PORT);
    zts_inet_pton(ZTS_AF_INET, zt_addr, &zaddr.sin_addr);

    /* Retry bind — same timing race as the port forwarders */
    {
        int bind_ok = 0;
        for (int attempt = 0; attempt < 10; attempt++) {
            if (zts_bsd_bind(srv, (struct zts_sockaddr *)&zaddr, sizeof(zaddr)) == 0) {
                bind_ok = 1;
                break;
            }
            syslog(LOG_WARNING, "socks5: bind failed (attempt %d/10), retrying...", attempt + 1);
            zts_util_delay(1000);
        }
        if (!bind_ok) {
            syslog(LOG_ERR, "socks5: bind failed after 10 attempts");
            zts_bsd_close(srv);
            return NULL;
        }
    }
    if (zts_bsd_listen(srv, 32) < 0) {
        syslog(LOG_ERR, "socks5: listen failed");
        zts_bsd_close(srv);
        return NULL;
    }

    syslog(LOG_INFO, "SOCKS5 proxy ready on %s:%d", zt_addr, SOCKS5_PORT);

    while (!shutdown_requested) {
        int client = zts_bsd_accept(srv, NULL, NULL);
        if (client < 0) {
            if (shutdown_requested || atomic_load(&g_srv_fds[SOCKS5_SRV_IDX]) != srv) break;
            zts_util_delay(1000);
            continue;
        }

        socks5_conn_t *sc = malloc(sizeof(*sc));
        if (!sc) { zts_bsd_close(client); continue; }
        sc->zt_fd = client;

        pthread_t thr;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thr, &attr, handle_socks5, sc) != 0) {
            zts_bsd_close(client);
            free(sc);
        }
        pthread_attr_destroy(&attr);
    }

    zts_bsd_close(srv);
    return NULL;
}

/* ── signal handlers ─────────────────────────────────────────────── */

static void sig_handler(int sig) {
    if (sig == SIGUSR1)
        reload_requested = 1;
    else
        shutdown_requested = 1;
}

/* ── main ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    const char *config_path = DEFAULT_CONFIG;
    if (argc > 1)
        config_path = argv[1];

    openlog(APP_NAME, LOG_PID, LOG_USER);
    syslog(LOG_INFO, "zerotier-userspace starting (config: %s)", config_path);

    /* Set up signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);

    /* Ensure state directories exist */
    mkdir(STATE_DIR, 0755);
    {
        char nets_dir[512];
        snprintf(nets_dir, sizeof(nets_dir), "%s/networks.d", STATE_DIR);
        mkdir(nets_dir, 0755);
    }

    /* Initialize ZeroTier from persistent state */
    int rc = zts_init_from_storage(STATE_DIR);
    if (rc != ZTS_ERR_OK) {
        syslog(LOG_ERR, "zts_init_from_storage failed: %d", rc);
        return 1;
    }

    /* Start the ZeroTier node */
    rc = zts_node_start();
    if (rc != ZTS_ERR_OK) {
        syslog(LOG_ERR, "zts_node_start failed: %d", rc);
        return 1;
    }

    syslog(LOG_INFO, "Waiting for ZeroTier node to come online...");
    while (!zts_node_is_online()) {
        if (shutdown_requested) goto cleanup;
        zts_util_delay(200);
    }

    syslog(LOG_INFO, "Node online, ID: %llx",
           (unsigned long long)zts_node_get_id());

    /* Track config file mtime for auto-reload */
    struct stat st;
    time_t last_mtime = 0;
    /* Initialise all server socket slots to -1 */
    for (size_t i = 0; i < N_FORWARD_PORTS + 1; i++)
        atomic_store(&g_srv_fds[i], -1);

    /* Main loop: load config, join network, run proxy */
    while (!shutdown_requested) {
        config_t cfg;
        if (!load_config(config_path, &cfg) || cfg.network_id[0] == '\0') {
            syslog(LOG_INFO, "Config incomplete — waiting for network ID");
            for (int i = 0; i < 50 && !shutdown_requested && !reload_requested; i++)
                zts_util_delay(200);
            if (reload_requested) { reload_requested = 0; continue; }
            continue;
        }

        /* Parse network ID */
        uint64_t nwid = strtoull(cfg.network_id, NULL, 16);
        if (nwid == 0) {
            syslog(LOG_ERR, "Invalid network ID: %s", cfg.network_id);
            for (int i = 0; i < 50 && !shutdown_requested && !reload_requested; i++)
                zts_util_delay(200);
            if (reload_requested) { reload_requested = 0; continue; }
            continue;
        }

        /* Leave old network if switching */
        if (current_nwid != 0 && current_nwid != nwid) {
            syslog(LOG_INFO, "Leaving network %llx", (unsigned long long)current_nwid);
            zts_net_leave(current_nwid);
            current_nwid = 0;
        }

        /* Join network */
        if (current_nwid != nwid) {
            syslog(LOG_INFO, "Joining network %s", cfg.network_id);
            rc = zts_net_join(nwid);
            if (rc != ZTS_ERR_OK) {
                syslog(LOG_ERR, "zts_net_join failed: %d", rc);
                zts_util_delay(5000);
                continue;
            }
            current_nwid = nwid;
        }

        /* Wait for IP address assignment */
        syslog(LOG_INFO, "Waiting for address assignment on network %s "
               "(authorize this node in ZeroTier Central: %llx)",
               cfg.network_id, (unsigned long long)zts_node_get_id());

        int got_addr = 0;
        for (int i = 0; i < 300 && !shutdown_requested && !reload_requested; i++) {
            if (zts_addr_is_assigned(nwid, ZTS_AF_INET)) {
                got_addr = 1;
                break;
            }
            zts_util_delay(1000);
        }

        if (reload_requested) { reload_requested = 0; continue; }
        if (shutdown_requested) break;

        if (!got_addr) {
            syslog(LOG_WARNING, "Timed out waiting for address — "
                   "ensure node %llx is authorized in ZeroTier Central",
                   (unsigned long long)zts_node_get_id());
            zts_util_delay(10000);
            continue;
        }

        /* Get assigned address */
        char zt_addr_str[ZTS_IP_MAX_STR_LEN] = {0};
        zts_addr_get_str(nwid, ZTS_AF_INET, zt_addr_str, sizeof(zt_addr_str));
        syslog(LOG_INFO, "Address assigned: %s on network %s",
               zt_addr_str, cfg.network_id);

        /* Start port forwarders */
        pthread_t fwd_threads[N_FORWARD_PORTS];
        for (size_t i = 0; i < N_FORWARD_PORTS; i++) {
            forwarder_ctx_t *fctx = malloc(sizeof(*fctx));
            if (!fctx) continue;
            fctx->port = FORWARD_PORTS[i];
            snprintf(fctx->zt_addr, sizeof(fctx->zt_addr), "%s", zt_addr_str);
            pthread_create(&fwd_threads[i], NULL, port_forwarder, fctx);
        }

        /* Start SOCKS5 proxy */
        /* zt_addr_str is on the stack but the SOCKS5 server copies what it
           needs before we could possibly overwrite it.  We use a static buffer
           so the thread has a stable pointer. */
        static char socks5_addr[ZTS_IP_MAX_STR_LEN];
        snprintf(socks5_addr, sizeof(socks5_addr), "%s", zt_addr_str);
        pthread_t socks5_thread;
        pthread_create(&socks5_thread, NULL, socks5_server, socks5_addr);

        syslog(LOG_INFO, "ZeroTier VPN is running — "
               "IP: %s | Ports: 80,443,554 | SOCKS5: %s:%d",
               zt_addr_str, zt_addr_str, SOCKS5_PORT);

        /* Snapshot current config mtime so the change-detection loop
           doesn't immediately fire on the write that brought us here */
        if (stat(config_path, &st) == 0)
            last_mtime = st.st_mtime;

        /* Wait for reload or shutdown */
        while (!shutdown_requested && !reload_requested) {
            /* Check for config file changes */
            if (stat(config_path, &st) == 0 && st.st_mtime > last_mtime) {
                last_mtime = st.st_mtime;
                syslog(LOG_INFO, "Config file changed — reloading");
                break;
            }
            zts_util_delay(5000);
        }

        if (reload_requested)
            reload_requested = 0;

        /* Close server sockets so accept-loop threads unblock and exit */
        close_server_sockets();

        /* The port forwarder and SOCKS5 threads will exit when
           shutdown_requested is set or when the ZT node stops.
           For a reload, we leave the network (which tears down
           the ZT sockets) and re-enter the main loop. */
        if (!shutdown_requested && current_nwid != 0) {
            syslog(LOG_INFO, "Reloading — leaving network for rejoin");
            zts_net_leave(current_nwid);
            current_nwid = 0;
            /* Give threads a moment to notice */
            zts_util_delay(2000);
        }
    }

cleanup:
    syslog(LOG_INFO, "Shutting down ZeroTier node");
    if (current_nwid != 0)
        zts_net_leave(current_nwid);
    zts_node_stop();
    syslog(LOG_INFO, "zerotier-userspace stopped");
    closelog();
    return 0;
}
