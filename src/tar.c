/* tar.c - ustar reader, reading straight through zlib's gzFile so gzip
 * decompression and tar parsing happen in one pass with no external tar.
 *
 * Supported entries: regular files ('0'/'\0'), directories ('5'), symlinks
 * ('2'), hardlinks to earlier entries ('1'), and the pax extended header ('x') for path / linkpath values that
 * do not fit the 100-byte ustar fields (GNU 'L'/'K' long names are accepted
 * too).  Device nodes are skipped.  Every name is checked
 * before it touches the filesystem: absolute paths and ".." escapes are
 * refused, because xpkg extracts as root.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include "xpkg.h"

#define USTAR_BLOCK 512
#define TAR_MAX_META (64 * 1024)

struct ustar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

static unsigned long long parse_octal(const char *field, size_t len) {
    unsigned long long v = 0;
    size_t i = 0;
    while (i < len && field[i] == ' ') i++;
    for (; i < len && field[i] >= '0' && field[i] <= '7'; i++) v = v * 8 + (unsigned)(field[i] - '0');
    return v;
}

static int checksum_ok(const unsigned char *b) {
    unsigned long want = (unsigned long)parse_octal((const char *)b + 148, 8), sum = 0;
    for (int i = 0; i < USTAR_BLOCK; i++) sum += (i >= 148 && i < 156) ? ' ' : b[i];
    return sum == want;
}

/* Relative, no "..", no empty result. */
static int path_is_safe(const char *p) {
    if (!p[0] || p[0] == '/') return 0;
    const char *c = p;
    while (*c) {
        const char *slash = strchr(c, '/');
        size_t len = slash ? (size_t)(slash - c) : strlen(c);
        if (len == 2 && c[0] == '.' && c[1] == '.') return 0;
        if (!slash) break;
        c = slash + 1;
    }
    return 1;
}

static int read_block(gzFile gz, unsigned char *b) {
    int n = gzread(gz, b, USTAR_BLOCK);
    return n == USTAR_BLOCK ? 0 : (n == 0 ? 1 : -1);
}

/* Reads an entry payload (pax / GNU long-name data) into a string. */
static char *read_payload(gzFile gz, unsigned long long size) {
    if (size > TAR_MAX_META) return NULL;
    char *buf = malloc((size_t)size + 1);
    if (!buf) return NULL;
    unsigned long long got = 0;
    unsigned char b[USTAR_BLOCK];
    while (got < size) {
        if (read_block(gz, b) != 0) { free(buf); return NULL; }
        size_t take = (size - got) < USTAR_BLOCK ? (size_t)(size - got) : USTAR_BLOCK;
        memcpy(buf + got, b, take);
        got += take;
    }
    buf[size] = '\0';
    return buf;
}

/* pax records: "<len> <key>=<value>\n" */
static void pax_parse(const char *data, char **path, char **linkpath, unsigned long long *size, int *has_size) {
    const char *p = data;
    while (*p) {
        char *end;
        unsigned long len = strtoul(p, &end, 10);
        if (end == p || *end != ' ' || len == 0) return;
        const char *rec = p;
        const char *kv = end + 1;
        const char *eq = strchr(kv, '=');
        const char *nl = rec + len - 1;
        if (!eq || eq > nl || *nl != '\n') return;
        size_t klen = (size_t)(eq - kv), vlen = (size_t)(nl - eq - 1);
        char *val = malloc(vlen + 1);
        if (!val) return;
        memcpy(val, eq + 1, vlen);
        val[vlen] = '\0';
        if (klen == 4 && !memcmp(kv, "path", 4)) { free(*path); *path = val; }
        else if (klen == 8 && !memcmp(kv, "linkpath", 8)) { free(*linkpath); *linkpath = val; }
        else if (klen == 4 && !memcmp(kv, "size", 4)) { *size = strtoull(val, NULL, 10); *has_size = 1; free(val); }
        else free(val);
        p = rec + len;
    }
}

