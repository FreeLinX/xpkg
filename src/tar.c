/* tar.c - minimal ustar reader, reading directly through zlib's gzFile
 * so gzip decompression and tar parsing happen in one pass with no
 * external `tar`/`gzip` process invoked.
 *
 * v1 scope: regular files and directories only. v2: symlinks (typeflag
 * '2') are supported — the desktop stack (openbox, st xterm/uxterm, urxvt
 * rxvt, mupdf) ships real symlinks that must survive a package round-trip.
 * Hardlinks and special files remain out of scope.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include "xpkg.h"

#define USTAR_BLOCK 512

/* Layout of a ustar header block. Field widths per the POSIX ustar spec. */
struct ustar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];      /* octal, ASCII */
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];       /* "ustar\0" */
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

static long parse_octal(const char *field, size_t len) {
    char buf[32];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, field, n);
    buf[n] = '\0';
    return strtol(buf, NULL, 8);
}

/* Rejects archive entries that would escape the extraction root via ".."
 * components in the ustar name/prefix fields (path traversal). xpkg extracts
 * and installs as root, so this must be strict: a leading ".." (going above
 * dest_dir) is refused unless every later component brings the depth back
 * to >= 0. "." components are harmless. Returns 1 when safe, 0 otherwise. */
static int entry_path_is_safe(const char *prefix, const char *name) {
    int depth = 0;
    const char *parts[2] = { prefix, name };
    for (int i = 0; i < 2; i++) {
        const char *p = parts[i];
        if (p == NULL || *p == '\0')
            continue;
        const char *c = p;
        while (*c) {
            const char *slash = strchr(c, '/');
            size_t len = slash != NULL ? (size_t)(slash - c) : strlen(c);
            if (len == 2 && c[0] == '.' && c[1] == '.') {
                if (--depth < 0)
                    return 0; /* escapes the extraction root */
            } else if (len == 1 && c[0] == '.') {
                /* current directory: no depth change */
            } else if (len > 0) {
                depth++;
            }
            if (slash == NULL)
                break;
            c = slash + 1;
        }
    }
    return 1;
}

/* Creates every path component of `path` (excluding the final component,
 * which the caller creates itself as either a file or a directory). Mirrors
 * `mkdir -p $(dirname path)`. */
static void mkdir_parents(const char *path) {
    char tmp[XPKG_MAX_PATH];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

xpkg_status_t xpkg_tar_extract(const char *archive_path, const char *dest_dir) {
    gzFile gz = gzopen(archive_path, "rb");
    if (!gz) {
        fprintf(stderr, "xpkg: cannot open archive: %s\n", archive_path);
        return XPKG_ERR_IO;
    }

    unsigned char block[USTAR_BLOCK];
    int zero_blocks_seen = 0; /* two consecutive all-zero blocks = end of archive */

    while (1) {
        int n = gzread(gz, block, USTAR_BLOCK);
        if (n == 0) {
            break; /* clean EOF */
        }
        if (n != USTAR_BLOCK) {
            fprintf(stderr, "xpkg: truncated archive: %s\n", archive_path);
            gzclose(gz);
            return XPKG_ERR_BAD_ARCHIVE;
        }

        /* Check for an all-zero block (archive terminator). */
        int all_zero = 1;
        for (int i = 0; i < USTAR_BLOCK; i++) {
            if (block[i] != 0) { all_zero = 0; break; }
        }
        if (all_zero) {
            zero_blocks_seen++;
            if (zero_blocks_seen >= 2) break;
            continue;
        }
        zero_blocks_seen = 0;

        struct ustar_header *hdr = (struct ustar_header *)block;

        /* Refuse path traversal before touching the filesystem. */
        if (!entry_path_is_safe(hdr->prefix, hdr->name)) {
            fprintf(stderr, "xpkg: refusing unsafe archive entry: %s/%s\n",
                    hdr->prefix, hdr->name);
            gzclose(gz);
            return XPKG_ERR_BAD_ARCHIVE;
        }

        /* Build the full extraction path: dest_dir + "/" + prefix + name.
         * Long-name "prefix" splitting is part of the ustar spec for paths
         * over 100 chars; every path xpkg deals with so far is short, but
         * honoring prefix costs nothing and avoids silent truncation later. */
        char full_path[XPKG_MAX_PATH];
        if (hdr->prefix[0] != '\0') {
            snprintf(full_path, sizeof(full_path), "%s/%.155s/%.100s",
                      dest_dir, hdr->prefix, hdr->name);
        } else {
            snprintf(full_path, sizeof(full_path), "%s/%.100s", dest_dir, hdr->name);
        }

        long size = parse_octal(hdr->size, sizeof(hdr->size));
        long blocks_for_data = (size + USTAR_BLOCK - 1) / USTAR_BLOCK;

        if (hdr->typeflag == '5') {
            /* Directory entry. */
            mkdir_parents(full_path);
            mkdir(full_path, 0755);
        } else if (hdr->typeflag == '2') {
            /* Symlink: recreate the link. The target is stored in the
             * linkname field (100 bytes, may not be NUL-terminated). */
            char linkname[101];
            memcpy(linkname, hdr->linkname, 100);
            linkname[100] = '\0';

            mkdir_parents(full_path);
            if (symlink(linkname, full_path) != 0) {
                fprintf(stderr, "xpkg: cannot create symlink %s -> %s\n",
                        full_path, linkname);
                gzclose(gz);
                return XPKG_ERR_IO;
            }
        } else if (hdr->typeflag == '0' || hdr->typeflag == '\0') {
            /* Regular file. */
            mkdir_parents(full_path);

            FILE *out = fopen(full_path, "wb");
            if (!out) {
                fprintf(stderr, "xpkg: cannot create %s\n", full_path);
                gzclose(gz);
                return XPKG_ERR_IO;
            }

            long remaining = size;
            unsigned char data_block[USTAR_BLOCK];
            for (long i = 0; i < blocks_for_data; i++) {
                if (gzread(gz, data_block, USTAR_BLOCK) != USTAR_BLOCK) {
                    fprintf(stderr, "xpkg: truncated file data in %s\n", archive_path);
                    fclose(out);
                    gzclose(gz);
                    return XPKG_ERR_BAD_ARCHIVE;
                }
                long to_write = remaining < USTAR_BLOCK ? remaining : USTAR_BLOCK;
                fwrite(data_block, 1, (size_t)to_write, out);
                remaining -= to_write;
            }
            fclose(out);

            /* Apply the mode recorded in the header. Best-effort: not
             * fatal if it fails (matches the "mounts are best-effort"
             * philosophy already used in rootfs/init). */
            long mode = parse_octal(hdr->mode, sizeof(hdr->mode));
            chmod(full_path, (mode_t)mode);
        } else {
            /* Symlink/hardlink/device/etc: skip the entry's data blocks
             * but do not fail the whole extraction. v1 scope, see file
             * header comment. */
            for (long i = 0; i < blocks_for_data; i++) {
                unsigned char skip_block[USTAR_BLOCK];
                gzread(gz, skip_block, USTAR_BLOCK);
            }
        }
    }

    gzclose(gz);
    return XPKG_OK;
}
