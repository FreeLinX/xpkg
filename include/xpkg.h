/* xpkg.h - shared definitions for the FreeLinX package manager.
 *
 * Design (agreed 2026-09-01):
 *   - .xpkg files are gzip'd ustar archives containing a top-level
 *     "pkg-info" metadata file plus a "files/" tree extracted relative to /.
 *   - Installed-package state lives in a SQLite database at
 *     /var/lib/xpkg/xpkg.db (packages + files tables).
 *   - v1 scope: local-file install only (`xpkg install <file.xpkg>`).
 *     Network/repo-fetch (`xpkg install <name>`) is a later phase, once
 *     networking lands.
 *   - No external `tar` dependency: xpkg does its own minimal ustar
 *     parsing (see tar.c) to avoid a bootstrap chicken-and-egg problem
 *     (tar itself must never need to be an xpkg package to install tar).
 */
#ifndef XPKG_H
#define XPKG_H

#include <stddef.h>
#include <stdint.h>

#define XPKG_VERSION        "0.1.0"
#define XPKG_DB_PATH         "/var/lib/xpkg/xpkg.db"
#define XPKG_DB_DIR          "/var/lib/xpkg"
#define XPKG_CACHE_DIR       "/var/cache/xpkg"
#define XPKG_CONFIG_DIR      "/etc/xpkg"
#define XPKG_REPOS_CONF      "/etc/xpkg/repos.conf"

#define XPKG_MAX_NAME        128
#define XPKG_MAX_VERSION     64
#define XPKG_MAX_LINE        1024
#define XPKG_MAX_PATH        4096
#define XPKG_MAX_DEPENDS     32

/* Parsed contents of a package's pkg-info file. */
typedef struct {
    char name[XPKG_MAX_NAME];
    char version[XPKG_MAX_VERSION];
    char description[XPKG_MAX_LINE];
    char arch[XPKG_MAX_NAME];
    char depends[XPKG_MAX_DEPENDS][XPKG_MAX_NAME];
    int  depends_count;
} xpkg_info_t;

/* Return codes used throughout xpkg. Kept small and explicit rather than
 * reusing errno directly, since failures here are often xpkg-specific
 * (bad archive, missing pkg-info, dependency not installed) rather than
 * raw OS errors. */
typedef enum {
    XPKG_OK = 0,
    XPKG_ERR_USAGE,
    XPKG_ERR_NOT_FOUND,
    XPKG_ERR_IO,
    XPKG_ERR_BAD_ARCHIVE,
    XPKG_ERR_BAD_PKGINFO,
    XPKG_ERR_DB,
    XPKG_ERR_ALREADY_INSTALLED,
    XPKG_ERR_MISSING_DEPENDENCY,
    XPKG_ERR_VERIFY_FAILED
} xpkg_status_t;

/* --- pkginfo.c ------------------------------------------------------- */
/* Parses a pkg-info file already extracted to a path on disk into an
 * xpkg_info_t. Returns XPKG_OK or XPKG_ERR_BAD_PKGINFO. */
xpkg_status_t xpkg_parse_pkginfo(const char *path, xpkg_info_t *out);

/* --- db.c -------------------------------------------------------------
 * All functions open/close their own connection to XPKG_DB_PATH internally
 * (SQLite connections are cheap; this keeps callers simple and avoids a
 * global connection object threading through every command). */
xpkg_status_t xpkg_db_init(void);
xpkg_status_t xpkg_db_register_package(const xpkg_info_t *info);
xpkg_status_t xpkg_db_register_file(const char *pkg_name, const char *path, const char *sha256);
xpkg_status_t xpkg_db_is_installed(const char *pkg_name, int *out_installed);
xpkg_status_t xpkg_db_remove_package(const char *pkg_name);
xpkg_status_t xpkg_db_list(void); /* prints directly, matching xpkg's other list/info commands */
xpkg_status_t xpkg_db_info(const char *pkg_name);
xpkg_status_t xpkg_db_files(const char *pkg_name);

/* --- hash.c -------------------------------------------------------------
 * Writes a 65-byte (64 hex chars + NUL) lowercase SHA256 hex digest of the
 * file at `path` into `out`. */
xpkg_status_t xpkg_sha256_file(const char *path, char out[65]);

/* --- tar.c --------------------------------------------------------------
 * Minimal ustar reader. Extracts a gzip'd tar archive (already
 * decompressed in memory or via a FILE*, see tar.c) to a destination
 * directory. No symlink/hardlink support in v1 -- regular files and
 * directories only, which covers every package we've built so far
 * (netbsd-sh, runit, pfetch). Revisit if a future port genuinely needs
 * symlinks preserved. */
xpkg_status_t xpkg_tar_extract(const char *archive_path, const char *dest_dir);

/* --- cmd_*.c ------------------------------------------------------------ */
int xpkg_cmd_install(const char *file_path);
int xpkg_cmd_remove(const char *name);
int xpkg_cmd_list(void);
int xpkg_cmd_info(const char *name);
int xpkg_cmd_files(const char *name);
int xpkg_cmd_verify(const char *name);

#endif /* XPKG_H */

/* --- net.c ------------------------------------------------------------ */
/* Performs an HTTP/HTTPS GET of `url`, writing the response body to
 * `dest_path`. Returns XPKG_OK on success (HTTP 200). No external tools. */
xpkg_status_t xpkg_net_get(const char *url, const char *dest_path);

/* --- repo.c ------------------------------------------------------------ */
/* `xpkg install <name>` (v2): resolve <name> across repos.conf, download,
 * verify, and install. Returns 0 on success. */
int xpkg_cmd_install_repo(const char *name);
int xpkg_cmd_repo_add(const char *url);
int xpkg_cmd_repo_remove(const char *url);
int xpkg_cmd_repo_list(void);
