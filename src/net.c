/* net.c - minimal HTTP/HTTPS client for xpkg repo fetching (v2).
 *
 * Fetches a URL (http:// or https://) and writes the response body to a
 * file. HTTPS is done in-process with OpenSSL (libssl + libcrypto), so
 * xpkg can talk to real package repos (e.g. Hugging Face `resolve/main`
 * URLs) with no external curl/wget. HTTP is plain sockets.
 *
 * Scope kept deliberately small:
 *   - GET only, HTTP/1.1, Connection: close (one response per request)
 *   - 200 only (with a redirect follow for 301/302, Location)
 *   - body streamed to disk in chunks (packages can be large)
 *   - no chunked-encoding support in v2 (Content-Length based); reject
 *     chunked responses rather than corrupt the download.
 *
 * Everything here is first-party FreeLinX code (no GNU components).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "xpkg.h"

#define NET_TIMEOUT_SEC 30

struct parsed_url {
    int  use_tls;
    char host[XPKG_MAX_PATH];
    char port[8];          /* "80" or "443" unless overridden */
    char path[XPKG_MAX_PATH];
    long max_redirects;
};

/* Split scheme://host[:port]/path. Returns XPKG_OK or XPKG_ERR_USAGE. */
static xpkg_status_t parse_url(const char *url, struct parsed_url *p) {
    memset(p, 0, sizeof(*p));
    strncpy(p->port, "80", sizeof(p->port) - 1);

    const char *rest = url;
    if (strncasecmp(rest, "http://", 7) == 0) {
        p->use_tls = 0;
        rest += 7;
    } else if (strncasecmp(rest, "https://", 8) == 0) {
        p->use_tls = 1;
        strncpy(p->port, "443", sizeof(p->port) - 1);
        rest += 8;
    } else {
        return XPKG_ERR_USAGE;
    }

    /* host[:port] up to next '/' or end */
    char hostport[XPKG_MAX_PATH];
    size_t n = 0;
    while (rest[n] && rest[n] != '/' && n < sizeof(hostport) - 1) {
        hostport[n] = rest[n];
        n++;
    }
    hostport[n] = '\0';
    const char *path = rest[n] ? rest + n : "/";

    /* split host:port */
    const char *colon = strrchr(hostport, ':');
    if (colon && colon != hostport) {
        size_t hostlen = (size_t)(colon - hostport);
        memcpy(p->host, hostport, hostlen);
        p->host[hostlen] = '\0';
        strncpy(p->port, colon + 1, sizeof(p->port) - 1);
    } else {
        strncpy(p->host, hostport, sizeof(p->host) - 1);
    }

    strncpy(p->path, path, sizeof(p->path) - 1);
    p->max_redirects = 5;
    return XPKG_OK;
}

/* Establish a connected (and TLS-wrapped, if https) fd for host:port. */
static int net_connect(const struct parsed_url *p, SSL_CTX **ctx_out, SSL **ssl_out) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int err = getaddrinfo(p->host, p->port, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "xpkg: cannot resolve %s: %s\n", p->host, gai_strerror(err));
        return -1;
    }

    int fd = -1;
    /* Two passes: IPv4 first, then IPv6. Many hosts (and constrained
     * networks like WSL without IPv6) expose AAAA records first; trying
     * an unreachable IPv6 immediately fails, so prefer v4. */
    for (int pass = 0; pass < 2 && fd < 0; pass++) {
        for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
            if (pass == 0 && ai->ai_family != AF_INET) continue;   /* v4 first */
            if (pass == 1 && ai->ai_family == AF_INET) continue;   /* v6 second */
            int cfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (cfd < 0) continue;
            if (connect(cfd, ai->ai_addr, ai->ai_addrlen) == 0) {
                fd = cfd;
                break;
            }
            close(cfd);
        }
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fprintf(stderr, "xpkg: cannot connect to %s:%s\n", p->host, p->port);
        return -1;
    }

    if (!p->use_tls) {
        *ctx_out = NULL;
        *ssl_out = NULL;
        return fd;
    }

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        close(fd);
        return -1;
    }
    SSL *ssl = SSL_new(ctx);
    if (!ssl) {
        SSL_CTX_free(ctx);
        close(fd);
        return -1;
    }
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, p->host);

    if (SSL_connect(ssl) != 1) {
        fprintf(stderr, "xpkg: TLS handshake with %s failed\n", p->host);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(fd);
        return -1;
    }
    *ctx_out = ctx;
    *ssl_out = ssl;
    return fd;
}

