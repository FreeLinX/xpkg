/* net.c - HTTP/HTTPS GET for repository fetches.
 *
 * TLS is OpenSSL with certificate *and* host name verification against the
 * system CA bundle (XPKG_CA_FILE overrides it); a download from a host that
 * cannot prove its identity is refused.  Bodies are streamed to <dest>.part
 * and renamed into place only when complete (Content-Length checked, or the
 * final zero-size chunk seen), so an interrupted fetch never leaves a
 * truncated file where a good one is expected.  Redirects (absolute or
 * relative, as Hugging Face uses) are followed up to 8 hops.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include "xpkg.h"

#define NET_TIMEOUT_SEC 30
#define NET_MAX_REDIRECTS 8
#define NET_HDR_LINE 16384

int xpkg_net_quiet_404;

static const char *ca_candidates[] = {
    "/etc/ssl/certs/ca-certificates.crt",
    "/etc/ssl/cert.pem",
    "/etc/pki/tls/certs/ca-bundle.crt",
    NULL
};

typedef struct {
    int tls;
    char host[256];
    char port[8];
    char path[XPKG_MAX_URL];
} url_t;

typedef struct {
    int fd;
    SSL_CTX *ctx;
    SSL *ssl;
    unsigned char buf[65536];
    size_t pos, len;
} conn_t;

static int parse_url(const char *url, url_t *u) {
    memset(u, 0, sizeof(*u));
    const char *rest;
    if (!strncasecmp(url, "https://", 8)) { u->tls = 1; rest = url + 8; strcpy(u->port, "443"); }
    else if (!strncasecmp(url, "http://", 7)) { rest = url + 7; strcpy(u->port, "80"); }
    else return -1;
    size_t n = strcspn(rest, "/?#");
    char hostport[300];
    if (n == 0 || n >= sizeof(hostport)) return -1;
    memcpy(hostport, rest, n);
    hostport[n] = '\0';
    char *colon = strrchr(hostport, ':');
    if (colon && !strchr(hostport, ']')) {
        *colon = '\0';
        snprintf(u->port, sizeof(u->port), "%s", colon + 1);
    }
    snprintf(u->host, sizeof(u->host), "%s", hostport);
    const char *path = rest + n;
    if (*path == '#' || !*path) path = "/";
    if (*path == '?') snprintf(u->path, sizeof(u->path), "/%s", path);
    else snprintf(u->path, sizeof(u->path), "%s", path);
    char *hash = strchr(u->path, '#');
    if (hash) *hash = '\0';
    return 0;
}

static void conn_close(conn_t *c) {
    if (c->ssl) { SSL_shutdown(c->ssl); SSL_free(c->ssl); }
    if (c->ctx) SSL_CTX_free(c->ctx);
    if (c->fd >= 0) close(c->fd);
    c->ssl = NULL; c->ctx = NULL; c->fd = -1;
}

static int load_cas(SSL_CTX *ctx) {
    const char *env = getenv("XPKG_CA_FILE");
    if (env && env[0]) return SSL_CTX_load_verify_locations(ctx, env, NULL) == 1;
    for (int i = 0; ca_candidates[i]; i++)
        if (access(ca_candidates[i], R_OK) == 0 &&
            SSL_CTX_load_verify_locations(ctx, ca_candidates[i], NULL) == 1)
            return 1;
    return SSL_CTX_set_default_verify_paths(ctx) == 1;
}

static int conn_open(conn_t *c, const url_t *u) {
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo(u->host, u->port, &hints, &res);
    if (err) {
        xpkg_err("cannot resolve %s: %s", u->host, gai_strerror(err));
        return -1;
    }
    struct timeval tv = { NET_TIMEOUT_SEC, 0 };
    /* IPv4 first: networks without an IPv6 route are common. */
    for (int pass = 0; pass < 2 && c->fd < 0; pass++) {
        for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
            if ((pass == 0) != (ai->ai_family == AF_INET)) continue;
            int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
            if (fd < 0) continue;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) { c->fd = fd; break; }
            close(fd);
        }
    }
    freeaddrinfo(res);
    if (c->fd < 0) {
        xpkg_err("cannot connect to %s:%s", u->host, u->port);
        return -1;
    }
    if (!u->tls) return 0;

    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx) { conn_close(c); return -1; }
    SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
    if (!load_cas(c->ctx)) {
        xpkg_err("no CA certificates found (install ca-certificates or set XPKG_CA_FILE)");
        conn_close(c);
        return -1;
    }
    SSL_CTX_set_verify(c->ctx, SSL_VERIFY_PEER, NULL);
    c->ssl = SSL_new(c->ctx);
    if (!c->ssl) { conn_close(c); return -1; }
    SSL_set_fd(c->ssl, c->fd);
    SSL_set_tlsext_host_name(c->ssl, u->host);
    SSL_set1_host(c->ssl, u->host);
    if (SSL_connect(c->ssl) != 1) {
        long v = SSL_get_verify_result(c->ssl);
        conn_close(c);
        if (v != X509_V_OK) {
            xpkg_err("TLS: certificate of %s not trusted: %s", u->host, X509_verify_cert_error_string(v));
            return -2;   /* a verdict, not a hiccup: never retried */
        }
        xpkg_err("TLS handshake with %s failed", u->host);
        return -1;
    }
    return 0;
}

