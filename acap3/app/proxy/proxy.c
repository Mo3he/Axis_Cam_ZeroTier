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
#include <netdb.h>
#include <poll.h>

#define APP_NAME          "ZeroTier_VPN"
#define DEFAULT_CONFIG    "/usr/local/packages/ZeroTier_VPN/config.txt"
#define STATE_DIR         "/usr/local/packages/ZeroTier_VPN/localdata"
#define STATUS_FILE       "/usr/local/packages/ZeroTier_VPN/html/status.json"
#define STATUS_FILE_TMP   "/usr/local/packages/ZeroTier_VPN/html/status.json.tmp"
#define RELAY_BUF_SIZE    8192

/* Transparent port-forwarding: ZeroTier-IP:port → 127.0.0.1:port */
#define MAX_FORWARD_PORTS 16
static const int DEFAULT_FORWARD_PORTS[] = { 80, 443, 554 };
#define DEFAULT_FORWARD_PORT_COUNT (sizeof(DEFAULT_FORWARD_PORTS) / sizeof(DEFAULT_FORWARD_PORTS[0]))
static int g_forward_ports[MAX_FORWARD_PORTS];
static size_t g_forward_port_count;

/* SOCKS5 proxy port on the ZeroTier interface (bound to ZT IP, not loopback) */
#define SOCKS5_PORT        1080

/* ZeroTier's standard transport port, pinned so peers keep their cached path. */
#define ZT_UDP_PORT        9993

/* Default loopback proxy ports — overridden by config file values */
#define DEFAULT_HTTP_PORT   8080
#define DEFAULT_SOCKS5_PORT 1080

/* Reload flags set by signal handlers */
static volatile sig_atomic_t reload_requested = 0;
static volatile sig_atomic_t forward_reload_requested = 0;
static volatile sig_atomic_t shutdown_requested = 0;

/* Bumped to retire the current generation of accept loops. Each loop polls
   this and closes its own listening socket; closing a listening socket from
   another thread while libzt is blocked in accept() deadlocks its stack. */
static atomic_int g_server_epoch = 0;

/* Actual ports bound (0 = not yet bound / failed) */
static atomic_int g_http_port_actual        = 0;
static atomic_int g_local_socks5_port_actual = 0;

/* How long an idle accept loop sleeps before re-checking the epoch. */
#define ACCEPT_POLL_MS 200

static void retire_server_threads(void) {
    atomic_fetch_add(&g_server_epoch, 1);
}

/* Current network ID (0 = not joined) */
static uint64_t current_nwid = 0;

/* ── status file ─────────────────────────────────────────────────── */

/*
 * Write an atomic JSON status file served at /local/ZeroTier_VPN/status.json.
 * The UI reads this as its primary source of truth so it reflects the actual
 * tunnel state rather than relying solely on log parsing.
 *
 * state:      "starting" | "waiting_config" | "waiting_auth" | "connected" | "disconnected"
 * node_id:    hex node ID string, or NULL
 * zt_ip:      assigned ZeroTier IP, or NULL
 * network_id: 16-hex network ID, or NULL
 * http_port:  actual bound HTTP proxy port (0 = not bound)
 * socks5_port: actual bound outbound SOCKS5 port (0 = not bound)
 */
static void write_status(const char *state, const char *node_id,
                         const char *zt_ip, const char *network_id,
                         int http_port, int socks5_port)
{
    FILE *f = fopen(STATUS_FILE_TMP, "w");
    if (!f) return;
    fprintf(f,
        "{\n"
        "  \"state\": \"%s\",\n"
        "  \"node_id\": \"%s\",\n"
        "  \"zt_ip\": \"%s\",\n"
        "  \"network_id\": \"%s\",\n"
        "  \"http_port\": %d,\n"
        "  \"socks5_port\": %d,\n"
        "  \"ts\": %ld\n"
        "}\n",
        state,
        node_id    ? node_id    : "",
        zt_ip      ? zt_ip      : "",
        network_id ? network_id : "",
        http_port, socks5_port,
        (long)time(NULL));
    fclose(f);
    rename(STATUS_FILE_TMP, STATUS_FILE);
    chmod(STATUS_FILE, 0644);
}

