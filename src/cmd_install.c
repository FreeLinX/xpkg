/* cmd_install.c - `xpkg install` and the shared package-deploy core.
 *
 * The core deploy function `install_archive()` is used by:
 *   - `xpkg install <file.xpkg>`       (local file; refuse re-installs)
 *   - repo installs                    (via xpkg_cmd_install_ex, which lets
 *                                      already-installed deps pass through)
 *   - `xpkg upgrade <name>`            (via xpkg_cmd_upgrade_archive, which
 *                                      overwrites files, removes stale ones,
 *                                      and bumps the DB version)
 *
 * Deploy steps (matching the design agreed 2026-09-01):
 *   - extract the .xpkg to a scratch directory under the cache dir
 *   - parse its pkg-info
 *   - refuse (or skip) if already installed, per the flags
 *   - verify every DEPENDS entry is installed
 *   - copy files/ into place under /, hashing each file as it goes
 *   - register the package and its files in the database
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include "xpkg.h"

#define MAX_UPGRADE_FILES 8192

enum {
    INST_NONE           = 0,
    INST_SKIP_IF_INSTALLED = 1 << 0,
};

static void scratch_path(char *out, size_t n) {
    snprintf(out, n, "%s/install-scratch", xpkg_cache_dir());
}

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
 * registration calls. Symlink entries are recreated as symlinks and registered
 * with the digest of their target string. Directories are created as needed but
 * not separately recorded in the files table -- only files are tracked for
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
        if (lstat(src_path, &st) != 0) {
            continue;
        }

        char dest_path[XPKG_MAX_PATH];
        snprintf(dest_path, sizeof(dest_path), "%s%s", xpkg_root(), rel_path); /* rel_path is already "/"-rooted */

        if (S_ISDIR(st.st_mode)) {
            mkdir(dest_path, 0755);
            xpkg_status_t st2 = copy_tree(src_path, rel_path, pkg_name);
            if (st2 != XPKG_OK) {
                closedir(d);
                return st2;
            }
        } else if (S_ISLNK(st.st_mode)) {
            /* Symlink: recreate it, then register with the digest of its
             * target string (what `xpkg verify` re-checks via readlink). */
            char target[XPKG_MAX_PATH];
            ssize_t ln = readlink(src_path, target, sizeof(target) - 1);
            if (ln < 0) {
                closedir(d);
                return XPKG_ERR_IO;
            }
            target[ln] = '\0';
            if (symlink(target, dest_path) != 0) {
                fprintf(stderr, "xpkg: cannot create symlink %s -> %s\n",
                        dest_path, target);
                closedir(d);
                return XPKG_ERR_IO;
            }

            char digest[65];
            if (xpkg_sha256_str(target, digest) == XPKG_OK) {
                xpkg_db_register_file(pkg_name, rel_path, digest);
            }

            printf("  %s -> %s\n", rel_path, target);
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
        /* special files: not present in archives per tar.c's scope */
    }

    closedir(d);
    return XPKG_OK;
}