/* Write up to len bytes; returns bytes written or -1. Handles TLS or plain. */
static long net_write_all(int fd, SSL *ssl, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n;
        if (ssl) {
            n = SSL_write(ssl, buf + off, (int)(len - off));
        } else {
            n = (int)write(fd, buf + off, len - off);
        }
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return (long)off;
}

/* Read one byte; -1 on error, 0 on EOF. */
static int net_read_byte(int fd, SSL *ssl) {
    unsigned char c;
    int n;
    if (ssl) {
        n = SSL_read(ssl, &c, 1);
    } else {
        n = (int)read(fd, &c, 1);
    }
    if (n == 1) return c;
    if (n == 0) return 0;
    return -1;
}

/* Read one CRLF-terminated line (no CR, no LF). Returns 0 on success. */
static int net_read_line(int fd, SSL *ssl, char *buf, size_t bufsz) {
    size_t i = 0;
    while (i + 1 < bufsz) {
        int c = net_read_byte(fd, ssl);
        if (c < 0) return -1;
        if (c == 0) return -1; /* EOF mid-line */
        if (c == '\n') break;
        if (c != '\r') buf[i++] = (char)c;
    }
    buf[i] = '\0';
    return 0;
}

/* Read raw bytes into buf (up to bufsz), returns count or -1. */
static long net_read(int fd, SSL *ssl, unsigned char *buf, size_t bufsz) {
    if (ssl) {
        int n = SSL_read(ssl, buf, (int)bufsz);
        return n <= 0 ? (n == 0 ? 0 : -1) : (long)n;
    }
    ssize_t n = read(fd, buf, bufsz);
    return n <= 0 ? (n == 0 ? 0 : -1) : (long)n;
}

/* The full fetch for a single URL. Returns XPKG_OK (body saved to
 * dest_path), or an error status. `base_url` is used to resolve relative
 * redirect Locations; for a top-level call pass the same as `url`. */