xpkg_status_t xpkg_tar_extract(const char *archive_path, const char *dest_dir) {
    gzFile gz = gzopen(archive_path, "rb");
    if (!gz) {
        xpkg_err("cannot open archive %s", archive_path);
        return XPKG_ERR_IO;
    }
    gzbuffer(gz, 256 * 1024);

    xpkg_status_t st = XPKG_OK;
    unsigned char block[USTAR_BLOCK];
    char *long_path = NULL, *long_link = NULL;
    unsigned long long pax_size = 0;
    int has_pax_size = 0, zeros = 0;

    for (;;) {
        int r = read_block(gz, block);
        if (r == 1) break;
        if (r < 0) { xpkg_err("truncated archive %s", archive_path); st = XPKG_ERR_BAD_ARCHIVE; break; }

        int all_zero = 1;
        for (int i = 0; i < USTAR_BLOCK; i++) if (block[i]) { all_zero = 0; break; }
        if (all_zero) { if (++zeros >= 2) break; continue; }
        zeros = 0;

        if (!checksum_ok(block)) {
            xpkg_err("corrupt archive header in %s", archive_path);
            st = XPKG_ERR_BAD_ARCHIVE;
            break;
        }

        struct ustar_header *h = (struct ustar_header *)block;
        unsigned long long size = parse_octal(h->size, sizeof(h->size));
        char type = h->typeflag;

        if (type == 'x' || type == 'L' || type == 'K') {
            char *data = read_payload(gz, size);
            if (!data) { xpkg_err("bad extended header in %s", archive_path); st = XPKG_ERR_BAD_ARCHIVE; break; }
            if (type == 'x') pax_parse(data, &long_path, &long_link, &pax_size, &has_pax_size);
            else if (type == 'L') { free(long_path); long_path = data; data = NULL; }
            else { free(long_link); long_link = data; data = NULL; }
            free(data);
            continue;
        }
        if (type == 'g') {   /* global pax header: nothing we use */
            char *data = read_payload(gz, size);
            free(data);
            continue;
        }
        if (has_pax_size) size = pax_size;

        char rel[XPKG_MAX_PATH];
        if (long_path) snprintf(rel, sizeof(rel), "%s", long_path);
        else if (h->prefix[0]) snprintf(rel, sizeof(rel), "%.155s/%.100s", h->prefix, h->name);
        else snprintf(rel, sizeof(rel), "%.100s", h->name);
        /* tolerate "./x" and "x/" spellings */
        char *relp = rel;
        while (relp[0] == '.' && relp[1] == '/') relp += 2;
        size_t rl = strlen(relp);
        while (rl > 1 && relp[rl - 1] == '/') relp[--rl] = '\0';

        char linkpath[XPKG_MAX_PATH];
        if (long_link) snprintf(linkpath, sizeof(linkpath), "%s", long_link);
        else snprintf(linkpath, sizeof(linkpath), "%.100s", h->linkname);
        free(long_path); long_path = NULL;
        free(long_link); long_link = NULL;
        has_pax_size = 0;

        unsigned long long blocks = (size + USTAR_BLOCK - 1) / USTAR_BLOCK;
        if (!path_is_safe(relp)) {
            xpkg_err("refusing unsafe archive entry '%s' in %s", relp, archive_path);
            st = XPKG_ERR_BAD_ARCHIVE;
            break;
        }

        char full[XPKG_MAX_PATH];
        snprintf(full, sizeof(full), "%s/%s", dest_dir, relp);
        char parent[XPKG_MAX_PATH];
        snprintf(parent, sizeof(parent), "%s", full);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = '\0'; xpkg_mkdir_p(parent, 0755); }
        mode_t mode = (mode_t)(parse_octal(h->mode, sizeof(h->mode)) & 07777);

        if (type == '5') {
            if (mkdir(full, 0755) != 0 && errno != EEXIST) { st = XPKG_ERR_IO; break; }
            chmod(full, mode ? mode : 0755);
        } else if (type == '2') {
            unlink(full);
            if (symlink(linkpath, full) != 0) {
                xpkg_err("cannot create symlink %s: %s", full, strerror(errno));
                st = XPKG_ERR_IO;
                break;
            }
        } else if (type == '1') {
            /* hardlink to an earlier entry of this archive */
            char *lp = linkpath;
            while (lp[0] == '.' && lp[1] == '/') lp += 2;
            if (!path_is_safe(lp)) {
                xpkg_err("refusing unsafe hardlink '%s' in %s", lp, archive_path);
                st = XPKG_ERR_BAD_ARCHIVE;
                break;
            }
            char src[XPKG_MAX_PATH];
            snprintf(src, sizeof(src), "%s/%s", dest_dir, lp);
            unlink(full);
            if (link(src, full) != 0) {
                xpkg_err("cannot create hardlink %s: %s", full, strerror(errno));
                st = XPKG_ERR_IO;
                break;
            }
        } else if (type == '0' || type == '\0' || type == '7') {
            unlink(full);
            FILE *out = fopen(full, "wb");
            if (!out) { xpkg_err("cannot create %s: %s", full, strerror(errno)); st = XPKG_ERR_IO; break; }
            unsigned long long remaining = size;
            unsigned char data[USTAR_BLOCK];
            for (unsigned long long i = 0; i < blocks; i++) {
                if (read_block(gz, data) != 0) { st = XPKG_ERR_BAD_ARCHIVE; break; }
                size_t take = remaining < USTAR_BLOCK ? (size_t)remaining : USTAR_BLOCK;
                if (fwrite(data, 1, take, out) != take) { st = XPKG_ERR_IO; break; }
                remaining -= take;
            }
            if (fclose(out) != 0 && st == XPKG_OK) st = XPKG_ERR_IO;
            if (st != XPKG_OK) { xpkg_err("cannot extract %s from %s", relp, archive_path); break; }
            chmod(full, mode);
        } else {
            unsigned char skip[USTAR_BLOCK];
            for (unsigned long long i = 0; i < blocks; i++)
                if (read_block(gz, skip) != 0) { st = XPKG_ERR_BAD_ARCHIVE; break; }
            if (st != XPKG_OK) break;
        }
    }
    free(long_path);
    free(long_link);
    int zerr;
    gzerror(gz, &zerr);
    if (st == XPKG_OK && zerr != Z_OK && zerr != Z_STREAM_END) {
        xpkg_err("corrupt compressed data in %s", archive_path);
        st = XPKG_ERR_BAD_ARCHIVE;
    }
    gzclose(gz);
    return st;
}
