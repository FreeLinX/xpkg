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
 *   xpkg-create create --name NAME --version V [--description ".."]
 *       [--arch ARCH] [--depends a,b] [--post-install FILE] [--pre-remove FILE]
 *       --stage DIR --output OUT.xpkg
 *   xpkg-create index --dir DIR --output index.json
 *   xpkg-create keygen --out NAME          writes NAME.key (secret), NAME.pub
 *   xpkg-create sign --key NAME.key FILE   writes FILE.sig (Ed25519, base64)
 *
 * Archives are reproducible: entries are sorted and carry no timestamps or
 * owners.  Paths and link targets longer than the 100-byte ustar fields go
 * into pax extended headers, which xpkg >= 1.0 reads.
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
#include <fcntl.h>
#include <time.h>
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
    char linkname[PATH_MAX_LOCAL]; /* symlink target, when is_symlink */
    unsigned long size;
    mode_t mode;
    int is_dir;
    int is_symlink;
    int is_hardlink;               /* linkname = archive path of the first copy */
    dev_t dev;
    ino_t ino;
    nlink_t nlink;
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
            f->is_symlink = 0;
            f->is_hardlink = 0;
            f->dev = st.st_dev;
            f->ino = st.st_ino;
            f->nlink = st.st_nlink;
            f->linkname[0] = '\0';
        } else if (S_ISDIR(st.st_mode)) {
            struct filespec *f = &g_entries[g_nentries++];
            strncpy(f->relpath, arch_path, sizeof(f->relpath) - 1);
            strncpy(f->ondisk, disk, sizeof(f->ondisk) - 1);
            f->size = 0;
            f->mode = st.st_mode & 07777;
            f->is_dir = 1;
            f->is_symlink = 0;
            f->linkname[0] = '\0';
            /* recurse with this dir as the new prefix */
            if (collect_dir(disk, arch_path + strlen("files/")) != 0) {
                closedir(d);
                return -1;
            }
        } else if (S_ISLNK(st.st_mode)) {
            /* Symlinks are real: record the target and pack it as a ustar
             * typeflag '2' entry (no payload). The client recreates them on
             * install, so packages faithfully reproduce the tested rootfs. */
            struct filespec *f = &g_entries[g_nentries++];
            strncpy(f->relpath, arch_path, sizeof(f->relpath) - 1);
            strncpy(f->ondisk, disk, sizeof(f->ondisk) - 1);
            f->size = 0;
            f->mode = 0777;
            f->is_dir = 0;
            f->is_symlink = 1;
            ssize_t ln = readlink(disk, f->linkname, sizeof(f->linkname) - 1);
            if (ln < 0) {
                fprintf(stderr, "xpkg-create: readlink %s: %s\n", disk, strerror(errno));
                closedir(d);
                return -1;
            }
            f->linkname[ln] = '\0';
        }
        /* other types (special): skipped, matching tar.c's scope */
    }
    closedir(d);
    return 0;
}

static int cmp_entries(const void *a, const void *b) {
    const struct filespec *x = a, *y = b;
    return strcmp(x->relpath, y->relpath);
}

static void write_header(gzFile gz, const char *path, unsigned long size,
                         mode_t mode, char typeflag, const char *linkname);

/* pax record "<len> key=value\n", where len counts the whole record */
static size_t pax_record(char *out, size_t cap, const char *key, const char *val) {
    size_t body = strlen(key) + strlen(val) + 3;   /* ' ' '=' '\n' */
    size_t len = body + 1;
    for (;;) {
        char tmp[32];
        size_t d = (size_t)snprintf(tmp, sizeof(tmp), "%zu", len);
        if (body + d == len) break;
        len = body + d;
    }
    return (size_t)snprintf(out, cap, "%zu %s=%s\n", len, key, val);
}

/* Emits a pax 'x' header when path or link target do not fit ustar. */
static void write_pax_if_needed(gzFile gz, const char *path, const char *linkname) {
    int long_path = strlen(path) > 100, long_link = linkname && strlen(linkname) > 100;
    if (!long_path && !long_link) return;
    char data[3 * PATH_MAX_LOCAL];
    size_t n = 0;
    if (long_path) n += pax_record(data + n, sizeof(data) - n, "path", path);
    if (long_link) n += pax_record(data + n, sizeof(data) - n, "linkpath", linkname);
    write_header(gz, "././@PaxHeader", (unsigned long)n, 0644, 'x', NULL);
    gzwrite(gz, data, (unsigned)n);
    size_t pad = (USTAR_BLOCK - (n % USTAR_BLOCK)) % USTAR_BLOCK;
    if (pad) {
        unsigned char z[USTAR_BLOCK];
        memset(z, 0, sizeof(z));
        gzwrite(gz, z, (unsigned)pad);
    }
}

