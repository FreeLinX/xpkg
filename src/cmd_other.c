/* cmd_other.c - remove, autoremove, list, info, files, owns, verify, clean. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include "xpkg.h"

/* --- remove ---------------------------------------------------------------- */

typedef struct { char **skip; int nskip; char list[1024]; int n; } rdep_ctx_t;

static int rdep_cb(const char *pkg, const char *unused, void *user) {
    (void)unused;
    rdep_ctx_t *c = user;
    for (int i = 0; i < c->nskip; i++) if (!strcmp(c->skip[i], pkg)) return 0;
    size_t l = strlen(c->list);
    snprintf(c->list + l, sizeof(c->list) - l, "%s%s", c->n ? ", " : "", pkg);
    c->n++;
    return 0;
}

int xpkg_cmd_remove(char **names, int n) {
    if (xpkg_require_root() != 0 || xpkg_lock() != 0) return 1;
    if (xpkg_db_open() != XPKG_OK) return 1;
    for (int i = 0; i < n; i++) {
        if (!xpkg_db_is_installed(names[i])) { xpkg_err("%s is not installed", names[i]); return 1; }
        if (!strcmp(names[i], "xpkg") && !xpkg_opts.force) {
            xpkg_err("refusing to remove xpkg itself (--force overrides)");
            return 1;
        }
        rdep_ctx_t c = { names, n, "", 0 };
        xpkg_db_each_rdepend(names[i], rdep_cb, &c);
        if (c.n && !xpkg_opts.force) {
            xpkg_err("%s is needed by: %s (remove those too, or use --force)", names[i], c.list);
            return 1;
        }
    }
    int rc = 0;
    for (int i = 0; i < n; i++)
        if (xpkg_remove_package(names[i]) != XPKG_OK) rc = 1;
    return rc;
}

/* --- autoremove: dependencies nothing needs any more ---------------------- */

typedef struct { char names[4096][XPKG_MAX_NAME]; int n; } orphans_t;

static int any_rdep(const char *pkg, const char *unused, void *user) {
    (void)pkg; (void)unused;
    *(int *)user = 1;
    return 1;
}

static int orphan_cb(const xpkg_pkg_row_t *row, void *user) {
    orphans_t *o = user;
    if (row->explicit_ || o->n >= 4096) return 0;
    int needed = 0;
    xpkg_db_each_rdepend(row->name, any_rdep, &needed);
    if (!needed) snprintf(o->names[o->n++], XPKG_MAX_NAME, "%s", row->name);
    return 0;
}

int xpkg_cmd_autoremove(void) {
    if (xpkg_require_root() != 0 || xpkg_lock() != 0) return 1;
    if (xpkg_db_open() != XPKG_OK) return 1;
    orphans_t *o = malloc(sizeof(*o));
    if (!o) return 1;
    int total = 0, rc = 0;
    for (;;) {   /* removing one orphan can orphan its own dependencies */
        o->n = 0;
        xpkg_db_each_package(orphan_cb, o);
        if (!o->n) break;
        for (int i = 0; i < o->n; i++) {
            if (xpkg_remove_package(o->names[i]) != XPKG_OK) { rc = 1; break; }
            total++;
        }
        if (rc || xpkg_opts.dry_run) break;
    }
    free(o);
    if (!total && !xpkg_opts.quiet) printf("no unneeded packages\n");
    return rc;
}

/* --- queries ---------------------------------------------------------------- */

typedef struct { int explicit_only; int n; } list_ctx_t;

static int list_cb(const xpkg_pkg_row_t *row, void *user) {
    list_ctx_t *c = user;
    if (c->explicit_only && !row->explicit_) return 0;
    printf("%s %s\n", row->name, row->version);
    c->n++;
    return 0;
}

int xpkg_cmd_list(int explicit_only) {
    if (xpkg_db_open() != XPKG_OK) return 1;
    list_ctx_t c = { explicit_only, 0 };
    xpkg_db_each_package(list_cb, &c);
    return 0;
}

static int print_word(const char *s, const char *unused, void *user) {
    (void)unused;
    int *n = user;
    printf("%s%s", (*n)++ ? " " : "", s);
    return 0;
}

int xpkg_cmd_info(const char *name) {
    if (xpkg_db_open() != XPKG_OK) return 1;
    xpkg_pkg_row_t row;
    if (!xpkg_db_get(name, &row)) {
        xpkg_err("%s is not installed (try: xpkg show %s)", name, name);
        return 1;
    }
    char when[64] = "?";
    time_t t = (time_t)row.installed_at;
    struct tm tm;
    if (localtime_r(&t, &tm)) strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
    printf("Name        : %s\n", row.name);
    printf("Version     : %s\n", row.version);
    printf("Description : %s\n", row.description[0] ? row.description : "-");
    printf("Installed   : %s (%s)\n", when, row.explicit_ ? "explicitly" : "as a dependency");
    printf("Files       : %d\n", xpkg_db_count_files(name));
    int n = 0;
    printf("Depends on  : ");
    xpkg_db_each_depend(name, print_word, &n);
    printf("%s\nRequired by : ", n ? "" : "-");
    n = 0;
    xpkg_db_each_rdepend(name, print_word, &n);
    printf("%s\n", n ? "" : "-");
    return 0;
}

