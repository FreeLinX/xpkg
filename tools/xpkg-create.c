/* xpkg-create.c - FreeLinX .xpkg package builder and repo index generator.
 *
 * xpkg-create has two jobs:
 *
 *   1. "create"  - turn a finished port's staging tree (the rootfs-compatible
 *                  overlay that FreeLinX/ports fills, e.g. staging/usr/bin/less)
 *                  into a .xpkg package: a gzip'd ustar archive holding a
 *                  top-level "pkg-info" metadata file plus a "files/" tree
 *                  extracted relative to / on install.
 *   2. "index"   - scan a directory of *.xpkg files and write index.json for
 *                  a repo: name -> {url, version, description, arch, sha256,
 *                  size, depends}.
 *
 * The ustar writer here MUST stay byte-compatible with the reader in
 * src/tar.c (same field widths, typeflags '0'/'5', octal sizes, 512-byte
 * blocks, 2 zero-block end marker, optional prefix/name 155+100 split).
 * Keep the two in lockstep.
 *
 * xpkg-create is a host/build tool (it runs on the machine doing the
 * packaging, not on a FreeLinX target), but it is built with the same
 * FreeLinX clang + lld + musl toolchain and links the same zlib, keeping
 * everything first-party and consistent.
 *
 * Usage:
 *   xpkg-create create --name NAME --version V \
 *       [--description ".."] [--arch ARCH] --stage DIR --output OUT.xpkg
 *   xpkg-create index --dir DIR --output index.json
 *
 * --stage DIR: a rootfs-relative staging tree. Every path under DIR becomes
 *   files/<relpath> in the package (installed to /<relpath>). Prefer staging
 *   trees that contain only the files this package owns (e.g. staging/usr/bin)
 *   so 'remove' can cleanly own them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>
#include <openssl/evp.h>

#define USTAR_BLOCK 512
#define PATH_MAX_LOCAL 4096

/* Plain sexagesimal-ish octal formatting helper. */
static void set_octal(char *field, size_t len, unsigned long value) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%0*lo", (int)(len - 1), value);
    memcpy(field, buf, len - 1);
    /* field[len-1] stays '\0' -> oct field ends with a null, as ustar wants */
}

/* Compute the ustar checksum: sum of the header bytes with the checksum
 * field treated as all spaces. */
static unsigned int ustar_checksum(const unsigned char *hdr) {
    unsigned int sum = 0;
    for (int i = 0; i < USTAR_BLOCK; i++) {
        /* Checksum field occupies bytes 148..155 inclusive */
        if (i >= 148 && i < 156) {
            sum += ' ';
        } else {
            sum += hdr[i];
        }
    }
    return sum;
}

struct filespec {
    char relpath[PATH_MAX_LOCAL];  /* path as stored in the archive (files/...) */
    char ondisk[PATH_MAX_LOCAL];   /* path on the build host to read content from */
    unsigned long size;
    mode_t mode;
    int is_dir;
};

#define MAX_FILES 16384
static struct filespec g_entries[MAX_FILES];
static int g_nentries = 0;

/* Recursively collect a staging dir's entries into g_entries, mapping each
 * host path DIR/x to archive path "files/x". */
static int collect_dir(const char *dir, const char *arch_prefix) {
    DIR *d = opendir(dir);
    if (!d) {
        fprintf(stderr, "xpkg-create: cannot open staging dir %s: %s\n", dir, strerror(errno));
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        if (g_nentries >= MAX_FILES) {
            fprintf(stderr, "xpkg-create: too many files (limit %d)\n", MAX_FILES);
            closedir(d);
            return -1;
        }
        char disk[PATH_MAX_LOCAL], arch_path[PATH_MAX_LOCAL];
        snprintf(disk, sizeof(disk), "%s/%s", dir, e->d_name);
        if (arch_prefix[0]) {
            snprintf(arch_path, sizeof(arch_path), "files/%s/%s", arch_prefix, e->d_name);
        } else {
            snprintf(arch_path, sizeof(arch_path), "files/%s", e->d_name);
        }

        struct stat st;
        if (lstat(disk, &st) != 0) {
            fprintf(stderr, "xpkg-create: lstat %s: %s\n", disk, strerror(errno));
            closedir(d);
            return -1;
        }
        if (S_ISREG(st.st_mode)) {
            struct filespec *f = &g_entries[g_nentries++];
            strncpy(f->relpath, arch_path, sizeof(f->relpath) - 1);
            strncpy(f->ondisk, disk, sizeof(f->ondisk) - 1);
            f->size = (unsigned long)st.st_size;
            f->mode = st.st_mode & 07777;
            f->is_dir = 0;
        } else if (S_ISDIR(st.st_mode)) {
            struct filespec *f = &g_entries[g_nentries++];
            strncpy(f->relpath, arch_path, sizeof(f->relpath) - 1);
            strncpy(f->ondisk, disk, sizeof(f->ondisk) - 1);
            f->size = 0;
            f->mode = 0755;
            f->is_dir = 1;
            /* recurse with this dir as the new prefix */
            if (collect_dir(disk, arch_path + strlen("files/")) != 0) {
                closedir(d);
                return -1;
            }
        }
        /* other types (symlink, special): skipped, matching tar.c's scope */
    }
    closedir(d);
    return 0;
}