static long conn_read_raw(conn_t *c, void *buf, size_t n) {
    for (;;) {
        if (c->ssl) {
            int r = SSL_read(c->ssl, buf, (int)n);
            if (r > 0) return r;
            int e = SSL_get_error(c->ssl, r);
            if (e == SSL_ERROR_ZERO_RETURN) return 0;
            if (e == SSL_ERROR_SYSCALL && r == 0) return 0; /* peer closed without close_notify */
            return -1;
        }
        ssize_t r = read(c->fd, buf, n);
        if (r < 0 && errno == EINTR) continue;
        return r;
    }
}

static int conn_fill(conn_t *c) {
    if (c->pos < c->len) return 1;
    long r = conn_read_raw(c, c->buf, sizeof(c->buf));
    if (r <= 0) return (int)r;
    c->pos = 0;
    c->len = (size_t)r;
    return 1;
}

static int conn_getc(conn_t *c) {
    if (conn_fill(c) <= 0) return -1;
    return c->buf[c->pos++];
}

static int conn_line(conn_t *c, char *out, size_t n) {
    size_t i = 0;
    for (;;) {
        int ch = conn_getc(c);
        if (ch < 0) return -1;
        if (ch == '\n') break;
        if (ch != '\r' && i + 1 < n) out[i++] = (char)ch;
    }
    out[i] = '\0';
    return 0;
}

static long conn_read(conn_t *c, void *out, size_t n) {
    if (c->pos < c->len) {
        size_t take = c->len - c->pos < n ? c->len - c->pos : n;
        memcpy(out, c->buf + c->pos, take);
        c->pos += take;
        return (long)take;
    }
    return conn_read_raw(c, out, n);
}

static int conn_write(conn_t *c, const char *s, size_t n) {
    while (n > 0) {
        long w = c->ssl ? SSL_write(c->ssl, s, (int)n) : write(c->fd, s, n);
        if (w <= 0) return -1;
        s += w; n -= (size_t)w;
    }
    return 0;
}

static int has_word_ci(const char *hay, const char *word) {
    size_t wl = strlen(word);
    for (; *hay; hay++)
        if (!strncasecmp(hay, word, wl)) return 1;
    return 0;
}

/* --- progress ------------------------------------------------------------ */

typedef struct {
    const char *label;
    unsigned long long total, done;
    double start, last;
} progress_t;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void progress_draw(progress_t *p, int final) {
    if (!p->label || xpkg_opts.quiet || !isatty(STDOUT_FILENO)) return;
    double t = now_s();
    if (!final && t - p->last < 0.1) return;
    p->last = t;
    char done[32], total[32], rate[32];
    double el = t - p->start;
    xpkg_human_size(p->done, done, sizeof(done));
    xpkg_human_size(p->total, total, sizeof(total));
    xpkg_human_size(el > 0.05 ? (unsigned long long)(p->done / el) : 0, rate, sizeof(rate));
    int pct = p->total ? (int)(p->done * 100 / p->total) : 0;
    int w = 24, fill = p->total ? (int)(p->done * w / p->total) : 0;
    char bar[32];
    for (int i = 0; i < w; i++) bar[i] = i < fill ? '#' : '-';
    bar[w] = '\0';
    if (p->total)
        printf("\r  %-28.28s [%s] %3d%% %10s %10s/s", p->label, bar, pct, total, rate);
    else
        printf("\r  %-28.28s %10s %10s/s", p->label, done, rate);
    if (final) putchar('\n');
    fflush(stdout);
}