/* Write one ustar header block for the given archive file. linkname is
 * written into the header only for symlink ('2') entries; it may be NULL
 * otherwise. */
static void write_header(gzFile gz, const char *path, unsigned long size,
                         mode_t mode, char typeflag, const char *linkname) {
    unsigned char block[USTAR_BLOCK];
    memset(block, '\0', USTAR_BLOCK);

    /* ustar prefix/name split: names over 100 chars use up to 155 prefix. */
    char name[101] = {0}, prefix[156] = {0};
    size_t plen = strlen(path);
    if (plen > 100) {
        /* ustar split at a '/': prefix (<=155) '/' name (<=100); the pax
         * header written before this one carries the exact path anyway */
        size_t split = plen - 101;
        while (split < plen && path[split] != '/') split++;
        if (split < plen && split <= 155 && plen - split - 1 <= 100) {
            memcpy(prefix, path, split);
            memcpy(name, path + split + 1, plen - split - 1);
        } else {
            memcpy(name, path, 100);
        }
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
    /* linkname field at offset 157 (ustar), 100 bytes: only for '2'. */
    if ((typeflag == '2' || typeflag == '1') && linkname && linkname[0]) {
        memcpy(block + 157, linkname, strlen(linkname) > 100 ? 100 : strlen(linkname));
    }
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
    const char *depends = NULL, *post_install = NULL, *pre_remove = NULL;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--version") && i + 1 < argc) version = argv[++i];
        else if (!strcmp(argv[i], "--description") && i + 1 < argc) desc = argv[++i];
        else if (!strcmp(argv[i], "--arch") && i + 1 < argc) arch = argv[++i];
        else if (!strcmp(argv[i], "--depends") && i + 1 < argc) depends = argv[++i];
        else if (!strcmp(argv[i], "--stage") && i + 1 < argc) stage = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) output = argv[++i];
        else if (!strcmp(argv[i], "--post-install") && i + 1 < argc) post_install = argv[++i];
        else if (!strcmp(argv[i], "--pre-remove") && i + 1 < argc) pre_remove = argv[++i];
        else {
            fprintf(stderr, "xpkg-create: unknown arg: %s\n", argv[i]);
            return 2;
        }
    }
    if (!name || !version || !stage || !output) {
        fprintf(stderr,
                "usage: xpkg-create create --name N --version V "
                "[--description D] [--arch A] [--depends X,Y] "
                "--stage DIR --output OUT.xpkg\n");
        return 2;
    }

    g_nentries = 0;
    if (collect_dir(stage, "") != 0) {
        return 1;
    }
    qsort(g_entries, (size_t)g_nentries, sizeof(g_entries[0]), cmp_entries);
    /* hardlinked files (Mesa's *_dri.so are one driver under many names):
     * the first path in archive order carries the data, the others are
     * ustar hardlink entries naming it, so the package and the installed
     * system keep a single copy */
    for (int i = 0; i < g_nentries; i++) {
        struct filespec *f = &g_entries[i];
        if (f->is_dir || f->is_symlink || f->nlink < 2) continue;
        for (int j = 0; j < i; j++) {
            struct filespec *o = &g_entries[j];
            if (!o->is_dir && !o->is_symlink && !o->is_hardlink && o->dev == f->dev && o->ino == f->ino) {
                f->is_hardlink = 1;
                snprintf(f->linkname, sizeof(f->linkname), "%s", o->relpath);
                break;
            }
        }
    }
    if (strchr(desc, '\n') || strchr(version, '\n') || strchr(name, '\n')) {
        fprintf(stderr, "xpkg-create: newline in metadata\n");
        return 2;
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
    if (depends && depends[0]) {
        fprintf(pkginfo, "DEPENDS=%s\n", depends);
    }
    fflush(pkginfo);

    long pkginfo_size;
    {
        fseek(pkginfo, 0, SEEK_END);
        pkginfo_size = ftell(pkginfo);
        fseek(pkginfo, 0, SEEK_SET);
    }
    write_header(gz, "pkg-info", (unsigned long)pkginfo_size, 0644, '0', NULL);
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

    /* 1b) optional scripts */
    const char *script_src[2] = { post_install, pre_remove };
    const char *script_name[2] = { "post-install", "pre-remove" };
    for (int k = 0; k < 2; k++) {
        if (!script_src[k]) continue;
        struct stat sst;
        FILE *in = fopen(script_src[k], "rb");
        if (!in || fstat(fileno(in), &sst) != 0) {
            fprintf(stderr, "xpkg-create: cannot read %s\n", script_src[k]);
            if (in) fclose(in);
            gzclose(gz);
            return 1;
        }
        write_header(gz, script_name[k], (unsigned long)sst.st_size, 0755, '0', NULL);
        write_payload(gz, in, (unsigned long)sst.st_size);
        fclose(in);
    }

    /* 2) files/ tree: directories first, then files (in collection order). */
    for (int i = 0; i < g_nentries; i++) {
        struct filespec *f = &g_entries[i];
        if (f->is_dir) {
            write_pax_if_needed(gz, f->relpath, NULL);
            write_header(gz, f->relpath, 0, f->mode, '5', NULL);
        }
    }
    for (int i = 0; i < g_nentries; i++) {
        struct filespec *f = &g_entries[i];
        if (f->is_dir) {
            continue;
        }
        if (f->is_hardlink) {
            write_pax_if_needed(gz, f->relpath, f->linkname);
            write_header(gz, f->relpath, 0, f->mode, '1', f->linkname);
            continue;
        }
        if (f->is_symlink) {
            /* symlink: typeflag '2', linkname in the header, no payload */
            write_pax_if_needed(gz, f->relpath, f->linkname);
            write_header(gz, f->relpath, 0, f->mode, '2', f->linkname);
            continue;
        }
        write_pax_if_needed(gz, f->relpath, NULL);
        write_header(gz, f->relpath, f->size, f->mode, '0', NULL);
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

static void json_str_out(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p);
        else fputc(*p, f);
    }
    fputc('"', f);
}

