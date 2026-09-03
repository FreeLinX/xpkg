/* cmd_install.c - `xpkg install <file.xpkg>` (v1: local file only).
 *
 * Steps, matching the design agreed 2026-09-01:
 *   1. Extract the .xpkg archive to a scratch directory.
 *   2. Parse its pkg-info.
 *   3. Refuse if already installed (no upgrade-in-place logic yet -- that
 *      is a deliberate v1 limitation, not an oversight: `xpkg remove` then
 *      `xpkg install` is the v1 upgrade path).
 *   4. Check every DEPENDS entry is already installed; refuse with a
 *      clear message naming the missing dependency if not (no
 *      auto-fetching yet -- that needs the network/repo work, phase 2).
 *   5. Copy files/ into place under /, hashing each file as it's placed.
 *   6. Register the package and its files in the database.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "xpkg.h"

#define SCRATCH_DIR "/var/cache/xpkg/install-scratch"

/* Recursively delete a directory and its contents (used to clear the
 * scratch dir between installs). Minimal, no external tools. */
static void rm_tree(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char p[XPKG_MAX_PATH];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
            rm_tree(p);
        }
        unlink(p);
    }
    closedir(d);
    rmdir(dir);
}


/* Recursively copies the src_root files tree into / (dest_root), recording each
 * regular file's path (relative to /) and hash via the callback-style
 * registration calls. Directories are created as needed but not
 * separately recorded in the files table -- only files are tracked for
 * ownership/removal purposes, matching the schema design. */
static xpkg_status_t copy_tree(const char *src_dir, const char *rel_prefix, const char *pkg_name) {
    DIR *d = opendir(src_dir);
    if (!d) {
        return XPKG_ERR_IO;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char src_path[XPKG_MAX_PATH];
        char rel_path[XPKG_MAX_PATH];
        snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, entry->d_name);
        snprintf(rel_path, sizeof(rel_path), "%s/%s", rel_prefix, entry->d_name);

        struct stat st;
        if (stat(src_path, &st) != 0) {
            continue;
        }

        char dest_path[XPKG_MAX_PATH];
        snprintf(dest_path, sizeof(dest_path), "%s", rel_path); /* rel_path is already "/"-rooted */

        if (S_ISDIR(st.st_mode)) {
            mkdir(dest_path, 0755);
            xpkg_status_t st2 = copy_tree(src_path, rel_path, pkg_name);
            if (st2 != XPKG_OK) {
                closedir(d);
                return st2;
            }
        } else if (S_ISREG(st.st_mode)) {
            FILE *in = fopen(src_path, "rb");
            if (!in) { closedir(d); return XPKG_ERR_IO; }
            FILE *out = fopen(dest_path, "wb");
            if (!out) { fclose(in); closedir(d); return XPKG_ERR_IO; }

            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
                fwrite(buf, 1, n, out);
            }
            fclose(in);
            fclose(out);
            chmod(dest_path, st.st_mode & 0777);

            char digest[65];
            if (xpkg_sha256_file(dest_path, digest) == XPKG_OK) {
                xpkg_db_register_file(pkg_name, rel_path, digest);
            }

            printf("  %s\n", rel_path);
        }
        /* symlinks etc: not present in v1 archives per tar.c's scope */
    }

    closedir(d);
    return XPKG_OK;
}

int xpkg_cmd_install(const char *file_path) {
    xpkg_db_init();

    /* Fresh scratch directory every install -- never trust leftovers from
     * a previous (possibly interrupted) run. */
    char rm_cmd_unused; (void)rm_cmd_unused; /* no shelling out to rm; do it directly below */
    mkdir("/var/cache/xpkg", 0755);
    rm_tree(SCRATCH_DIR);   /* never trust leftover state from a prior install */
    mkdir(SCRATCH_DIR, 0755);

    printf("xpkg: extracting %s\n", file_path);
    if (xpkg_tar_extract(file_path, SCRATCH_DIR) != XPKG_OK) {
        fprintf(stderr, "xpkg: failed to extract %s\n", file_path);
        return 1;
    }

    char pkginfo_path[XPKG_MAX_PATH];
    snprintf(pkginfo_path, sizeof(pkginfo_path), "%s/pkg-info", SCRATCH_DIR);

    xpkg_info_t info;
    if (xpkg_parse_pkginfo(pkginfo_path, &info) != XPKG_OK) {
        fprintf(stderr, "xpkg: %s has no valid pkg-info\n", file_path);
        return 1;
    }

    int already;
    xpkg_db_is_installed(info.name, &already);
    if (already) {
        fprintf(stderr, "xpkg: %s is already installed (remove it first to reinstall)\n", info.name);
        return 1;
    }

    for (int i = 0; i < info.depends_count; i++) {
        int dep_installed;
        xpkg_db_is_installed(info.depends[i], &dep_installed);
        if (!dep_installed) {
            fprintf(stderr, "xpkg: missing dependency: %s (required by %s)\n",
                    info.depends[i], info.name);
            return 1;
        }
    }

    printf("xpkg: installing %s %s\n", info.name, info.version);

    char files_dir[XPKG_MAX_PATH];
    snprintf(files_dir, sizeof(files_dir), "%s/files", SCRATCH_DIR);

    if (copy_tree(files_dir, "", info.name) != XPKG_OK) {
        fprintf(stderr, "xpkg: failed while copying files for %s\n", info.name);
        return 1;
    }

    if (xpkg_db_register_package(&info) != XPKG_OK) {
        fprintf(stderr, "xpkg: warning: files installed but database registration failed\n");
        return 1;
    }

    printf("xpkg: %s %s installed\n", info.name, info.version);
    return 0;
}
