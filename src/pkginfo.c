/* pkginfo.c - parses a package's pkg-info metadata file.
 *
 * Format (plain key=value text, one per line, matching the same style
 * already used by FreeLinX's own /etc/os-release):
 *
 *     NAME=mypackage
 *     VERSION=1.0
 *     DESCRIPTION=Does a thing
 *     ARCH=x86_64
 *     DEPENDS=libfoo,libbar
 *
 * DEPENDS is comma-separated, no spaces. Unknown keys are ignored rather
 * than rejected, so pkg-info can grow new fields later without breaking
 * older xpkg builds parsing newer packages.
 *
 * The same metadata can be read straight out of an archive via
 * xpkg_pkginfo_from_archive() (a gzFile ustar scan, no extraction), which
 * the dependency resolver uses to learn DEPENDS before installing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <zlib.h>
#include "xpkg.h"

#define PKGINFO_BLOCK 512

static void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

static void split_depends(xpkg_info_t *out, const char *value) {
    char buf[XPKG_MAX_LINE];
    strncpy(buf, value, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    out->depends_count = 0;
    char *tok = strtok(buf, ",");
    while (tok != NULL && out->depends_count < XPKG_MAX_DEPENDS) {
        while (isspace((unsigned char)*tok)) tok++; /* skip leading space */
        strncpy(out->depends[out->depends_count], tok, XPKG_MAX_NAME - 1);
        out->depends[out->depends_count][XPKG_MAX_NAME - 1] = '\0';
        out->depends_count++;
        tok = strtok(NULL, ",");
    }
}

/* Core parser over an in-memory, NUL-terminated pkg-info document. */
static xpkg_status_t parse_pkginfo_data(const char *data, xpkg_info_t *out) {
    memset(out, 0, sizeof(*out));
    strncpy(out->arch, "x86_64", sizeof(out->arch) - 1); /* sensible default */

    char *buf = strdup(data);
    if (!buf) return XPKG_ERR_BAD_PKGINFO;

    char *line = strtok(buf, "\n");
    while (line) {
        trim_newline(line);
        if (line[0] == '\0' || line[0] == '#') {
            line = strtok(NULL, "\n");
            continue; /* blank line or comment */
        }

        char *eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            const char *key = line;
            const char *value = eq + 1;

            if (strcmp(key, "NAME") == 0) {
                strncpy(out->name, value, sizeof(out->name) - 1);
            } else if (strcmp(key, "VERSION") == 0) {
                strncpy(out->version, value, sizeof(out->version) - 1);
            } else if (strcmp(key, "DESCRIPTION") == 0) {
                strncpy(out->description, value, sizeof(out->description) - 1);
            } else if (strcmp(key, "ARCH") == 0) {
                strncpy(out->arch, value, sizeof(out->arch) - 1);
            } else if (strcmp(key, "DEPENDS") == 0) {
                split_depends(out, value);
            }
            /* unknown keys: ignored on purpose, see file header comment */
        }
        line = strtok(NULL, "\n");
    }
    free(buf);

    if (out->name[0] == '\0' || out->version[0] == '\0') {
        /* NAME and VERSION are the only truly required fields -- everything
         * else has a sane default or is optional. */
        return XPKG_ERR_BAD_PKGINFO;
    }

    return XPKG_OK;
}

xpkg_status_t xpkg_parse_pkginfo(const char *path, xpkg_info_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return XPKG_ERR_BAD_PKGINFO;
    }

    /* Read the whole file into memory (pkg-info is tiny). */
    char *data = NULL;
    long len = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    if (len > 0 && len < XPKG_MAX_LINE * 8) {
        data = malloc((size_t)len + 1);
        if (data) {
            size_t got = fread(data, 1, (size_t)len, f);
            data[got] = '\0';
        }
    }
    fclose(f);
    if (!data) {
        return XPKG_ERR_BAD_PKGINFO;
    }

    xpkg_status_t st = parse_pkginfo_data(data, out);
    free(data);
    return st;
}

xpkg_status_t xpkg_pkginfo_from_archive(const char *archive_path, xpkg_info_t *out) {
    gzFile gz = gzopen(archive_path, "rb");
    if (!gz) {
        return XPKG_ERR_IO;
    }

    unsigned char block[PKGINFO_BLOCK];
    while (gzread(gz, block, PKGINFO_BLOCK) == PKGINFO_BLOCK) {
        /* all-zero block = archive terminator */
        int all_zero = 1;
        for (int i = 0; i < PKGINFO_BLOCK; i++) {
            if (block[i] != 0) { all_zero = 0; break; }
        }
        if (all_zero) {
            break;
        }

        char name[101];
        memcpy(name, block, 100);
        name[100] = '\0';
        char tflag = block[156];

        char sizebuf[13];
        memcpy(sizebuf, block + 124, 12);
        sizebuf[12] = '\0';
        unsigned long size = strtoul(sizebuf, NULL, 8);
        unsigned long blocks = (size + PKGINFO_BLOCK - 1) / PKGINFO_BLOCK;

        if (tflag == '0' && strcmp(name, "pkg-info") == 0) {
            char *buf = malloc((size_t)size + 1);
            if (!buf) {
                gzclose(gz);
                return XPKG_ERR_BAD_PKGINFO;
            }
            size_t got = 0;
            unsigned char db[PKGINFO_BLOCK];
            while (got < size && gzread(gz, db, PKGINFO_BLOCK) > 0) {
                size_t take = (size - got) < PKGINFO_BLOCK ? (size - got) : PKGINFO_BLOCK;
                memcpy(buf + got, db, take);
                got += take;
            }
            buf[got] = '\0';
            gzclose(gz);
            xpkg_status_t st = parse_pkginfo_data(buf, out);
            free(buf);
            return st;
        } else {
            /* skip this entry's data blocks */
            unsigned char db[PKGINFO_BLOCK];
            for (unsigned long i = 0; i < blocks; i++) {
                if (gzread(gz, db, PKGINFO_BLOCK) != PKGINFO_BLOCK) {
                    gzclose(gz);
                    return XPKG_ERR_BAD_ARCHIVE;
                }
            }
        }
    }

    gzclose(gz);
    return XPKG_ERR_BAD_PKGINFO;
}