/* numeric-aware version compare, the same rules as xpkg's version.c */
static int vcmp(const char *a, const char *b) {
    for (;;) {
        while (*a == '.' || *a == '-' || *a == '_') a++;
        while (*b == '.' || *b == '-' || *b == '_') b++;
        if (!*a || !*b) return *a ? 1 : (*b ? -1 : 0);
        const char *ea = a, *eb = b;
        while (*ea && *ea != '.' && *ea != '-' && *ea != '_') ea++;
        while (*eb && *eb != '.' && *eb != '-' && *eb != '_') eb++;
        int da = 1, db = 1;
        for (const char *p = a; p < ea; p++) if (!isdigit((unsigned char)*p)) da = 0;
        for (const char *p = b; p < eb; p++) if (!isdigit((unsigned char)*p)) db = 0;
        int c;
        if (da && db) {
            while (a < ea - 1 && *a == '0') a++;
            while (b < eb - 1 && *b == '0') b++;
            c = (ea - a) != (eb - b) ? ((ea - a) < (eb - b) ? -1 : 1) : strncmp(a, b, (size_t)(ea - a));
        } else {
            size_t la = (size_t)(ea - a), lb = (size_t)(eb - b);
            c = strncmp(a, b, la < lb ? la : lb);
            if (!c) c = la < lb ? -1 : (la > lb ? 1 : 0);
        }
        if (c) return c;
        a = ea; b = eb;
    }
}

struct idx_ent {
    char name[256], version[128], desc[1024], arch[64], depends[1024], file[256], sha[65];
    unsigned long size;
};

static int cmp_idx(const void *a, const void *b) {
    return strcmp(((const struct idx_ent *)a)->name, ((const struct idx_ent *)b)->name);
}

/* index: every *.xpkg in DIR; when several archives carry the same NAME the
 * highest VERSION is listed (older ones stay on disk for rollback). */