/* ── config ──────────────────────────────────────────────────────── */

typedef struct {
    char network_id[20];    /* 16-hex-char network ID */
    int  http_proxy_port;   /* loopback HTTP CONNECT proxy port */
    int  socks5_proxy_port; /* loopback outbound SOCKS5 port */
    char forward_ports[256]; /* comma-separated direct forwarding ports */
} config_t;

static void set_default_forward_ports(void) {
    memcpy(g_forward_ports, DEFAULT_FORWARD_PORTS, sizeof(DEFAULT_FORWARD_PORTS));
    g_forward_port_count = DEFAULT_FORWARD_PORT_COUNT;
}

static void parse_forward_ports(const char *value) {
    char copy[256];
    char *saveptr = NULL;
    char *token;

    g_forward_port_count = 0;
    snprintf(copy, sizeof(copy), "%s", value ? value : "");
    token = strtok_r(copy, ",", &saveptr);
    while (token && g_forward_port_count < MAX_FORWARD_PORTS) {
        char *start = token;
        char *end;
        long port;
        while (*start == ' ' || *start == '\t') start++;
        end = start + strlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;
        *end = '\0';
        port = strtol(start, &end, 10);
        while (*end == ' ' || *end == '\t') end++;
        if (start[0] != '\0' && *end == '\0' && port >= 1 && port <= 65535) {
            bool duplicate = false;
            for (size_t i = 0; i < g_forward_port_count; i++) {
                if (g_forward_ports[i] == (int)port) duplicate = true;
            }
            if (!duplicate) g_forward_ports[g_forward_port_count++] = (int)port;
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
    if (g_forward_port_count == 0) set_default_forward_ports();
}

static bool load_config(const char *path, config_t *cfg) {
    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    memset(cfg, 0, sizeof(*cfg));
    cfg->http_proxy_port   = DEFAULT_HTTP_PORT;
    cfg->socks5_proxy_port = DEFAULT_SOCKS5_PORT;
    snprintf(cfg->forward_ports, sizeof(cfg->forward_ports), "80,443,554");
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
        } else if (strcmp(key, "http_proxy_port") == 0) {
            int p = atoi(val);
            if (p > 0 && p <= 65535) cfg->http_proxy_port = p;
        } else if (strcmp(key, "socks5_proxy_port") == 0) {
            int p = atoi(val);
            if (p > 0 && p <= 65535) cfg->socks5_proxy_port = p;
        } else if (strcmp(key, "forward_ports") == 0) {
            snprintf(cfg->forward_ports, sizeof(cfg->forward_ports), "%s", val);
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
    int my_epoch = atomic_load(&g_server_epoch);

    /* Create ZT listening socket */
    int srv = zts_bsd_socket(ZTS_AF_INET, ZTS_SOCK_STREAM, 0);
    if (srv < 0) {
        syslog(LOG_ERR, "proxy: socket for port %d failed", port);
        free(fctx);
        return NULL;
    }

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
    zts_set_blocking(srv, 0);

    syslog(LOG_INFO, "proxy: forwarding %s:%d → 127.0.0.1:%d",
           fctx->zt_addr, port, port);

    while (!shutdown_requested && atomic_load(&g_server_epoch) == my_epoch) {
        int client = zts_bsd_accept(srv, NULL, NULL);
        if (client < 0) {
            zts_util_delay(ACCEPT_POLL_MS);
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

        /* Clear the connect timeouts — they must not apply to the relay, or an
           idle keep-alive connection would be torn down after 10 s of silence,
           bouncing the web UI back to "System is getting ready". */
        struct timeval no_tv = { .tv_sec = 0, .tv_usec = 0 };
        setsockopt(local, SOL_SOCKET, SO_RCVTIMEO, &no_tv, sizeof(no_tv));
        setsockopt(local, SOL_SOCKET, SO_SNDTIMEO, &no_tv, sizeof(no_tv));

        /* Enable TCP keepalive so a peer that vanishes without a clean FIN is
           eventually reaped instead of leaking a relay thread + fds forever. */
        int keepalive = 1;
        setsockopt(local, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        zts_bsd_setsockopt(client, ZTS_SOL_SOCKET, ZTS_SO_KEEPALIVE,
                           &keepalive, sizeof(keepalive));

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

    /* Clear the connect timeouts — they must not apply to the relay, or an
       idle keep-alive connection would be torn down after 10 s of silence. */
    struct timeval no_tv = { .tv_sec = 0, .tv_usec = 0 };
    setsockopt(local, SOL_SOCKET, SO_RCVTIMEO, &no_tv, sizeof(no_tv));
    setsockopt(local, SOL_SOCKET, SO_SNDTIMEO, &no_tv, sizeof(no_tv));

    /* Enable TCP keepalive so a peer that vanishes without a clean FIN is
       eventually reaped instead of leaking a relay thread + fds forever. */
    int keepalive = 1;
    setsockopt(local, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    zts_bsd_setsockopt(zt_fd, ZTS_SOL_SOCKET, ZTS_SO_KEEPALIVE,
                       &keepalive, sizeof(keepalive));

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
    int my_epoch = atomic_load(&g_server_epoch);

    int srv = zts_bsd_socket(ZTS_AF_INET, ZTS_SOCK_STREAM, 0);
    if (srv < 0) {
        syslog(LOG_ERR, "socks5: socket failed");
        return NULL;
    }

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
    zts_set_blocking(srv, 0);

    syslog(LOG_INFO, "SOCKS5 proxy ready on %s:%d", zt_addr, SOCKS5_PORT);

    while (!shutdown_requested && atomic_load(&g_server_epoch) == my_epoch) {
        int client = zts_bsd_accept(srv, NULL, NULL);
        if (client < 0) {
            zts_util_delay(ACCEPT_POLL_MS);
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

/* ── outbound proxies (loopback → ZeroTier) ──────────────────────── */

/* Resolve host via system DNS and open a ZeroTier TCP connection.
   Returns a zts fd on success, -1 on failure. */
static int zt_connect_to(const char *host, int port) {
    struct addrinfo hints, *res = NULL;
    char portstr[8];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return -1;

    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    struct zts_sockaddr_in zaddr;
    memset(&zaddr, 0, sizeof(zaddr));
    zaddr.sin_family = ZTS_AF_INET;
    zaddr.sin_port   = sa->sin_port;           /* already network byte order */
    memcpy(&zaddr.sin_addr, &sa->sin_addr, 4); /* 4-byte IPv4 */
    freeaddrinfo(res);

    int zt_fd = zts_bsd_socket(ZTS_AF_INET, ZTS_SOCK_STREAM, 0);
    if (zt_fd < 0)
        return -1;

    if (zts_bsd_connect(zt_fd, (struct zts_sockaddr *)&zaddr,
                         sizeof(zaddr)) != 0) {
        zts_bsd_close(zt_fd);
        return -1;
    }
    return zt_fd;
}

/* Create a POSIX TCP server socket bound to 127.0.0.1:port. */
static int make_local_server(int port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0)
        return -1;

    int reuse = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(srv, 32) < 0) {
        close(srv);
        return -1;
    }
    return srv;
}

/* accept() that gives up after ACCEPT_POLL_MS so the caller can re-check the
   epoch instead of blocking forever on a socket it alone owns. */
static int accept_with_timeout(int srv) {
    struct pollfd pfd = { .fd = srv, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, ACCEPT_POLL_MS) <= 0)
        return -1;
    return accept(srv, NULL, NULL);
}

/* Read HTTP request headers from a POSIX fd (one byte at a time) until the
   blank line terminator \r\n\r\n is found or the buffer is full.
   Returns byte count, or -1 on error. */
static int read_http_headers(int fd, char *buf, int bufsize) {
    int total = 0;
    while (total < bufsize - 1) {
        ssize_t n = read(fd, buf + total, 1);
        if (n <= 0) { buf[total] = '\0'; return -1; }
        total++;
        if (total >= 4 && memcmp(buf + total - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    buf[total] = '\0';
    return total;
}

/**
 * Handle one HTTP CONNECT request from a local camera app.
 * CONNECT <host>:<port> HTTP/x.x  →  ZeroTier connection  →  relay.
 */
static void *handle_http_connect(void *arg) {
    int local_fd = (int)(intptr_t)arg;
    char buf[4096];

    if (read_http_headers(local_fd, buf, (int)sizeof(buf)) < 0)
        goto fail_local;

    /* First line: "CONNECT host:port HTTP/x.x" */
    char method[16], hostport[512];
    if (sscanf(buf, "%15s %511s", method, hostport) != 2 ||
        strcasecmp(method, "CONNECT") != 0) {
        write(local_fd, "HTTP/1.1 405 Method Not Allowed\r\n\r\n", 36);
        goto fail_local;
    }

    /* Split "host:port" on the last colon */
    char host[256] = {0};
    int  port = 443;
    char *colon = strrchr(hostport, ':');
    if (colon && colon != hostport) {
        int hlen = (int)(colon - hostport);
        if (hlen >= (int)sizeof(host))
            hlen = (int)sizeof(host) - 1;
        memcpy(host, hostport, (size_t)hlen);
        host[hlen] = '\0';
        port = atoi(colon + 1);
    } else {
        snprintf(host, sizeof(host), "%s", hostport);
    }
    if (port <= 0 || port > 65535) {
        write(local_fd, "HTTP/1.1 400 Bad Request\r\n\r\n", 28);
        goto fail_local;
    }

    {
        int zt_fd = zt_connect_to(host, port);
        if (zt_fd < 0) {
            write(local_fd, "HTTP/1.1 502 Bad Gateway\r\n\r\n", 28);
            goto fail_local;
        }
        write(local_fd, "HTTP/1.1 200 Connection established\r\n\r\n", 39);
        relay(zt_fd, local_fd);
    }
    return NULL;

fail_local:
    close(local_fd);
    return NULL;
}

/* HTTP CONNECT proxy accept loop — binds on 127.0.0.1 on the configured port. */
static void *http_connect_server(void *arg) {
    int port = (int)(intptr_t)arg;
    int my_epoch = atomic_load(&g_server_epoch);

    int srv = make_local_server(port);
    if (srv < 0) {
        syslog(LOG_ERR, "http-proxy: failed to bind 127.0.0.1:%d — "
               "port may be in use; change HTTPProxyPort in Settings", port);
        return NULL;
    }
    atomic_store(&g_http_port_actual, port);
    syslog(LOG_INFO, "HTTP CONNECT proxy ready on 127.0.0.1:%d", port);

    while (!shutdown_requested && atomic_load(&g_server_epoch) == my_epoch) {
        int client = accept_with_timeout(srv);
        if (client < 0)
            continue;
        pthread_t thr;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thr, &attr, handle_http_connect,
                           (void *)(intptr_t)client) != 0)
            close(client);
        pthread_attr_destroy(&attr);
    }
    close(srv);
    return NULL;
}

/**
 * Handle one outbound SOCKS5 CONNECT from a local camera app (RFC 1928).
 * Routes the connection through ZeroTier.
 */
static void *handle_local_socks5(void *arg) {
    int local_fd = (int)(intptr_t)arg;
    unsigned char buf[260];

    /* Greeting */
    if (read(local_fd, buf, 2) != 2 || buf[0] != 0x05) goto fail;
    {
        int nm = buf[1];
        if (nm > 0 && read(local_fd, buf, (size_t)nm) != nm) goto fail;
    }
    { unsigned char rep[] = { 0x05, 0x00 }; write(local_fd, rep, 2); }

    /* Request */
    if (read(local_fd, buf, 4) != 4) goto fail;
    if (buf[0] != 0x05 || buf[1] != 0x01) {
        unsigned char err[] = { 0x05, 0x07, 0x00, 0x01, 0,0,0,0, 0,0 };
        write(local_fd, err, sizeof(err));
        goto fail;
    }

    {
        char host[256] = {0};
        int  port = 0;

        switch (buf[3]) {
        case 0x01: { /* IPv4 */
            unsigned char raw[6];
            if (read(local_fd, raw, 6) != 6) goto fail;
            snprintf(host, sizeof(host), "%u.%u.%u.%u",
                     raw[0], raw[1], raw[2], raw[3]);
            port = ((int)raw[4] << 8) | raw[5];
            break;
        }
        case 0x03: { /* Domain name */
            unsigned char lenb;
            if (read(local_fd, &lenb, 1) != 1) goto fail;
            int nlen = lenb;
            if (nlen >= (int)sizeof(host)) nlen = (int)sizeof(host) - 1;
            unsigned char tmp[257];
            if (read(local_fd, tmp, (size_t)(nlen + 2)) != nlen + 2) goto fail;
            memcpy(host, tmp, (size_t)nlen);
            host[nlen] = '\0';
            port = ((int)tmp[nlen] << 8) | tmp[nlen + 1];
            break;
        }
        default: {
            unsigned char err[] = { 0x05, 0x08, 0x00, 0x01, 0,0,0,0, 0,0 };
            write(local_fd, err, sizeof(err));
            goto fail;
        }
        }

        if (port <= 0 || port > 65535) goto fail;

        int zt_fd = zt_connect_to(host, port);
        if (zt_fd < 0) {
            unsigned char err[] = { 0x05, 0x04, 0x00, 0x01, 0,0,0,0, 0,0 };
            write(local_fd, err, sizeof(err));
            goto fail;
        }
        {
            unsigned char ok[] = {
                0x05, 0x00, 0x00, 0x01,
                127, 0, 0, 1,
                (unsigned char)(port >> 8), (unsigned char)(port & 0xFF)
            };
            write(local_fd, ok, sizeof(ok));
        }
        relay(zt_fd, local_fd);
    }
    return NULL;

fail:
    close(local_fd);
    return NULL;
}

/* Outbound SOCKS5 accept loop — binds on 127.0.0.1 on the configured port. */
static void *local_socks5_server(void *arg) {
    int port = (int)(intptr_t)arg;
    int my_epoch = atomic_load(&g_server_epoch);

    int srv = make_local_server(port);
    if (srv < 0) {
        syslog(LOG_ERR, "local-socks5: failed to bind 127.0.0.1:%d — "
               "port may be in use; change SOCKS5ProxyPort in Settings", port);
        return NULL;
    }
    atomic_store(&g_local_socks5_port_actual, port);
    syslog(LOG_INFO, "Outbound SOCKS5 proxy ready on 127.0.0.1:%d", port);

    while (!shutdown_requested && atomic_load(&g_server_epoch) == my_epoch) {
        int client = accept_with_timeout(srv);
        if (client < 0)
            continue;
        pthread_t thr;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&thr, &attr, handle_local_socks5,
                           (void *)(intptr_t)client) != 0)
            close(client);
        pthread_attr_destroy(&attr);
    }
    close(srv);
    return NULL;
}

/* ── signal handlers ─────────────────────────────────────────────── */

static void sig_handler(int sig) {
    if (sig == SIGUSR1)
        reload_requested = 1;
    else if (sig == SIGUSR2)
        forward_reload_requested = 1;
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
    sigaction(SIGUSR2, &sa, NULL);

    /* Ensure state directories exist */
    mkdir(STATE_DIR, 0755);
    {
        char nets_dir[512];
        snprintf(nets_dir, sizeof(nets_dir), "%s/networks.d", STATE_DIR);
        mkdir(nets_dir, 0755);
    }

    /* Log whether a custom planet (roots) file is active */
    {
        char roots_path[512];
        struct stat rst;
        snprintf(roots_path, sizeof(roots_path), "%s/roots", STATE_DIR);
        if (stat(roots_path, &rst) == 0)
            syslog(LOG_INFO, "Custom planet file active (%ld bytes) — connecting to self-hosted controller",
                   (long)rst.st_size);
        else
            syslog(LOG_INFO, "No custom planet file — using built-in ZeroTier planet (zerotier.com)");
    }

    /* Initialize ZeroTier from persistent state */
    int rc = zts_init_from_storage(STATE_DIR);
    if (rc != ZTS_ERR_OK) {
        syslog(LOG_ERR, "zts_init_from_storage failed: %d", rc);
        return 1;
    }

    /* Keep the same UDP port across restarts. With a random port, every peer
       keeps sending to the previous one and needs minutes to re-path. */
    zts_init_set_port(ZT_UDP_PORT);

    /* Start the ZeroTier node */
    rc = zts_node_start();
    if (rc != ZTS_ERR_OK) {
        syslog(LOG_WARNING, "zts_node_start on port %d failed: %d — "
               "retrying with a random port", ZT_UDP_PORT, rc);
        zts_init_set_port(0);
        rc = zts_node_start();
    }
    if (rc != ZTS_ERR_OK) {
        syslog(LOG_ERR, "zts_node_start failed: %d", rc);
        return 1;
    }

    write_status("starting", NULL, NULL, NULL, 0, 0);
    syslog(LOG_INFO, "Waiting for ZeroTier node to come online...");
    while (!zts_node_is_online()) {
        if (shutdown_requested) goto cleanup;
        zts_util_delay(200);
    }

    {
        char node_hex[20];
        snprintf(node_hex, sizeof(node_hex), "%llx",
                 (unsigned long long)zts_node_get_id());
        syslog(LOG_INFO, "Node online, ID: %s", node_hex);
        write_status("waiting_config", node_hex, NULL, NULL, 0, 0);
    }

    /* Main loop: load config, join network, run proxy */
    while (!shutdown_requested) {
        char node_hex[20];
        snprintf(node_hex, sizeof(node_hex), "%llx",
                 (unsigned long long)zts_node_get_id());

        config_t cfg;
        if (!load_config(config_path, &cfg) || cfg.network_id[0] == '\0') {
            syslog(LOG_INFO, "Config incomplete — waiting for network ID");
            write_status("waiting_config", node_hex, NULL, NULL, 0, 0);
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
        {
            char roots_path[512];
            struct stat rst;
            snprintf(roots_path, sizeof(roots_path), "%s/roots", STATE_DIR);
            bool custom_planet = (stat(roots_path, &rst) == 0);
            syslog(LOG_INFO, "Waiting for address assignment on network %s "
                   "(authorize node %llx in %s)",
                   cfg.network_id, (unsigned long long)zts_node_get_id(),
                   custom_planet ? "your self-hosted ZeroTier controller" : "ZeroTier Central");
        }
        write_status("waiting_auth", node_hex, NULL, cfg.network_id, 0, 0);

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

        /* Server threads are detached: a reload retires them and starts a new
           generation, so nothing ever joins them. */
        pthread_attr_t srv_attr;
        pthread_attr_init(&srv_attr);
        pthread_attr_setdetachstate(&srv_attr, PTHREAD_CREATE_DETACHED);

        /* Start port forwarders */
        parse_forward_ports(cfg.forward_ports);
        pthread_t fwd_thread;
        for (size_t i = 0; i < g_forward_port_count; i++) {
            forwarder_ctx_t *fctx = malloc(sizeof(*fctx));
            if (!fctx) continue;
            fctx->port = g_forward_ports[i];
            snprintf(fctx->zt_addr, sizeof(fctx->zt_addr), "%s", zt_addr_str);
            if (pthread_create(&fwd_thread, &srv_attr, port_forwarder, fctx) != 0)
                free(fctx);
        }

        /* Start SOCKS5 proxy */
        /* zt_addr_str is on the stack but the SOCKS5 server copies what it
           needs before we could possibly overwrite it.  We use a static buffer
           so the thread has a stable pointer. */
        static char socks5_addr[ZTS_IP_MAX_STR_LEN];
        snprintf(socks5_addr, sizeof(socks5_addr), "%s", zt_addr_str);
        pthread_t socks5_thread;
        pthread_create(&socks5_thread, &srv_attr, socks5_server, socks5_addr);

        /* Start outbound proxies on loopback (camera apps → ZeroTier) */
        atomic_store(&g_http_port_actual, 0);
        atomic_store(&g_local_socks5_port_actual, 0);
        pthread_t http_proxy_thread;
        pthread_create(&http_proxy_thread, &srv_attr, http_connect_server,
                       (void *)(intptr_t)cfg.http_proxy_port);
        pthread_t local_socks5_thread;
        pthread_create(&local_socks5_thread, &srv_attr, local_socks5_server,
                       (void *)(intptr_t)cfg.socks5_proxy_port);
        pthread_attr_destroy(&srv_attr);

        /* Give the loopback proxy threads a moment to bind, then report
           the actual ports they secured (may differ from 8080/1080 if those
           are taken by another VPN ACAP). */
        zts_util_delay(500);
        syslog(LOG_INFO, "ZeroTier VPN is running — "
               "IP: %s | Forward ports configured | SOCKS5: %s:%d | "
               "HTTP proxy: 127.0.0.1:%d | Outbound SOCKS5: 127.0.0.1:%d",
               zt_addr_str, zt_addr_str, SOCKS5_PORT,
               atomic_load(&g_http_port_actual),
               atomic_load(&g_local_socks5_port_actual));
        write_status("connected", node_hex, zt_addr_str, cfg.network_id,
                     atomic_load(&g_http_port_actual),
                     atomic_load(&g_local_socks5_port_actual));

        /* Wait for reload or shutdown */
        int heartbeat_ticks = 0;
        while (!shutdown_requested && !reload_requested && !forward_reload_requested) {
            /* Detect address loss — important for custom planet servers where
               root keepalives can drop and ZeroTier silently loses the network
               membership without killing the process.  Re-enter the join loop
               so the proxy reconnects automatically. */
            if (!zts_addr_is_assigned(nwid, ZTS_AF_INET)) {
                syslog(LOG_WARNING,
                       "ZeroTier address lost on network %s — rejoining",
                       cfg.network_id);
                write_status("waiting_auth", node_hex, NULL, cfg.network_id, 0, 0);
                break;
            }
            /* Emit a heartbeat every 5 minutes so the UI log never goes stale
               after syslog rotates the initial startup messages out. */
            if (++heartbeat_ticks >= 60) {
                heartbeat_ticks = 0;
                syslog(LOG_INFO, "ZeroTier VPN is running — "
                       "IP: %s | Forward ports configured | SOCKS5: %s:%d | "
                       "HTTP proxy: 127.0.0.1:%d | Outbound SOCKS5: 127.0.0.1:%d",
                       zt_addr_str, zt_addr_str, SOCKS5_PORT,
                       atomic_load(&g_http_port_actual),
                       atomic_load(&g_local_socks5_port_actual));
                /* Also refresh the status file timestamp so the UI can detect
                   stale/dead status files (ts more than ~10 min old = suspect). */
                write_status("connected", node_hex, zt_addr_str, cfg.network_id,
                             atomic_load(&g_http_port_actual),
                             atomic_load(&g_local_socks5_port_actual));
            }
            zts_util_delay(5000);
        }

        bool full_reload = (reload_requested != 0);
        bool preserve_network = (forward_reload_requested != 0);
        if (reload_requested)
            reload_requested = 0;
        if (forward_reload_requested)
            forward_reload_requested = 0;

        /* Retire the current accept loops; each closes its own socket. */
        retire_server_threads();
        zts_util_delay(ACCEPT_POLL_MS * 3);

        if (preserve_network && !full_reload) {
            syslog(LOG_INFO, "Reloading forwarded ports without leaving network");
            continue;
        }

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
    write_status("disconnected", NULL, NULL, NULL, 0, 0);
    syslog(LOG_INFO, "Shutting down ZeroTier node");
    if (current_nwid != 0)
        zts_net_leave(current_nwid);
    zts_node_stop();
    syslog(LOG_INFO, "zerotier-userspace stopped");
    closelog();
    return 0;
}