static xpkg_status_t fetch_url_once(const char *url, const char *dest_path) {
    struct parsed_url p;
    if (parse_url(url, &p) != XPKG_OK) {
        fprintf(stderr, "xpkg: bad repo URL: %s\n", url);
        return XPKG_ERR_USAGE;
    }

    SSL_CTX *ctx = NULL;
    SSL *ssl = NULL;
    int fd = net_connect(&p, &ctx, &ssl);
    if (fd < 0) {
        return XPKG_ERR_IO;
    }

    /* Build request: GET <path> HTTP/1.1 */
    char req[XPKG_MAX_PATH * 2];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%s\r\n"
             "User-Agent: xpkg/%s\r\n"
             "Accept: */*\r\n"
             "Connection: close\r\n\r\n",
             p.path, p.host, p.port, XPKG_VERSION);
    if (net_write_all(fd, ssl, req, strlen(req)) != (long)strlen(req)) {
        fprintf(stderr, "xpkg: write to %s failed\n", p.host);
        if (ssl) { SSL_free(ssl); SSL_CTX_free(ctx); }
        close(fd);
        return XPKG_ERR_IO;
    }

    /* Read status line */
    char status[XPKG_MAX_LINE];
    if (net_read_line(fd, ssl, status, sizeof(status)) != 0) {
        if (ssl) { SSL_free(ssl); SSL_CTX_free(ctx); }
        close(fd);
        return XPKG_ERR_IO;
    }
    if (strncmp(status, "HTTP/", 5) != 0) {
        if (ssl) { SSL_free(ssl); SSL_CTX_free(ctx); }
        close(fd);
        return XPKG_ERR_IO;
    }
    int code = 0;
    {
        const char *sp = strchr(status, ' ');
        if (sp) sscanf(sp + 1, "%d", &code);
    }

    /* Headers */
    int transfer_chunked = 0;
    long content_length = -1;
    char location[XPKG_MAX_PATH] = {0};
    char line[XPKG_MAX_LINE];
    while (net_read_line(fd, ssl, line, sizeof(line)) == 0 && line[0] != '\0') {
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            content_length = atol(line + 15);
        } else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            if (strstr(line + 18, "chunked")) transfer_chunked = 1;
        } else if (strncasecmp(line, "Location:", 9) == 0) {
            const char *v = line + 9;
            while (*v == ' ') v++;
            strncpy(location, v, sizeof(location) - 1);
        }
    }

    /* Redirects (absolute or relative Location). */
    if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) && location[0]) {
        if (ssl) { SSL_free(ssl); SSL_CTX_free(ctx); }
        close(fd);
        if (p.max_redirects-- <= 0) {
            return XPKG_ERR_IO;
        }
        char next[XPKG_MAX_PATH];
        if (strncasecmp(location, "http://", 7) == 0 ||
            strncasecmp(location, "https://", 8) == 0) {
            strncpy(next, location, sizeof(next) - 1);
        } else {
            /* relative: scheme://host + path (keep query) */
            char base[XPKG_MAX_PATH];
            snprintf(base, sizeof(base), "%s://%s",
                     p.use_tls ? "https" : "http", p.host);
            if (strlen(base) + 1 + strlen(location) >= sizeof(next)) {
                return XPKG_ERR_IO;
            }
            if (location[0] == '/') {
                /* absolute path: scheme://host + location */
                snprintf(next, sizeof(next), "%s://%s:%s%s",
                         p.use_tls ? "https" : "http", p.host, p.port, location);
            } else {
                /* relative path: keep last slash of current path */
                const char *lastslash = NULL;
                for (const char *q = p.path; *q; q++) if (*q == '/') lastslash = q;
                if (lastslash) {
                    size_t dirn = (size_t)(lastslash - p.path) + 1;
                    char dir[XPKG_MAX_PATH];
                    memcpy(dir, p.path, dirn);
                    dir[dirn] = '\0';
                    snprintf(next, sizeof(next), "%s://%s:%s%s%s",
                             p.use_tls ? "https" : "http", p.host, p.port, dir, location);
                } else {
                    snprintf(next, sizeof(next), "%s://%s:%s/%s",
                             p.use_tls ? "https" : "http", p.host, p.port, location);
                }
            }
            (void)base;
        }
        return fetch_url_once(next, dest_path);
    }

    if (code != 200) {
        fprintf(stderr, "xpkg: HTTP %d fetching %s\n", code, url);
        if (ssl) { SSL_free(ssl); SSL_CTX_free(ctx); }
        close(fd);
        return XPKG_ERR_NOT_FOUND;
    }

    /* Stream body to file. Handle both Content-Length and chunked
     * transfer-encoding (example.com and many CDNs use chunked). */
    FILE *out = fopen(dest_path, "wb");
    if (!out) {
        if (ssl) { SSL_free(ssl); SSL_CTX_free(ctx); }
        close(fd);
        return XPKG_ERR_IO;
    }

    if (transfer_chunked) {
        /* Read chunks: <hex-size>CRLF <data> CRLF, until a 0-size chunk. */
        while (1) {
            char sizeline[XPKG_MAX_LINE];
            if (net_read_line(fd, ssl, sizeline, sizeof(sizeline)) != 0) break;
            long csize = strtol(sizeline, NULL, 16);
            if (csize <= 0) break; /* end of chunks */
            long remaining = csize;
            unsigned char buf[65536];
            while (remaining > 0) {
                size_t want = remaining < (long)sizeof(buf) ? (size_t)remaining : sizeof(buf);
                long n = net_read(fd, ssl, buf, want);
                if (n <= 0) { remaining = -1; break; }
                fwrite(buf, 1, (size_t)n, out);
                remaining -= n;
            }
            if (remaining < 0) break;
            /* consume CRLF after chunk data */
            if (net_read_byte(fd, ssl) != '\r') { /* tolerate */ }
            if (net_read_byte(fd, ssl) != '\n') { /* tolerate */ }
        }
    } else {
        unsigned char buf[65536];
        long total = 0;
        while (1) {
            long n = net_read(fd, ssl, buf, sizeof(buf));
            if (n < 0) break;
            if (n == 0) break;
            fwrite(buf, 1, (size_t)n, out);
            total += n;
            if (content_length > 0 && total >= content_length) break;
        }
    }
    fclose(out);

    if (ssl) { SSL_free(ssl); SSL_CTX_free(ctx); }
    close(fd);
    return XPKG_OK;
}

/* Public: fetch URL to dest_path. */
xpkg_status_t xpkg_net_get(const char *url, const char *dest_path) {
    static int openssl_init_done = 0;
    if (!openssl_init_done) {
        SSL_library_init();
        SSL_load_error_strings();
        openssl_init_done = 1;
    }
    return fetch_url_once(url, dest_path);
}