static int cmd_index(int argc, char **argv) {
    const char *dir = ".", *output = "index.json";
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) output = argv[++i];
    }
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "xpkg-create: cannot open %s\n", dir); return 1; }

    size_t cap = 256, n = 0;
    struct idx_ent *v = malloc(cap * sizeof(*v));
    if (!v) { closedir(d); return 1; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 6 || strcmp(e->d_name + len - 5, ".xpkg") != 0 || len >= 256) continue;
        char full[PATH_MAX_LOCAL];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct idx_ent x;
        memset(&x, 0, sizeof(x));
        if (read_pkginfo_field(full, "NAME", x.name, sizeof(x.name)) != 0 ||
            read_pkginfo_field(full, "VERSION", x.version, sizeof(x.version)) != 0) {
            fprintf(stderr, "xpkg-create: skipping %s (no pkg-info)\n", e->d_name);
            continue;
        }
        read_pkginfo_field(full, "DESCRIPTION", x.desc, sizeof(x.desc));
        if (read_pkginfo_field(full, "ARCH", x.arch, sizeof(x.arch)) != 0) strcpy(x.arch, "x86_64");
        read_pkginfo_field(full, "DEPENDS", x.depends, sizeof(x.depends));
        snprintf(x.file, sizeof(x.file), "%s", e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        x.size = (unsigned long)st.st_size;
        sha256_file(full, x.sha);

        size_t j;
        for (j = 0; j < n; j++) if (!strcmp(v[j].name, x.name)) break;
        if (j < n) {
            if (vcmp(x.version, v[j].version) > 0) v[j] = x;
            continue;
        }
        if (n == cap) {
            struct idx_ent *nv = realloc(v, cap * 2 * sizeof(*v));
            if (!nv) { free(v); closedir(d); return 1; }
            v = nv; cap *= 2;
        }
        v[n++] = x;
    }
    closedir(d);
    qsort(v, n, sizeof(*v), cmp_idx);

    char tmp[PATH_MAX_LOCAL];
    snprintf(tmp, sizeof(tmp), "%s.tmp", output);
    FILE *out = fopen(tmp, "w");
    if (!out) { fprintf(stderr, "xpkg-create: cannot write %s\n", output); free(v); return 1; }
    fprintf(out, "{\n  \"generated\": %ld,\n  \"packages\": {\n", (long)time(NULL));
    for (size_t i = 0; i < n; i++) {
        struct idx_ent *x = &v[i];
        fprintf(out, "    ");
        json_str_out(out, x->name);
        fprintf(out, ": {\n      \"version\": ");
        json_str_out(out, x->version);
        fprintf(out, ",\n      \"description\": ");
        json_str_out(out, x->desc);
        fprintf(out, ",\n      \"arch\": ");
        json_str_out(out, x->arch);
        fprintf(out, ",\n      \"depends\": [");
        char deps[1024];
        snprintf(deps, sizeof(deps), "%s", x->depends);
        int first = 1;
        for (char *save = NULL, *t = strtok_r(deps, ", ", &save); t; t = strtok_r(NULL, ", ", &save)) {
            if (!first) fputs(", ", out);
            json_str_out(out, t);
            first = 0;
        }
        fprintf(out, "],\n      \"file\": ");
        json_str_out(out, x->file);
        fprintf(out, ",\n      \"size\": %lu,\n      \"sha256\": \"%s\"\n    }%s\n",
                x->size, x->sha, i + 1 < n ? "," : "");
    }
    fprintf(out, "  }\n}\n");
    int bad = ferror(out);
    if (fclose(out) != 0 || bad || rename(tmp, output) != 0) {
        fprintf(stderr, "xpkg-create: cannot write %s\n", output);
        unlink(tmp);
        free(v);
        return 1;
    }
    printf("xpkg-create: wrote %s (%zu packages)\n", output, n);
    free(v);
    return 0;
}

/* --- keys and signatures -------------------------------------------------- */

static int b64_out(const unsigned char *in, int n, char *out) {
    return EVP_EncodeBlock((unsigned char *)out, in, n);
}