/* --- request ------------------------------------------------------------- */

static void resolve_location(const url_t *u, const char *loc, char *next, size_t n) {
    if (!strncasecmp(loc, "http://", 7) || !strncasecmp(loc, "https://", 8)) {
        snprintf(next, n, "%s", loc);
        return;
    }
    const char *scheme = u->tls ? "https" : "http";
    int defport = (u->tls && !strcmp(u->port, "443")) || (!u->tls && !strcmp(u->port, "80"));
    char origin[320];
    if (defport) snprintf(origin, sizeof(origin), "%s://%s", scheme, u->host);
    else snprintf(origin, sizeof(origin), "%s://%s:%s", scheme, u->host, u->port);
    if (loc[0] == '/' && loc[1] == '/') { snprintf(next, n, "%s:%s", scheme, loc); return; }
    if (loc[0] == '/') { snprintf(next, n, "%s%s", origin, loc); return; }
    char dir[XPKG_MAX_URL];
    snprintf(dir, sizeof(dir), "%s", u->path);
    char *q = strchr(dir, '?');
    if (q) *q = '\0';
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = '\0';
    snprintf(next, n, "%s%s%s", origin, dir, loc);
}

static xpkg_status_t get_once(const char *url, FILE *out, const char *label, char *redirect, size_t rsz) {
    url_t u;
    if (parse_url(url, &u) != 0) {
        xpkg_err("bad URL: %s", url);
        return XPKG_ERR_USAGE;
    }
    conn_t c;
    int oc = conn_open(&c, &u);
    if (oc != 0) return oc == -2 ? XPKG_ERR_SIGNATURE : XPKG_ERR_IO;

    int defport = (u.tls && !strcmp(u.port, "443")) || (!u.tls && !strcmp(u.port, "80"));
    char *req = malloc(XPKG_MAX_URL + 512);
    if (!req) { conn_close(&c); return XPKG_ERR_IO; }
    snprintf(req, XPKG_MAX_URL + 512,
             "GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: xpkg/%s\r\n"
             "Accept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
             u.path, u.host, defport ? "" : ":", defport ? "" : u.port, XPKG_VERSION);
    int wr = conn_write(&c, req, strlen(req));
    free(req);
    if (wr != 0) { xpkg_err("cannot send request to %s", u.host); conn_close(&c); return XPKG_ERR_IO; }

    char *line = malloc(NET_HDR_LINE);
    if (!line) { conn_close(&c); return XPKG_ERR_IO; }
    xpkg_status_t st = XPKG_OK;
    int code = 0, chunked = 0;
    long long clen = -1;
    redirect[0] = '\0';
    if (conn_line(&c, line, NET_HDR_LINE) != 0 || strncmp(line, "HTTP/", 5) != 0) {
        xpkg_err("bad HTTP response from %s", u.host);
        st = XPKG_ERR_IO;
        goto done;
    }
    {
        const char *sp = strchr(line, ' ');
        if (sp) code = atoi(sp + 1);
    }
    for (;;) {
        if (conn_line(&c, line, NET_HDR_LINE) != 0) { st = XPKG_ERR_IO; goto done; }
        if (!line[0]) break;
        if (!strncasecmp(line, "Content-Length:", 15)) clen = atoll(line + 15);
        else if (!strncasecmp(line, "Transfer-Encoding:", 18) && has_word_ci(line + 18, "chunked")) chunked = 1;
        else if (!strncasecmp(line, "Location:", 9)) {
            const char *v = line + 9;
            while (*v == ' ') v++;
            resolve_location(&u, v, redirect, rsz);
        }
    }
    if (code >= 300 && code < 400 && redirect[0]) { st = XPKG_OK; goto done; }
    redirect[0] = '\0';
    if (code != 200) {
        if (!(code == 404 && xpkg_net_quiet_404)) xpkg_err("HTTP %d for %s", code, url);
        st = code == 404 ? XPKG_ERR_NOT_FOUND : XPKG_ERR_IO;
        goto done;
    }

    progress_t p = { label, clen > 0 ? (unsigned long long)clen : 0, 0, now_s(), 0 };
    unsigned char buf[65536];
    if (chunked) {
        for (;;) {
            if (conn_line(&c, line, NET_HDR_LINE) != 0) { st = XPKG_ERR_IO; break; }
            char *end;
            unsigned long long left = strtoull(line, &end, 16);
            if (end == line) { st = XPKG_ERR_IO; break; }
            if (left == 0) break;
            while (left > 0) {
                long r = conn_read(&c, buf, left < sizeof(buf) ? (size_t)left : sizeof(buf));
                if (r <= 0) { st = XPKG_ERR_IO; break; }
                if (fwrite(buf, 1, (size_t)r, out) != (size_t)r) { st = XPKG_ERR_IO; break; }
                left -= (unsigned long long)r;
                p.done += (unsigned long long)r;
                progress_draw(&p, 0);
            }
            if (st != XPKG_OK) break;
            if (conn_line(&c, line, NET_HDR_LINE) != 0) { st = XPKG_ERR_IO; break; }
        }
    } else {
        for (;;) {
            size_t want = sizeof(buf);
            if (clen >= 0) {
                if (p.done >= (unsigned long long)clen) break;
                if ((unsigned long long)clen - p.done < want) want = (size_t)(clen - (long long)p.done);
            }
            long r = conn_read(&c, buf, want);
            if (r < 0) { st = XPKG_ERR_IO; break; }
            if (r == 0) break;
            if (fwrite(buf, 1, (size_t)r, out) != (size_t)r) { st = XPKG_ERR_IO; break; }
            p.done += (unsigned long long)r;
            progress_draw(&p, 0);
        }
        if (st == XPKG_OK && clen >= 0 && p.done != (unsigned long long)clen) st = XPKG_ERR_IO;
    }
    if (st == XPKG_OK) progress_draw(&p, 1);
    else {
        if (p.label && isatty(STDOUT_FILENO) && !xpkg_opts.quiet) putchar('\n');
        xpkg_err("download of %s was cut off", url);
    }
done:
    free(line);
    conn_close(&c);
    return st;
}