static xpkg_status_t install_archive(const char *file_path, int flags) {
    xpkg_db_init();

    char scratch[XPKG_MAX_PATH];
    scratch_path(scratch, sizeof(scratch));
    mkdir(xpkg_cache_dir(), 0755);
    rm_tree(scratch);        /* never trust leftover state from a prior install */
    mkdir(scratch, 0755);

    printf("xpkg: extracting %s\n", file_path);
    if (xpkg_tar_extract(file_path, scratch) != XPKG_OK) {
        fprintf(stderr, "xpkg: failed to extract %s\n", file_path);
        return XPKG_ERR_IO;
    }

    char pkginfo_path[XPKG_MAX_PATH];
    snprintf(pkginfo_path, sizeof(pkginfo_path), "%s/pkg-info", scratch);

    xpkg_info_t info;
    if (xpkg_parse_pkginfo(pkginfo_path, &info) != XPKG_OK) {
        fprintf(stderr, "xpkg: %s has no valid pkg-info\n", file_path);
        return XPKG_ERR_BAD_PKGINFO;
    }

    {
        const char *root = xpkg_root();
        if (root[0]) mkdir(root, 0755);
    }

    int already;
    xpkg_db_is_installed(info.name, &already);
    if (already) {
        if (flags & INST_SKIP_IF_INSTALLED) {
            printf("xpkg: %s %s already installed; skipping\n", info.name, info.version);
            return XPKG_OK;
        }
        fprintf(stderr, "xpkg: %s is already installed (use 'xpkg upgrade %s' to update)\n",
                info.name, info.name);
        return XPKG_ERR_ALREADY_INSTALLED;
    }

    for (int i = 0; i < info.depends_count; i++) {
        int dep_installed;
        xpkg_db_is_installed(info.depends[i], &dep_installed);
        if (!dep_installed) {
            fprintf(stderr, "xpkg: missing dependency: %s (required by %s)\n",
                    info.depends[i], info.name);
            return XPKG_ERR_MISSING_DEPENDENCY;
        }
    }

    printf("xpkg: installing %s %s\n", info.name, info.version);

    char files_dir[XPKG_MAX_PATH];
    snprintf(files_dir, sizeof(files_dir), "%s/files", scratch);

    if (copy_tree(files_dir, "", info.name) != XPKG_OK) {
        fprintf(stderr, "xpkg: failed while copying files for %s\n", info.name);
        return XPKG_ERR_IO;
    }

    if (xpkg_db_register_package(&info) != XPKG_OK) {
        fprintf(stderr, "xpkg: warning: files installed but database registration failed\n");
        return XPKG_ERR_DB;
    }

    printf("xpkg: %s %s installed\n", info.name, info.version);
    return XPKG_OK;
}

int xpkg_cmd_install(const char *file_path) {
    return install_archive(file_path, INST_NONE) == XPKG_OK ? 0 : 1;
}

int xpkg_cmd_install_ex(const char *file_path, int skip_if_installed) {
    return install_archive(file_path, skip_if_installed ? INST_SKIP_IF_INSTALLED : INST_NONE) == XPKG_OK ? 0 : 1;
}

/* Collect every file under srcdir (relative paths starting with "/") into
 * list (cap MAX_UPGRADE_FILES entries). Returns count. */
static size_t collect_files(const char *srcdir, const char *rel, char list[][XPKG_MAX_PATH], size_t cap) {
    DIR *d = opendir(srcdir);
    if (!d) return 0;

    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < cap) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char src[XPKG_MAX_PATH], full[XPKG_MAX_PATH];
        snprintf(src, sizeof(src), "%s/%s", srcdir, e->d_name);
        snprintf(full, sizeof(full), "%s/%s", rel, e->d_name);

        struct stat st;
        if (lstat(src, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            n += collect_files(src, full, list + n, cap - n);
        } else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
            strncpy(list[n], full, XPKG_MAX_PATH - 1);
            list[n][XPKG_MAX_PATH - 1] = '\0';
            n++;
        }
    }
    closedir(d);
    return n;
}

static int path_in_list(const char *path, char list[][XPKG_MAX_PATH], size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (strcmp(list[i], path) == 0) return 1;
    }
    return 0;
}

