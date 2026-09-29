/* util.c - messages, filesystem helpers and small formatting utilities. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include "xpkg.h"

int xpkg_is_tty(void) {
    static int tty = -1;
    if (tty < 0) {
        const char *nc = getenv("NO_COLOR");
        tty = isatty(STDOUT_FILENO) && !(nc && nc[0]);
    }
    return tty;
}

const char *xpkg_color(const char *code) {
    return xpkg_is_tty() ? code : "";
}

void xpkg_msg(const char *fmt, ...) {
    if (xpkg_opts.quiet) return;
    va_list ap;
    printf("%s::%s ", xpkg_color("\033[1;34m"), xpkg_color("\033[0m"));
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

void xpkg_warn(const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "%swarning:%s ", isatty(2) ? "\033[1;33m" : "", isatty(2) ? "\033[0m" : "");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void xpkg_err(const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "%serror:%s ", isatty(2) ? "\033[1;31m" : "", isatty(2) ? "\033[0m" : "");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

int xpkg_mkdir_p(const char *path, unsigned mode) {
    char tmp[XPKG_MAX_PATH];
    if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp)) return -1;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return -1;
    return 0;
}

void xpkg_rm_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) {
        unlink(dir);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[XPKG_MAX_PATH];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode)) xpkg_rm_tree(p);
        else unlink(p);
    }
    closedir(d);
    rmdir(dir);
}

/* Removes now-empty parent directories of path, never going above stop. */
void xpkg_rmdir_parents(const char *path, const char *stop) {
    char tmp[XPKG_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t stoplen = strlen(stop);
    for (;;) {
        char *slash = strrchr(tmp, '/');
        if (!slash || slash == tmp) return;
        *slash = '\0';
        if (strlen(tmp) <= stoplen) return;
        if (rmdir(tmp) != 0) return;   /* not empty (or not ours): stop */
    }
}

char *xpkg_read_file(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return NULL; }
    size_t cap = st.st_size > 0 ? (size_t)st.st_size : 4096, got = 0;
    char *buf = malloc(cap + 1);
    if (!buf) { close(fd); return NULL; }
    for (;;) {
        if (got == cap) {
            char *nb = realloc(buf, cap * 2 + 1);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb; cap *= 2;
        }
        ssize_t r = read(fd, buf + got, cap - got);
        if (r < 0) { if (errno == EINTR) continue; free(buf); close(fd); return NULL; }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = '\0';
    if (len) *len = got;
    return buf;
}

int xpkg_write_file_atomic(const char *path, const void *data, size_t len) {
    char tmp[XPKG_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    const char *p = data;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) { if (errno == EINTR) continue; close(fd); unlink(tmp); return -1; }
        p += w; len -= (size_t)w;
    }
    if (fsync(fd) != 0 || close(fd) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

void xpkg_human_size(unsigned long long n, char *out, size_t outsz) {
    const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)n;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) snprintf(out, outsz, "%llu B", n);
    else snprintf(out, outsz, "%.1f %s", v, u[i]);
}

/* Package names: letters, digits and . _ + - ; must start alphanumeric. */
int xpkg_valid_name(const char *s) {
    if (!s || !*s || strlen(s) >= XPKG_MAX_NAME) return 0;
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9')))
        return 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '+' || c == '-')
            continue;
        return 0;
    }
    return 1;
}
