/* xpkg.h - shared definitions for the FreeLinX package manager.
 *
 * Design (agreed 2026-09-01):
 *   - .xpkg files are gzip'd ustar archives containing a top-level
 *     "pkg-info" metadata file plus a "files/" tree extracted relative to /.
 *   - Installed-package state lives in a SQLite database at
 *     /var/lib/xpkg/xpkg.db (packages + files tables).
 *   - Local-file install (`xpkg install <file.xpkg>`) plus network/repo
 *     fetch (`xpkg install <name>`), never external `tar`/`curl`/`wget`.
 *   - No external `tar` dependency: xpkg does its own minimal ustar
 *     parsing (see tar.c) to avoid a bootstrap chicken-and-egg problem.
 *
 * Paths default to /etc/xpkg, /var/lib/xpkg, /var/cache/xpkg but can be
 * overridden at runtime via the XPKG_CONFIG_DIR / XPKG_DB_DIR /
 * XPKG_CACHE_DIR environment variables (used by the host-side headless
 * tests, which run as a non-root user against a tmpdir). On a FreeLinX
 * device nothing sets those, so the defaults apply. */
#ifndef XPKG_H
#define XPKG_H

#include <stddef.h>
#include <stdint.h>

#define XPKG_VERSION        "0.1.0"

#define XPKG_DB_PATH_DEFAULT    "/var/lib/xpkg/xpkg.db"
#define XPKG_DB_DIR_DEFAULT     "/var/lib/xpkg"
#define XPKG_CACHE_DIR_DEFAULT  "/var/cache/xpkg"
#define XPKG_CONFIG_DIR_DEFAULT "/etc/xpkg"

#define XPKG_MAX_NAME        128
#define XPKG_MAX_VERSION     64
#define XPKG_MAX_LINE        1024
#define XPKG_MAX_PATH        4096
#define XPKG_MAX_DEPENDS     32

/* --- paths.c -------------------------------------------------------------
 * Resolved, possibly env-overridden filesystem locations. All other
 * modules use these functions instead of the _DEFAULT macros directly. */
const char *xpkg_db_dir(void);
const char *xpkg_db_path(void);
const char *xpkg_cache_dir(void);
const char *xpkg_config_dir(void);
const char *xpkg_repos_conf(void);
/* Install-time destination root ("" on a real FreeLinX device; XPKG_ROOT
 * redirects the host-side headless tests out of /). */
const char *xpkg_root(void);

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
 * (bad archive, missing pkg-info, dependency cycle) rather than raw OS
 * errors. */
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
    XPKG_ERR_VERIFY_FAILED,
    XPKG_ERR_CYCLE
} xpkg_status_t;

/* --- version.c ---------------------------------------------------------- */
/* Compares two version strings ("1.2", "2.10", "1.2.9-nb1"). Negaitve if
 * a < b, zero if equal, positive if a > b. Numeric dot/-/_ segments are
 * compared numerically (so 1.10 > 1.9). */
int xpkg_version_cmp(const char *a, const char *b);

/* --- pkginfo.c ---------------------------------------------------------- */
/* Parses a pkg-info file already extracted to a path on disk into an
 * xpkg_info_t. Returns XPKG_OK or XPKG_ERR_BAD_PKGINFO. */
xpkg_status_t xpkg_parse_pkginfo(const char *path, xpkg_info_t *out);

/* Reads just the pkg-info entry from inside a .xpkg archive (gzip'd
 * ustar) without extracting the files, so the dependency resolver can
 * learn a package's DEPENDS before installing anything. */
xpkg_status_t xpkg_pkginfo_from_archive(const char *archive_path, xpkg_info_t *out);

/* --- db.c ----------------------------------------------------------------
 * All functions open/close their own connection to xpkg_db_path()
 * internally (SQLite connections are cheap; this keeps callers simple and
 * avoids a global connection object threading through every command). */
xpkg_status_t xpkg_db_init(void);
xpkg_status_t xpkg_db_register_package(const xpkg_info_t *info);
xpkg_status_t xpkg_db_register_file(const char *pkg_name, const char *path, const char *sha256);
xpkg_status_t xpkg_db_is_installed(const char *pkg_name, int *out_installed);
xpkg_status_t xpkg_db_get_version(const char *pkg_name, char *out, size_t outsz);
xpkg_status_t xpkg_db_clear_files(const char *pkg_name);
xpkg_status_t xpkg_db_set_version(const char *pkg_name, const char *version);
xpkg_status_t xpkg_db_remove_package(const char *pkg_name);
xpkg_status_t xpkg_db_list(void); /* prints directly, matching xpkg's other list/info commands */
xpkg_status_t xpkg_db_info(const char *pkg_name);
xpkg_status_t xpkg_db_files(const char *pkg_name);

/* Calls `it(name, user)` for every installed package (in name order).
 * Stops early if the callback returns non-zero. */
typedef int (*xpkg_db_pkg_iterator)(const char *name, void *user);
xpkg_status_t xpkg_db_foreach(xpkg_db_pkg_iterator it, void *user);

/* --- hash.c -------------------------------------------------------------
 * Writes a 65-byte (64 hex chars + NUL) lowercase SHA256 hex digest of the
 * file at `path` into `out`. */
xpkg_status_t xpkg_sha256_file(const char *path, char out[65]);

/* --- tar.c --------------------------------------------------------------
 * Minimal ustar reader. Extracts a gzip'd tar archive (already
 * decompressed in memory or via a FILE*, see tar.c) to a destination
 * directory. No symlink/hardlink support in v1 -- regular files and
 * directories only. */
xpkg_status_t xpkg_tar_extract(const char *archive_path, const char *dest_dir);

/* --- net.c --------------------------------------------------------------
 * Performs an HTTP/HTTPS GET of `url`, writing the response body to
 * `dest_path`. Returns XPKG_OK on success (HTTP 200). No external tools. */
xpkg_status_t xpkg_net_get(const char *url, const char *dest_path);

/* --- cmd_*.c -------------------------------------------------------------- */
int xpkg_cmd_install(const char *file_path);
/* install from file, but treat an already-installed package as a no-op
 * instead of an error (used by the dependency resolver). */
int xpkg_cmd_install_ex(const char *file_path, int skip_if_installed);
int xpkg_cmd_upgrade_archive(const char *file_path);
int xpkg_cmd_remove(const char *name);
int xpkg_cmd_list(void);
int xpkg_cmd_info(const char *name);
int xpkg_cmd_files(const char *name);
int xpkg_cmd_verify(const char *name);

/* --- repo.c --------------------------------------------------------------
 * `xpkg install <name>` resolves <name> (and its DEPENDS) across the
 * repos in xpkg_repos_conf(): download, verify, and install in dependency
 * order. `xpkg update` refreshes the cached per-repo index used by later
 * installs. */
int xpkg_cmd_update(void);
int xpkg_cmd_install_repo(const char *name);
int xpkg_cmd_upgrade(const char *name);
int xpkg_cmd_upgrade_all(void);
int xpkg_cmd_repo_add(const char *url);
int xpkg_cmd_repo_remove(const char *url);
int xpkg_cmd_repo_list(void);

#endif /* XPKG_H */