/* Write one ustar header block for the given archive file. */
static void write_header(gzFile gz, const char *path, unsigned long size,
                         mode_t mode, char typeflag) {
    unsigned char block[USTAR_BLOCK];
    memset(block, '\0', USTAR_BLOCK);

    /* ustar prefix/name split: names over 100 chars use up to 155 prefix. */
    char name[101] = {0}, prefix[156] = {0};
    size_t plen = strlen(path);
    if (plen > 100) {
        /* find split at 100-char boundary, keeping '/' boundaries */
        size_t split = plen > 100 ? plen - 100 : 0;
        while (split > 0 && path[split] != '/') split--;
        if (split > 155) split = 155;
        memcpy(prefix, path, split);
        memcpy(name, path + split, plen - split);
    } else {
        memcpy(name, path, plen);
    }

    memcpy(block, name, 100);             /* name */
    set_octal((char *)block + 100, 8, (unsigned long)mode);   /* mode */
    set_octal((char *)block + 108, 8, 0); /* uid */
    set_octal((char *)block + 116, 8, 0); /* gid */
    set_octal((char *)block + 124, 12, size);                 /* size */
    set_octal((char *)block + 136, 12, 0); /* mtime */
    /* chksum field 148..156: leave NUL, patched below */
    block[156] = typeflag;                /* typeflag */
    /* magic "ustar\0" + "00" */
    memcpy(block + 257, "ustar", 6);
    block[263] = '0'; block[264] = '0';
    /* uname/gname/prefix: NUL-filled (zero), as ustar requires */
    if (prefix[0]) {
        memcpy(block + 345, prefix, strlen(prefix));
    }

    /* checksum: compute over the block with the checksum field as spaces */
    /* first set 148..154 to spaces */
    memset(block + 148, ' ', 6);
    block[154] = '\0';
    block[155] = ' ';
    unsigned int sum = ustar_checksum(block);
    char chksum[8];
    snprintf(chksum, sizeof(chksum), "%06o", sum);
    memcpy(block + 148, chksum, 6);
    block[154] = '\0'; block[155] = ' ';

    gzwrite(gz, block, USTAR_BLOCK);
}

/* Write file data padded to a 512 boundary. */
static void write_payload(gzFile gz, FILE *in, unsigned long size) {
    unsigned char buf[USTAR_BLOCK];
    unsigned long remaining = size;
    while (remaining > 0) {
        size_t chunk = remaining < USTAR_BLOCK ? (size_t)remaining : USTAR_BLOCK;
        size_t got = fread(buf, 1, chunk, in);
        if (got != chunk) { /* zero-fill on short read to keep alignment */
            memset(buf + got, 0, chunk - got);
        }
        gzwrite(gz, buf, chunk);
        remaining -= chunk;
    }
    /* pad to block boundary */
    size_t pad = (USTAR_BLOCK - (size % USTAR_BLOCK)) % USTAR_BLOCK;
    if (pad) {
        unsigned char z[USTAR_BLOCK];
        memset(z, 0, sizeof(z));
        gzwrite(gz, z, pad);
    }
}

