/* tar.c - minimal ustar reader, reading directly through zlib's gzFile
 * so gzip decompression and tar parsing happen in one pass with no
 * external `tar`/`gzip` process invoked.
 *
 * v1 scope (documented in xpkg.h): regular files and directories only.
 * No symlinks, hardlinks, or special files -- every package built so far
 * (netbsd-sh, runit, pfetch) is plain files and directories, so this
 * covers real, current need rather than speculative completeness. Revisit
 * if a future port genuinely ships a symlink that matters.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
