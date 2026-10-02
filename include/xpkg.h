/* xpkg.h - shared definitions for the FreeLinX package manager.
 *
 * Package format: a .xpkg is a gzip'd ustar archive holding
 *   pkg-info        NAME/VERSION/DESCRIPTION/ARCH/DEPENDS (key=value lines)
 *   post-install    optional sh script, run after install and upgrade
 *   pre-remove      optional sh script, run before remove
 *   files/...       the tree installed relative to /
 * Entries are regular files ('0'), directories ('5') and symlinks ('2');
 * pax extended headers ('x') carry paths and link targets over 100 bytes.
 *
 * Repositories serve index.json (+ index.json.sig, an Ed25519 signature of
 * the exact index bytes) and the archives.  Installed state lives in SQLite
 * at /var/lib/xpkg/xpkg.db.  No external tar/gzip/curl is ever run.
 *
 * Every directory can be redirected with XPKG_ROOT (install into a mounted
 * target, e.g. the installer) or individually with XPKG_CONFIG_DIR /
 * XPKG_DB_DIR / XPKG_CACHE_DIR (host-side tests). */
#ifndef XPKG_H
#define XPKG_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define XPKG_VERSION        "1.0.0"

#define XPKG_DB_DIR_DEFAULT     "/var/lib/xpkg"
#define XPKG_CACHE_DIR_DEFAULT  "/var/cache/xpkg"
#define XPKG_CONFIG_DIR_DEFAULT "/etc/xpkg"

#define XPKG_MAX_NAME        128
#define XPKG_MAX_VERSION     64
#define XPKG_MAX_LINE        1024
#define XPKG_MAX_PATH        4096
#define XPKG_MAX_URL         8192
#define XPKG_MAX_DEPENDS     64

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
    XPKG_ERR_CYCLE,
    XPKG_ERR_CONFLICT,
    XPKG_ERR_SIGNATURE
} xpkg_status_t;

/* --- global options (main.c) ------------------------------------------- */
typedef struct {
    int force;           /* ignore file conflicts / reverse dependencies */
    int dry_run;         /* print the plan, change nothing */
    int quiet;           /* no per-file output, no progress bar */
    int verbose;         /* list every installed file */
    int allow_unsigned;  /* accept repos without a valid signature */
    int no_scripts;      /* do not run post-install / pre-remove */
} xpkg_opts_t;
extern xpkg_opts_t xpkg_opts;

/* --- util.c ------------------------------------------------------------- */
void xpkg_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void xpkg_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void xpkg_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  xpkg_mkdir_p(const char *path, unsigned mode);
void xpkg_rm_tree(const char *dir);
void xpkg_rmdir_parents(const char *path, const char *stop);
char *xpkg_read_file(const char *path, size_t *len);
int  xpkg_write_file_atomic(const char *path, const void *data, size_t len);
void xpkg_human_size(unsigned long long n, char *out, size_t outsz);
int  xpkg_is_tty(void);
const char *xpkg_color(const char *code); /* "" when not a tty */
int  xpkg_valid_name(const char *s);

/* --- paths.c ------------------------------------------------------------ */
const char *xpkg_root(void);          /* "" on a live system */
const char *xpkg_db_dir(void);
const char *xpkg_db_path(void);
const char *xpkg_cache_dir(void);
const char *xpkg_config_dir(void);
const char *xpkg_repos_conf(void);
const char *xpkg_keys_dir(void);
const char *xpkg_scripts_dir(void);

/* --- lock.c ------------------------------------------------------------- */
int  xpkg_lock(void);                 /* exclusive; waits for other xpkg */
void xpkg_unlock(void);
int  xpkg_require_root(void);         /* 0 if we may modify the system */

/* --- version.c ---------------------------------------------------------- */
int xpkg_version_cmp(const char *a, const char *b);

/* --- json.c ------------------------------------------------------------- */
typedef enum { JSON_NULL, JSON_BOOL, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ } json_type_t;
typedef struct json json_t;
struct json {
    json_type_t type;
    double num;
    int boolean;
    char *str;          /* JSON_STR */
    char *key;          /* member name when inside an object */
    json_t *child;      /* first element / member */
    json_t *next;       /* next sibling */
};
json_t *json_parse(const char *text, char *err, size_t errsz);
void json_free(json_t *j);
json_t *json_get(const json_t *obj, const char *key);
const char *json_str(const json_t *obj, const char *key, const char *def);
double json_num(const json_t *obj, const char *key, double def);
void json_write_str(FILE *f, const char *s);

/* --- pkginfo.c ---------------------------------------------------------- */
typedef struct {
    char name[XPKG_MAX_NAME];
    char version[XPKG_MAX_VERSION];
    char description[XPKG_MAX_LINE];
    char arch[XPKG_MAX_NAME];
    char depends[XPKG_MAX_DEPENDS][XPKG_MAX_NAME];
    int  depends_count;
} xpkg_info_t;
xpkg_status_t xpkg_parse_pkginfo(const char *path, xpkg_info_t *out);
xpkg_status_t xpkg_parse_pkginfo_data(const char *data, xpkg_info_t *out);
xpkg_status_t xpkg_pkginfo_from_archive(const char *archive_path, xpkg_info_t *out);
/* "libfoo>=1.2" -> "libfoo" */
void xpkg_dep_name(const char *dep, char *out, size_t outsz);

/* --- hash.c ------------------------------------------------------------- */
xpkg_status_t xpkg_sha256_file(const char *path, char out[65]);
xpkg_status_t xpkg_sha256_str(const char *s, char out[65]);