static xpkg_status_t net_get_once(const char *url, const char *dest_path, const char *label);

/* Network hiccups (DNS "try again", a dropped connection) are retried with a
 * short back-off; a definite answer (404, bad certificate) is not. */
xpkg_status_t xpkg_net_get(const char *url, const char *dest_path, const char *label) {
    xpkg_status_t st = XPKG_ERR_IO;
    for (int attempt = 1; attempt <= 4; attempt++) {
        st = net_get_once(url, dest_path, label);
        if (st != XPKG_ERR_IO || attempt == 4) break;
        if (!xpkg_opts.quiet) fprintf(stderr, "xpkg: retrying in %d s (attempt %d of 4)\n", attempt * 2, attempt + 1);
        sleep((unsigned)(attempt * 2));
    }
    return st;
}

static xpkg_status_t net_get_once(const char *url, const char *dest_path, const char *label) {
    static int init = 0;
    if (!init) {
        OPENSSL_init_ssl(0, NULL);
        init = 1;
    }
    char part[XPKG_MAX_PATH];
    snprintf(part, sizeof(part), "%s.part", dest_path);
    char *cur = malloc(XPKG_MAX_URL), *next = malloc(XPKG_MAX_URL);
    if (!cur || !next) { free(cur); free(next); return XPKG_ERR_IO; }
    snprintf(cur, XPKG_MAX_URL, "%s", url);

    xpkg_status_t st = XPKG_ERR_IO;
    for (int hop = 0; hop <= NET_MAX_REDIRECTS; hop++) {
        FILE *out = fopen(part, "wb");
        if (!out) {
            xpkg_err("cannot write %s: %s", part, strerror(errno));
            break;
        }
        st = get_once(cur, out, label, next, XPKG_MAX_URL);
        if (fclose(out) != 0 && st == XPKG_OK) st = XPKG_ERR_IO;
        if (st != XPKG_OK) break;
        if (!next[0]) {
            if (rename(part, dest_path) != 0) st = XPKG_ERR_IO;
            break;
        }
        /* never follow a redirect from https down to plain http */
        if (!strncasecmp(cur, "https://", 8) && strncasecmp(next, "https://", 8)) {
            xpkg_err("refusing redirect from HTTPS to %s", next);
            st = XPKG_ERR_SIGNATURE;
            break;
        }
        snprintf(cur, XPKG_MAX_URL, "%s", next);
        st = XPKG_ERR_IO;
    }
    if (st != XPKG_OK) unlink(part);
    free(cur);
    free(next);
    return st;
}