static int cmd_create(int argc, char **argv) {
    const char *name = NULL, *version = NULL, *desc = "No description";
    const char *arch = "x86_64", *stage = NULL, *output = NULL;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--version") && i + 1 < argc) version = argv[++i];
        else if (!strcmp(argv[i], "--description") && i + 1 < argc) desc = argv[++i];
        else if (!strcmp(argv[i], "--arch") && i + 1 < argc) arch = argv[++i];
        else if (!strcmp(argv[i], "--stage") && i + 1 < argc) stage = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) output = argv[++i];
        else {
            fprintf(stderr, "xpkg-create: unknown arg: %s\n", argv[i]);
            return 2;
        }
    }
    if (!name || !version || !stage || !output) {
        fprintf(stderr,
                "usage: xpkg-create create --name N --version V "
                "[--description D] [--arch A] --stage DIR --output OUT.xpkg\n");
        return 2;
    }

    g_nentries = 0;
    if (collect_dir(stage, "") != 0) {
        return 1;
    }

    gzFile gz = gzopen(output, "wb6");
    if (!gz) {
        fprintf(stderr, "xpkg-create: cannot open output %s\n", output);
        return 1;
    }

    /* 1) pkg-info metadata file */
    FILE *pkginfo = tmpfile();
    if (!pkginfo) {
        fprintf(stderr, "xpkg-create: tmpfile failed\n");
        gzclose(gz);
        return 1;
    }
    fprintf(pkginfo, "NAME=%s\n", name);
    fprintf(pkginfo, "VERSION=%s\n", version);
    fprintf(pkginfo, "DESCRIPTION=%s\n", desc);
    fprintf(pkginfo, "ARCH=%s\n", arch);
    fflush(pkginfo);

    long pkginfo_size;
    {
        fseek(pkginfo, 0, SEEK_END);
        pkginfo_size = ftell(pkginfo);
        fseek(pkginfo, 0, SEEK_SET);
    }
    write_header(gz, "pkg-info", (unsigned long)pkginfo_size, 0644, '0');
    {
        unsigned char buf[USTAR_BLOCK];
        long remaining = pkginfo_size;
        while (remaining > 0) {
            size_t chunk = remaining < USTAR_BLOCK ? (size_t)remaining : USTAR_BLOCK;
            size_t got = fread(buf, 1, chunk, pkginfo);
            gzwrite(gz, buf, got);
            remaining -= (long)got;
        }
        size_t pad = (USTAR_BLOCK - (pkginfo_size % USTAR_BLOCK)) % USTAR_BLOCK;
        if (pad) {
            unsigned char z[USTAR_BLOCK]; memset(z, 0, sizeof(z));
            gzwrite(gz, z, pad);
        }
    }
    fclose(pkginfo);

    /* 2) files/ tree: directories first, then files (in collection order). */
    for (int i = 0; i < g_nentries; i++) {
        struct filespec *f = &g_entries[i];
        if (f->is_dir) {
            write_header(gz, f->relpath, 0, f->mode, '5');
        }
    }
    for (int i = 0; i < g_nentries; i++) {
        struct filespec *f = &g_entries[i];
        if (f->is_dir) {
            continue;
        }
        write_header(gz, f->relpath, f->size, f->mode, '0');
        FILE *in = fopen(f->ondisk, "rb");
        if (!in) {
            fprintf(stderr, "xpkg-create: cannot read %s\n", f->ondisk);
            gzclose(gz);
            return 1;
        }
        write_payload(gz, in, f->size);
        fclose(in);
    }

    /* 3) two zero blocks end the archive */
    unsigned char term[USTAR_BLOCK];
    memset(term, 0, sizeof(term));
    gzwrite(gz, term, USTAR_BLOCK);
    gzwrite(gz, term, USTAR_BLOCK);

    gzclose(gz);
    printf("xpkg-create: wrote %s (%d files)\n", output, g_nentries);
    return 0;
}

/* index: scan a dir of .xpkg files, emit index.json.
 * For each package, read its pkg-info from inside the archive (gzip data,
 * then ustar scan) to extract the index fields without extracting to disk. */