/* --- sign.c ------------------------------------------------------------- */
/* Verifies an Ed25519 signature (hex or base64 text in sig) of data with any
 * key in xpkg_keys_dir().  Returns XPKG_OK, XPKG_ERR_SIGNATURE, or
 * XPKG_ERR_NOT_FOUND when no keys are installed. */
xpkg_status_t xpkg_verify_signature(const void *data, size_t len, const char *sig, char *keyname, size_t keynamesz);
int xpkg_have_keys(void);

/* --- tar.c -------------------------------------------------------------- */
xpkg_status_t xpkg_tar_extract(const char *archive_path, const char *dest_dir);

/* --- net.c -------------------------------------------------------------- */
/* GET url into dest_path (via dest_path.part).  label, when non-NULL, turns
 * on a progress bar on a terminal. */
/* max: the most bytes the body may have; a larger one is refused (a mirror
 * cannot fill the disk before the checksum is even looked at) */
xpkg_status_t xpkg_net_get(const char *url, const char *dest_path, const char *label,
                           unsigned long long max);
extern int xpkg_net_quiet_404;   /* a missing file is expected (index.json.sig) */

/* --- db.c --------------------------------------------------------------- */
typedef struct {
    char name[XPKG_MAX_NAME];
    char version[XPKG_MAX_VERSION];
    char description[XPKG_MAX_LINE];
    long long installed_at;
    int explicit_;
} xpkg_pkg_row_t;

xpkg_status_t xpkg_db_open(void);
void xpkg_db_close(void);
xpkg_status_t xpkg_db_begin(void);
xpkg_status_t xpkg_db_commit(void);
void xpkg_db_rollback(void);
int  xpkg_db_get(const char *name, xpkg_pkg_row_t *out);         /* 1 found */
int  xpkg_db_is_installed(const char *name);
xpkg_status_t xpkg_db_put_package(const xpkg_info_t *info, int explicit_);
xpkg_status_t xpkg_db_set_explicit(const char *name, int explicit_);
xpkg_status_t xpkg_db_delete_package(const char *name);
xpkg_status_t xpkg_db_clear_files(const char *name);
xpkg_status_t xpkg_db_add_file(const char *name, const char *path, const char *sha256);
int  xpkg_db_file_owner(const char *path, char *owner, size_t ownersz); /* 1 owned */
int  xpkg_db_file_sha(const char *name, const char *path, char out[65]); /* 1 found */
/* Callbacks return 0 to continue, non-zero to stop. */
typedef int (*xpkg_row_cb)(const xpkg_pkg_row_t *row, void *user);
typedef int (*xpkg_str_cb)(const char *s, const char *s2, void *user);
xpkg_status_t xpkg_db_each_package(xpkg_row_cb cb, void *user);
xpkg_status_t xpkg_db_each_file(const char *name, xpkg_str_cb cb, void *user);   /* path, sha */
xpkg_status_t xpkg_db_each_depend(const char *name, xpkg_str_cb cb, void *user); /* dep, NULL */
xpkg_status_t xpkg_db_each_rdepend(const char *name, xpkg_str_cb cb, void *user);/* pkg, NULL */
int  xpkg_db_count_files(const char *name);

/* --- repo.c: indexes ---------------------------------------------------- */
typedef struct {
    char name[XPKG_MAX_NAME];
    char version[XPKG_MAX_VERSION];
    char *description;
    char arch[32];
    char file[XPKG_MAX_PATH];
    unsigned long long size;
    char sha[65];
    char depends[XPKG_MAX_DEPENDS][XPKG_MAX_NAME];
    int  depends_count;
    int  has_depends;       /* index carries a depends list */
    int  repo;              /* index into the repo table */
} xpkg_entry_t;

int  xpkg_repos_count(void);
const char *xpkg_repo_url(int i);
/* Loads every configured repo's cached index (fetching when missing). */
xpkg_status_t xpkg_index_load(int fetch_missing);
const xpkg_entry_t *xpkg_index_find(const char *name);
int  xpkg_index_count(void);
const xpkg_entry_t *xpkg_index_at(int i);
/* Downloads (or reuses a verified cached copy of) an entry's archive. */
xpkg_status_t xpkg_fetch_entry(const xpkg_entry_t *e, char *out_path, size_t n);

/* --- install.c ---------------------------------------------------------- */
/* Deploys one archive: install or upgrade/reinstall of the same name. */
xpkg_status_t xpkg_deploy_archive(const char *archive, int explicit_, int allow_same);
xpkg_status_t xpkg_remove_package(const char *name);

/* --- commands ----------------------------------------------------------- */
int xpkg_cmd_install(char **names, int n, int reinstall);
int xpkg_cmd_remove(char **names, int n);
int xpkg_cmd_autoremove(void);
int xpkg_cmd_update(void);
int xpkg_cmd_upgrade(char **names, int n);
int xpkg_cmd_outdated(void);
int xpkg_cmd_search(const char *term);
int xpkg_cmd_query(void);
int xpkg_cmd_show(const char *name);
int xpkg_cmd_list(int explicit_only);
int xpkg_cmd_info(const char *name);
int xpkg_cmd_files(const char *name);
int xpkg_cmd_owns(const char *path);
int xpkg_cmd_verify(char **names, int n);
int xpkg_cmd_clean(void);
int xpkg_cmd_repo_add(const char *url);
int xpkg_cmd_repo_remove(const char *url);
int xpkg_cmd_repo_list(void);

#endif /* XPKG_H */