static int print_path(const char *path, const char *sha, void *user) {
    (void)sha;
    (*(int *)user)++;
    printf("%s\n", path);
    return 0;
}

int xpkg_cmd_files(const char *name) {
    if (xpkg_db_open() != XPKG_OK) return 1;
    if (!xpkg_db_is_installed(name)) { xpkg_err("%s is not installed", name); return 1; }
    int n = 0;
    xpkg_db_each_file(name, print_path, &n);
    return 0;
}

int xpkg_cmd_owns(const char *path) {
    if (xpkg_db_open() != XPKG_OK) return 1;
    char abs[XPKG_MAX_PATH], owner[XPKG_MAX_NAME];
    if (path[0] == '/') snprintf(abs, sizeof(abs), "%s", path);
    else {
        char cwd[XPKG_MAX_PATH];
        if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
        snprintf(abs, sizeof(abs), "%s/%s", cwd, path);
    }
    if (xpkg_db_file_owner(abs, owner, sizeof(owner))) {
        printf("%s is owned by %s\n", abs, owner);
        return 0;
    }
    /* try the resolved path too (/bin/x vs /usr/bin/x through symlinks) */
    char real[PATH_MAX];
    if (realpath(abs, real) && strcmp(real, abs) && xpkg_db_file_owner(real, owner, sizeof(owner))) {
        printf("%s is owned by %s\n", real, owner);
        return 0;
    }
    fprintf(stderr, "no package owns %s\n", abs);
    return 1;
}

/* --- verify --------------------------------------------------------------- */

typedef struct { const char *pkg; int bad, total; } verify_ctx_t;

static int verify_file(const char *path, const char *sha, void *user) {
    verify_ctx_t *c = user;
    char full[XPKG_MAX_PATH], actual[65];
    snprintf(full, sizeof(full), "%s%s", xpkg_root(), path);
    c->total++;
    struct stat ls;
    if (lstat(full, &ls) != 0) {
        printf("%s: MISSING  %s\n", c->pkg, path);
        c->bad++;
        return 0;
    }
    int ok;
    if (S_ISLNK(ls.st_mode)) {
        char t[XPKG_MAX_PATH];
        ssize_t l = readlink(full, t, sizeof(t) - 1);
        ok = l >= 0 && (t[l] = '\0', xpkg_sha256_str(t, actual) == XPKG_OK);
    } else {
        ok = xpkg_sha256_file(full, actual) == XPKG_OK;
    }
    if (!ok || strcmp(actual, sha)) {
        printf("%s: %s %s\n", c->pkg, !strncmp(path, "/etc/", 5) ? "EDITED  " : "MODIFIED", path);
        if (strncmp(path, "/etc/", 5)) c->bad++;
    } else if (xpkg_opts.verbose) {
        printf("%s: OK       %s\n", c->pkg, path);
    }
    return 0;
}

typedef struct { int bad, pkgs, files; } verify_all_t;

static int verify_pkg(const xpkg_pkg_row_t *row, void *user) {
    verify_all_t *a = user;
    verify_ctx_t c = { row->name, 0, 0 };
    xpkg_db_each_file(row->name, verify_file, &c);
    a->bad += c.bad;
    a->files += c.total;
    a->pkgs++;
    return 0;
}

int xpkg_cmd_verify(char **names, int n) {
    if (xpkg_db_open() != XPKG_OK) return 1;
    verify_all_t a = { 0, 0, 0 };
    if (n == 0) {
        xpkg_db_each_package(verify_pkg, &a);
    } else {
        for (int i = 0; i < n; i++) {
            xpkg_pkg_row_t row;
            if (!xpkg_db_get(names[i], &row)) { xpkg_err("%s is not installed", names[i]); return 1; }
            verify_pkg(&row, &a);
        }
    }
    printf("%d package(s), %d file(s) checked: %s\n", a.pkgs, a.files,
           a.bad ? "PROBLEMS FOUND" : "all intact");
    return a.bad ? 1 : 0;
}

/* --- clean ---------------------------------------------------------------- */

int xpkg_cmd_clean(void) {
    if (xpkg_require_root() != 0 || xpkg_lock() != 0) return 1;
    char dir[XPKG_MAX_PATH];
    snprintf(dir, sizeof(dir), "%s/pkg", xpkg_cache_dir());
    if (xpkg_opts.dry_run) { printf("would remove %s\n", dir); return 0; }
    xpkg_rm_tree(dir);
    printf("removed downloaded packages from %s\n", xpkg_cache_dir());
    return 0;
}