int xpkg_cmd_upgrade_archive(const char *file_path) {
    xpkg_db_init();

    char scratch[XPKG_MAX_PATH];
    scratch_path(scratch, sizeof(scratch));
    mkdir(xpkg_cache_dir(), 0755);
    rm_tree(scratch);
    mkdir(scratch, 0755);

    printf("xpkg: extracting %s\n", file_path);
    if (xpkg_tar_extract(file_path, scratch) != XPKG_OK) {
        fprintf(stderr, "xpkg: failed to extract %s\n", file_path);
        return 1;
    }

    char pkginfo_path[XPKG_MAX_PATH];
    snprintf(pkginfo_path, sizeof(pkginfo_path), "%s/pkg-info", scratch);

    xpkg_info_t info;
    if (xpkg_parse_pkginfo(pkginfo_path, &info) != XPKG_OK) {
        fprintf(stderr, "xpkg: %s has no valid pkg-info\n", file_path);
        return 1;
    }

    {
        const char *root = xpkg_root();
        if (root[0]) mkdir(root, 0755);
    }

    int installed;
    xpkg_db_is_installed(info.name, &installed);
    if (!installed) {
        fprintf(stderr, "xpkg: %s is not installed; install it first\n", info.name);
        return 1;
    }

    char oldver[XPKG_MAX_VERSION] = {0};
    xpkg_db_get_version(info.name, oldver, sizeof(oldver));
    if (xpkg_version_cmp(info.version, oldver) <= 0) {
        printf("xpkg: %s %s is already up to date (installed %s)\n",
               info.name, info.version, oldver);
        return 0;
    }

    /* Snapshot the old file set before we touch anything. Heap: 8192 paths
     * x 4096 bytes x 2 sets is 64MB, which would overflow the stack. */
    char (*old_paths)[XPKG_MAX_PATH] = malloc(MAX_UPGRADE_FILES * XPKG_MAX_PATH);
    char (*new_paths)[XPKG_MAX_PATH] = malloc(MAX_UPGRADE_FILES * XPKG_MAX_PATH);
    if (!old_paths || !new_paths) {
        fprintf(stderr, "xpkg: out of memory during upgrade\n");
        free(old_paths);
        free(new_paths);
        return 1;
    }
    size_t nold = 0;
    {
        sqlite3 *db;
        if (sqlite3_open(xpkg_db_path(), &db) != SQLITE_OK) {
            fprintf(stderr, "xpkg: cannot open database\n");
            return 1;
        }
        sqlite3_stmt *stmt;
        const char *sql = "SELECT path FROM files WHERE package_name = ? ORDER BY path;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, info.name, -1, SQLITE_STATIC);
            while (nold < MAX_UPGRADE_FILES && sqlite3_step(stmt) == SQLITE_ROW) {
                const char *p = (const char *)sqlite3_column_text(stmt, 0);
                if (p) {
                    strncpy(old_paths[nold], p, XPKG_MAX_PATH - 1);
                    old_paths[nold][XPKG_MAX_PATH - 1] = '\0';
                    nold++;
                }
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }

    printf("xpkg: upgrading %s %s -> %s\n", info.name, oldver, info.version);

    char files_dir[XPKG_MAX_PATH];
    snprintf(files_dir, sizeof(files_dir), "%s/files", scratch);

    /* New file set, for stale-file removal. */
    size_t nnew = collect_files(files_dir, "", new_paths, MAX_UPGRADE_FILES);

    /* Drop old DB rows, copy new files (overwriting), then remove stale. */
    xpkg_db_clear_files(info.name);

    if (copy_tree(files_dir, "", info.name) != XPKG_OK) {
        fprintf(stderr, "xpkg: failed while copying files for %s\n", info.name);
        free(old_paths);
        free(new_paths);
        return 1;
    }

    for (size_t i = 0; i < nold; i++) {
        if (path_in_list(old_paths[i], new_paths, nnew)) continue;
        printf("  removing %s\n", old_paths[i]);
        char full[XPKG_MAX_PATH];
        snprintf(full, sizeof(full), "%s%s", xpkg_root(), old_paths[i]);
        remove(full);
    }

    if (xpkg_db_set_version(info.name, info.version) != XPKG_OK) {
        fprintf(stderr, "xpkg: warning: version update failed for %s\n", info.name);
        free(old_paths);
        free(new_paths);
        return 1;
    }

    free(old_paths);
    free(new_paths);

    printf("xpkg: %s %s installed\n", info.name, info.version);
    return 0;
}