static int cmd_keygen(int argc, char **argv) {
    const char *base = NULL;
    for (int i = 0; i < argc; i++)
        if (!strcmp(argv[i], "--out") && i + 1 < argc) base = argv[++i];
    if (!base) { fprintf(stderr, "usage: xpkg-create keygen --out NAME\n"); return 2; }
    EVP_PKEY *pk = NULL;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL);
    if (!ctx || EVP_PKEY_keygen_init(ctx) != 1 || EVP_PKEY_keygen(ctx, &pk) != 1) {
        fprintf(stderr, "xpkg-create: key generation failed\n");
        return 1;
    }
    EVP_PKEY_CTX_free(ctx);
    unsigned char priv[32], pub[32];
    size_t pl = 32, ql = 32;
    EVP_PKEY_get_raw_private_key(pk, priv, &pl);
    EVP_PKEY_get_raw_public_key(pk, pub, &ql);
    EVP_PKEY_free(pk);
    char b64[64], path[PATH_MAX_LOCAL];
    snprintf(path, sizeof(path), "%s.key", base);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { fprintf(stderr, "xpkg-create: %s: %s\n", path, strerror(errno)); return 1; }
    int l = b64_out(priv, 32, b64);
    b64[l] = '\n';
    if (write(fd, b64, (size_t)l + 1) != l + 1) { close(fd); return 1; }
    close(fd);
    memset(priv, 0, sizeof(priv));
    snprintf(path, sizeof(path), "%s.pub", base);
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "xpkg-create: cannot write %s\n", path); return 1; }
    l = b64_out(pub, 32, b64);
    b64[l] = '\0';
    fprintf(f, "%s\n", b64);
    fclose(f);
    printf("xpkg-create: wrote %s.key (keep secret) and %s.pub (install into /etc/xpkg/keys)\n", base, base);
    return 0;
}

static int cmd_sign(int argc, char **argv) {
    const char *keyf = NULL, *file = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--key") && i + 1 < argc) keyf = argv[++i];
        else file = argv[i];
    }
    if (!keyf || !file) { fprintf(stderr, "usage: xpkg-create sign --key NAME.key FILE\n"); return 2; }
    FILE *kf = fopen(keyf, "r");
    char kb[128] = {0};
    if (!kf || !fgets(kb, sizeof(kb), kf)) { fprintf(stderr, "xpkg-create: cannot read %s\n", keyf); if (kf) fclose(kf); return 1; }
    fclose(kf);
    size_t kl = strcspn(kb, "\r\n");
    kb[kl] = '\0';
    unsigned char raw[64];
    int rl = EVP_DecodeBlock(raw, (unsigned char *)kb, (int)kl);
    if (rl < 32) { fprintf(stderr, "xpkg-create: %s is not an Ed25519 secret key\n", keyf); return 1; }
    EVP_PKEY *pk = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, raw, 32);
    memset(raw, 0, sizeof(raw));
    if (!pk) { fprintf(stderr, "xpkg-create: bad key\n"); return 1; }

    FILE *in = fopen(file, "rb");
    if (!in) { fprintf(stderr, "xpkg-create: cannot read %s\n", file); EVP_PKEY_free(pk); return 1; }
    fseek(in, 0, SEEK_END);
    long len = ftell(in);
    fseek(in, 0, SEEK_SET);
    unsigned char *data = malloc(len > 0 ? (size_t)len : 1);
    if (!data || fread(data, 1, (size_t)len, in) != (size_t)len) { fclose(in); free(data); EVP_PKEY_free(pk); return 1; }
    fclose(in);

    unsigned char sig[64];
    size_t sl = sizeof(sig);
    EVP_MD_CTX *mctx = EVP_MD_CTX_new();
    int ok = mctx && EVP_DigestSignInit(mctx, NULL, NULL, NULL, pk) == 1 &&
             EVP_DigestSign(mctx, sig, &sl, data, (size_t)len) == 1;
    EVP_MD_CTX_free(mctx);
    EVP_PKEY_free(pk);
    free(data);
    if (!ok) { fprintf(stderr, "xpkg-create: signing failed\n"); return 1; }

    char out[PATH_MAX_LOCAL], b64[128];
    snprintf(out, sizeof(out), "%s.sig", file);
    int bl = b64_out(sig, 64, b64);
    b64[bl] = '\0';
    FILE *o = fopen(out, "w");
    if (!o) { fprintf(stderr, "xpkg-create: cannot write %s\n", out); return 1; }
    fprintf(o, "%s\n", b64);
    fclose(o);
    printf("xpkg-create: wrote %s\n", out);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: xpkg-create <create|index|keygen|sign> [options]\n");
        return 2;
    }
    if (strcmp(argv[1], "create") == 0) {
        return cmd_create(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "index") == 0) {
        return cmd_index(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "keygen") == 0) {
        return cmd_keygen(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "sign") == 0) {
        return cmd_sign(argc - 2, argv + 2);
    }
    fprintf(stderr, "xpkg-create: unknown subcommand: %s\n", argv[1]);
    return 2;
}