static int read_pkginfo_field(const char *path, const char *key, char *out, size_t outsz) {
    gzFile gz = gzopen(path, "rb");
    if (!gz) return -1;
    out[0] = '\0';
    int found = 0;
    unsigned char block[USTAR_BLOCK];
    /* walk headers until we find pkg-info */
    while (1) {
        int n = gzread(gz, block, USTAR_BLOCK);
        if (n != USTAR_BLOCK) break;
        /* header interpretation: parse size+type+name */
        char name[102]; memcpy(name, block, 100); name[100] = 0;
        char sizebuf[13]; memcpy(sizebuf, block + 124, 12); sizebuf[12] = 0;
        char tflag = block[156];
        unsigned long size = strtoul(sizebuf, NULL, 8);
        unsigned long blocks = (size + USTAR_BLOCK - 1) / USTAR_BLOCK;

        if (tflag == '0' && strcmp(name, "pkg-info") == 0) {
            /* read payload, scan for key=value lines */
            unsigned char *buf = malloc(size + 1);
            if (!buf) { gzclose(gz); return -1; }
            unsigned long got = 0;
            while (got < size) {
                unsigned char db[USTAR_BLOCK];
                int r = gzread(gz, db, USTAR_BLOCK);
                if (r <= 0) break;
                size_t c = (size - got) < USTAR_BLOCK ? (size - got) : USTAR_BLOCK;
                memcpy(buf + got, db, c);
                got += (unsigned long)c;
            }
            buf[got] = 0;
            /* parse */
            char *tok = strtok((char *)buf, "\n");
            while (tok) {
                char *nl = strchr(tok, '\r'); if (nl) *nl = 0;
                char *eq = strchr(tok, '=');
                if (eq && !strncmp(tok, key, strlen(key)) && (eq - tok) == (long)strlen(key)) {
                    snprintf(out, outsz, "%s", eq + 1);
                    found = 1;
                    break;
                }
                tok = strtok(NULL, "\n");
            }
            free(buf);
            break;
        } else {
            /* skip data blocks */
            for (unsigned long i = 0; i < blocks; i++) {
                unsigned char db[USTAR_BLOCK];
                int r = gzread(gz, db, USTAR_BLOCK);
                if (r != USTAR_BLOCK) break;
            }
        }
    }
    gzclose(gz);
    return found ? 0 : -1;
}

static void sha256_file(const char *path, char out[65]) {
    /* Use OpenSSL like xpkg's hash.c so the index sha256 matches what
     * xpkg verifies against. */
    out[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) { out[0] = '\0'; return; }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        if (ctx) EVP_MD_CTX_free(ctx);
        fclose(f);
        out[0] = '\0';
        return;
    }
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        EVP_DigestUpdate(ctx, buf, n);
    }
    fclose(f);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len;
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);
    for (unsigned int i = 0; i < digest_len; i++) {
        snprintf(out + (i * 2), 3, "%02x", digest[i]);
    }
    out[digest_len * 2] = '\0';
}

static int cmd_index(int argc, char **argv) {
    const char *dir = ".", *output = "index.json";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) output = argv[++i];
    }

    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "xpkg-create: cannot open %s\n", dir); return 1; }

    FILE *out = fopen(output, "w");
    if (!out) { fprintf(stderr, "xpkg-create: cannot write %s\n", output); return 1; }

    fprintf(out, "{\n  \"packages\": {\n");
    int first = 1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 6 || strcmp(e->d_name + len - 5, ".xpkg") != 0) continue;
        char full[PATH_MAX_LOCAL];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);

        char name[256], version[128], desc[1024], arch[128];
        read_pkginfo_field(full, "NAME", name, sizeof(name));
        read_pkginfo_field(full, "VERSION", version, sizeof(version));
        read_pkginfo_field(full, "DESCRIPTION", desc, sizeof(desc));
        read_pkginfo_field(full, "ARCH", arch, sizeof(arch));

        struct stat st; stat(full, &st);
        char sha[65]; sha256_file(full, sha);

        if (!first) fprintf(out, ",\n");
        first = 0;
        fprintf(out, "    \"%s\": {\n", name);
        fprintf(out, "      \"version\": \"%s\",\n", version);
        fprintf(out, "      \"description\": \"%s\",\n", desc);
        fprintf(out, "      \"arch\": \"%s\",\n", arch);
        fprintf(out, "      \"file\": \"%s\",\n", e->d_name);
        fprintf(out, "      \"size\": %lu,\n", (unsigned long)st.st_size);
        fprintf(out, "      \"sha256\": \"%s\"\n", sha);
        fprintf(out, "    }");
    }
    fprintf(out, "\n  }\n}\n");
    fflush(out);
    fclose(out);
    closedir(d);
    printf("xpkg-create: wrote %s\n", output);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: xpkg-create <create|index> [options]\n");
        return 2;
    }
    if (strcmp(argv[1], "create") == 0) {
        return cmd_create(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "index") == 0) {
        return cmd_index(argc - 2, argv + 2);
    }
    fprintf(stderr, "xpkg-create: unknown subcommand: %s\n", argv[1]);
    return 2;
}
