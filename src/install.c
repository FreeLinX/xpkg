/* install.c - deploying and removing one package.
 *
 * Deploy (install, upgrade, reinstall and downgrade are one code path):
 *   1. extract the archive into a private scratch directory
 *   2. read pkg-info; check architecture and dependencies
 *   3. check every path: a file owned by another package, or a directory
 *      where the package has a file, is a conflict (refused unless --force)
 *   4. inside one database transaction, write each file to a temporary name
 *      in its target directory and rename() it into place - a running
 *      program (xpkg itself, a shell) is never overwritten mid-flight and a
 *      reader never sees half a file
 *   5. drop files the previous version had and this one does not
 *   6. commit, then run the package's post-install script
 *
 * Configuration files (/etc) the user has edited are never overwritten: the
 * packaged version is written next to them as <file>.xpkgnew.  On removal an
 * edited config file is left in place.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include "xpkg.h"

typedef struct {
    char *path;          /* "/usr/bin/foo" */
    char type;           /* 'f' file, 'l' symlink, 'd' directory */
    mode_t mode;
    dev_t dev;
    ino_t ino;
    nlink_t nlink;
} entry_t;

typedef struct {
    entry_t *v;
    size_t n, cap;
} entries_t;

static int ent_push(entries_t *l, const char *path, char type, const struct stat *st) {
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 256;
        entry_t *nv = realloc(l->v, nc * sizeof(*nv));
        if (!nv) return -1;
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n].path = strdup(path);
    l->v[l->n].type = type;
    l->v[l->n].mode = type == 'l' ? 0777 : (st->st_mode & 07777);
    l->v[l->n].dev = st->st_dev;
    l->v[l->n].ino = st->st_ino;
    l->v[l->n].nlink = st->st_nlink;
    if (!l->v[l->n].path) return -1;
    l->n++;
    return 0;
}

static void ent_free(entries_t *l) {
    for (size_t i = 0; i < l->n; i++) free(l->v[i].path);
    free(l->v);
    memset(l, 0, sizeof(*l));
}

static int walk(const char *dir, const char *rel, entries_t *out) {
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? 0 : -1;
    struct dirent *e;
    int rc = 0;
    while (rc == 0 && (e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char src[XPKG_MAX_PATH], r[XPKG_MAX_PATH];
        snprintf(src, sizeof(src), "%s/%s", dir, e->d_name);
        snprintf(r, sizeof(r), "%s/%s", rel, e->d_name);
        struct stat st;
        if (lstat(src, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            rc = ent_push(out, r, 'd', &st);
            if (rc == 0) rc = walk(src, r, out);
        } else if (S_ISLNK(st.st_mode)) {
            rc = ent_push(out, r, 'l', &st);
        } else if (S_ISREG(st.st_mode)) {
            rc = ent_push(out, r, 'f', &st);
        }
    }
    closedir(d);
    return rc;
}

static int cmp_path(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int is_config(const char *rel) {
    return !strncmp(rel, "/etc/", 5);
}

/* Directories never removed even when a removal leaves them empty. */
static int protected_dir(const char *rel) {
    static const char *keep[] = {
        "/usr/local/bin", "/usr/local/lib", "/usr/local/share", "/usr/local/sbin",
        "/usr/share/man", "/usr/share/applications", "/usr/share/icons", "/usr/share/doc",
        "/usr/share/fonts", "/usr/share/pixmaps", "/usr/lib/pkgconfig", "/usr/include",
        "/usr/libexec", "/var/lib", "/var/cache", "/var/log", "/etc/xdg", NULL
    };
    int depth = 0;
    for (const char *p = rel; *p; p++) if (*p == '/') depth++;
    if (depth <= 2) return 1;
    for (int i = 0; keep[i]; i++) if (!strcmp(rel, keep[i])) return 1;
    return 0;
}

static void prune_dirs(const char *rel) {
    char tmp[XPKG_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s", rel);
    for (;;) {
        char *slash = strrchr(tmp, '/');
        if (!slash || slash == tmp) return;
        *slash = '\0';
        if (protected_dir(tmp)) return;
        char full[XPKG_MAX_PATH];
        snprintf(full, sizeof(full), "%s%s", xpkg_root(), tmp);
        if (rmdir(full) != 0) return;
    }
}

static int copy_file_atomic(const char *src, const char *dest, mode_t mode) {
    char tmp[XPKG_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.xpkg-tmp", dest);
    unlink(tmp);
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0) return -1;
    int out = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) { close(in); return -1; }
    char buf[131072];
    ssize_t r;
    int rc = 0;
    while ((r = read(in, buf, sizeof(buf))) > 0) {
        char *p = buf;
        while (r > 0) {
            ssize_t w = write(out, p, (size_t)r);
            if (w < 0) { if (errno == EINTR) continue; rc = -1; break; }
            p += w; r -= w;
        }
        if (rc) break;
    }
    if (r < 0) rc = -1;
    close(in);
    if (rc == 0 && fchmod(out, mode) != 0) rc = -1;
    if (close(out) != 0) rc = -1;
    if (rc == 0 && rename(tmp, dest) != 0) rc = -1;
    if (rc != 0) unlink(tmp);
    return rc;
}

static int symlink_atomic(const char *target, const char *dest) {
    char tmp[XPKG_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.xpkg-tmp", dest);
    unlink(tmp);
    if (symlink(target, tmp) != 0) return -1;
    if (rename(tmp, dest) != 0) { unlink(tmp); return -1; }
    return 0;
}

static int run_script(const char *script, const char *action, const char *oldv, const char *newv) {
    if (xpkg_opts.no_scripts || access(script, R_OK) != 0) return 0;
    const char *root = xpkg_root();
    if (root[0] && geteuid() != 0) {
        xpkg_warn("skipping %s script: needs root to chroot into %s", action, root);
        return 0;
    }
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setenv("XPKG_ACTION", action, 1);
        setenv("XPKG_OLD_VERSION", oldv ? oldv : "", 1);
        setenv("XPKG_NEW_VERSION", newv ? newv : "", 1);
        const char *path = script;
        char inner[XPKG_MAX_PATH];
        if (root[0]) {
            /* copy the script into the target so it can run after chroot */
            snprintf(inner, sizeof(inner), "%s/tmp/.xpkg-script", root);
            char dir[XPKG_MAX_PATH];
            snprintf(dir, sizeof(dir), "%s/tmp", root);
            xpkg_mkdir_p(dir, 01777);
            if (copy_file_atomic(script, inner, 0700) != 0 || chroot(root) != 0 || chdir("/") != 0)
                _exit(127);
            path = "/tmp/.xpkg-script";
        } else if (chdir("/") != 0) {
            _exit(127);
        }
        execl("/bin/sh", "sh", "-e", path, action, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (root[0]) {
        char inner[XPKG_MAX_PATH];
        snprintf(inner, sizeof(inner), "%s/tmp/.xpkg-script", root);
        unlink(inner);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        xpkg_warn("%s script exited with status %d", action,
                  WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return -1;
    }
    return 0;
}

static void script_path(const char *name, const char *kind, char *out, size_t n) {
    snprintf(out, n, "%s/%s.%s", xpkg_scripts_dir(), name, kind);
}

/* --- old file snapshot --------------------------------------------------- */

typedef struct {
    char **path;
    char (*sha)[65];
    size_t n, cap;
} oldfiles_t;

static int old_cb(const char *path, const char *sha, void *user) {
    oldfiles_t *o = user;
    if (o->n == o->cap) {
        size_t nc = o->cap ? o->cap * 2 : 256;
        char **np = realloc(o->path, nc * sizeof(*np));
        if (!np) return 1;
        o->path = np;
        char (*ns)[65] = realloc(o->sha, nc * sizeof(*ns));
        if (!ns) return 1;
        o->sha = ns;
        o->cap = nc;
    }
    o->path[o->n] = strdup(path);
    snprintf(o->sha[o->n], 65, "%s", sha ? sha : "");
    o->n++;
    return 0;
}

static void old_free(oldfiles_t *o) {
    for (size_t i = 0; i < o->n; i++) free(o->path[i]);
    free(o->path);
    free(o->sha);
}

static const char *old_sha(const oldfiles_t *o, const char *path) {
    for (size_t i = 0; i < o->n; i++)
        if (!strcmp(o->path[i], path)) return o->sha[i];
    return NULL;
}

/* sha of what is on disk now (file content or symlink target) */
static int disk_sha(const char *full, char out[65]) {
    struct stat st;
    if (lstat(full, &st) != 0) return -1;
    if (S_ISLNK(st.st_mode)) {
        char t[XPKG_MAX_PATH];
        ssize_t l = readlink(full, t, sizeof(t) - 1);
        if (l < 0) return -1;
        t[l] = '\0';
        return xpkg_sha256_str(t, out) == XPKG_OK ? 0 : -1;
    }
    if (!S_ISREG(st.st_mode)) return -1;
    return xpkg_sha256_file(full, out) == XPKG_OK ? 0 : -1;
}

/* --- deploy -------------------------------------------------------------- */

xpkg_status_t xpkg_deploy_archive(const char *archive, int explicit_, int allow_same) {
    if (xpkg_db_open() != XPKG_OK) return XPKG_ERR_DB;

    char scratch[XPKG_MAX_PATH];
    snprintf(scratch, sizeof(scratch), "%s/scratch-%d", xpkg_cache_dir(), (int)getpid());
    xpkg_rm_tree(scratch);
    if (xpkg_mkdir_p(scratch, 0700) != 0) {
        xpkg_err("cannot create %s: %s", scratch, strerror(errno));
        return XPKG_ERR_IO;
    }

    xpkg_status_t st = xpkg_tar_extract(archive, scratch);
    entries_t ents = { 0 };
    oldfiles_t old = { 0 };
    char **newpaths = NULL;
    int in_txn = 0;
    xpkg_info_t info;
    if (st != XPKG_OK) goto out;

    char p[XPKG_MAX_PATH];
    snprintf(p, sizeof(p), "%s/pkg-info", scratch);
    if (xpkg_parse_pkginfo(p, &info) != XPKG_OK) {
        xpkg_err("%s: missing or invalid pkg-info", archive);
        st = XPKG_ERR_BAD_PKGINFO;
        goto out;
    }
    if (info.arch[0] && strcmp(info.arch, "x86_64") && strcmp(info.arch, "any") && strcmp(info.arch, "noarch")) {
        xpkg_err("%s is built for %s, not x86_64", info.name, info.arch);
        st = XPKG_ERR_BAD_ARCHIVE;
        goto out;
    }

    xpkg_pkg_row_t row;
    int installed = xpkg_db_get(info.name, &row);
    const char *action = "install";
    if (installed) {
        int c = xpkg_version_cmp(info.version, row.version);
        if (c == 0 && !allow_same) {
            if (!xpkg_opts.quiet) printf("  %s %s is already installed\n", info.name, info.version);
            if (explicit_ && !row.explicit_) xpkg_db_set_explicit(info.name, 1);
            goto out;
        }
        if (c < 0 && !allow_same && !xpkg_opts.force) {
            xpkg_err("%s %s is older than the installed %s (use --force to downgrade)",
                     info.name, info.version, row.version);
            st = XPKG_ERR_ALREADY_INSTALLED;
            goto out;
        }
        action = c == 0 ? "reinstall" : "upgrade";
    }

    for (int i = 0; i < info.depends_count; i++) {
        char dep[XPKG_MAX_NAME];
        xpkg_dep_name(info.depends[i], dep, sizeof(dep));
        if (!xpkg_db_is_installed(dep) && !xpkg_opts.force) {
            xpkg_err("%s needs %s, which is not installed", info.name, dep);
            st = XPKG_ERR_MISSING_DEPENDENCY;
            goto out;
        }
    }

    char files[XPKG_MAX_PATH];
    snprintf(files, sizeof(files), "%s/files", scratch);
    if (walk(files, "", &ents) != 0) { st = XPKG_ERR_IO; goto out; }

    /* conflicts */
    int conflicts = 0;
    for (size_t i = 0; i < ents.n; i++) {
        entry_t *e = &ents.v[i];
        char full[XPKG_MAX_PATH], owner[XPKG_MAX_NAME];
        snprintf(full, sizeof(full), "%s%s", xpkg_root(), e->path);
        struct stat ls, fs;
        int lx = lstat(full, &ls) == 0;
        if (e->type == 'd') {
            if (lx && !S_ISDIR(ls.st_mode) && !(S_ISLNK(ls.st_mode) && stat(full, &fs) == 0 && S_ISDIR(fs.st_mode))) {
                xpkg_err("%s: %s exists and is not a directory", info.name, e->path);
                conflicts++;
            }
            continue;
        }
        if (lx && S_ISDIR(ls.st_mode)) {
            xpkg_err("%s: %s is a directory on this system", info.name, e->path);
            conflicts++;
            continue;
        }
        if (xpkg_db_file_owner(e->path, owner, sizeof(owner)) && strcmp(owner, info.name)) {
            if (xpkg_opts.force) xpkg_warn("%s: taking over %s from %s", info.name, e->path, owner);
            else {
                xpkg_err("%s: %s is owned by %s", info.name, e->path, owner);
                conflicts++;
            }
        }
    }
    if (conflicts && !xpkg_opts.force) {
        xpkg_err("%s: %d conflicting path(s); nothing was changed (--force overrides)", info.name, conflicts);
        st = XPKG_ERR_CONFLICT;
        goto out;
    }

    if (xpkg_opts.dry_run) {
        printf("  would %s %s %s (%zu paths)\n", action, info.name, info.version, ents.n);
        goto out;
    }

    printf("%s(%s)%s %s %s%s%s\n", xpkg_color("\033[1;32m"), action, xpkg_color("\033[0m"),
           info.name, installed && strcmp(action, "reinstall") ? row.version : "",
           installed && strcmp(action, "reinstall") ? " -> " : "", info.version);
    fflush(stdout);

    if (installed) xpkg_db_each_file(info.name, old_cb, &old);

    if (xpkg_db_begin() != XPKG_OK) { st = XPKG_ERR_DB; goto out; }
    in_txn = 1;
    if (xpkg_db_clear_files(info.name) != XPKG_OK) { st = XPKG_ERR_DB; goto out; }

    newpaths = malloc((ents.n + 1) * sizeof(*newpaths));
    size_t nnew = 0;
    if (!newpaths) { st = XPKG_ERR_IO; goto out; }

    for (size_t i = 0; i < ents.n; i++) {
        entry_t *e = &ents.v[i];
        char full[XPKG_MAX_PATH], src[XPKG_MAX_PATH], sha[65];
        snprintf(full, sizeof(full), "%s%s", xpkg_root(), e->path);
        snprintf(src, sizeof(src), "%s%s", files, e->path);
        if (e->type == 'd') {
            struct stat fs;
            if (stat(full, &fs) != 0) {
                if (xpkg_mkdir_p(full, 0755) != 0) {
                    xpkg_err("cannot create %s: %s", full, strerror(errno));
                    st = XPKG_ERR_IO;
                    goto out;
                }
                chmod(full, e->mode ? e->mode : 0755);
            }
            continue;
        }
        char parent[XPKG_MAX_PATH];
        snprintf(parent, sizeof(parent), "%s", full);
        char *sl = strrchr(parent, '/');
        if (sl) { *sl = '\0'; xpkg_mkdir_p(parent, 0755); }

        if (e->type == 'l') {
            char target[XPKG_MAX_PATH];
            ssize_t l = readlink(src, target, sizeof(target) - 1);
            if (l < 0) { st = XPKG_ERR_IO; goto out; }
            target[l] = '\0';
            if (symlink_atomic(target, full) != 0) {
                xpkg_err("cannot create symlink %s: %s", full, strerror(errno));
                st = XPKG_ERR_IO;
                goto out;
            }
            xpkg_sha256_str(target, sha);
        } else {
            if (xpkg_sha256_file(src, sha) != XPKG_OK) { st = XPKG_ERR_IO; goto out; }
            int write_it = 1;
            if (is_config(e->path)) {
                char cur[65];
                if (disk_sha(full, cur) == 0) {
                    const char *was = old_sha(&old, e->path);
                    if (!strcmp(cur, sha)) write_it = 0;                  /* identical */
                    else if (!(was && !strcmp(cur, was))) {               /* edited by the user */
                        char alt[XPKG_MAX_PATH];
                        snprintf(alt, sizeof(alt), "%s.xpkgnew", full);
                        if (copy_file_atomic(src, alt, e->mode) != 0) { st = XPKG_ERR_IO; goto out; }
                        printf("  kept your %s; new version saved as %s.xpkgnew\n", e->path, e->path);
                        write_it = 0;
                    }
                }
            }
            /* a hardlink of a file already written: link it, keep one copy */
            const char *twin = NULL;
            if (e->nlink > 1)
                for (size_t k = 0; k < i && !twin; k++)
                    if (ents.v[k].type == 'f' && ents.v[k].dev == e->dev && ents.v[k].ino == e->ino &&
                        !is_config(ents.v[k].path))
                        twin = ents.v[k].path;
            if (write_it && twin && !is_config(e->path)) {
                char tf[XPKG_MAX_PATH], tmp[XPKG_MAX_PATH];
                snprintf(tf, sizeof(tf), "%s%s", xpkg_root(), twin);
                snprintf(tmp, sizeof(tmp), "%s.xpkg-tmp", full);
                unlink(tmp);
                if (link(tf, tmp) == 0 && rename(tmp, full) == 0) write_it = 0;
                else unlink(tmp);   /* different filesystem: plain copy below */
            }
            if (write_it && copy_file_atomic(src, full, e->mode) != 0) {
                xpkg_err("cannot write %s: %s", full, strerror(errno));
                st = XPKG_ERR_IO;
                goto out;
            }
        }
        if (xpkg_db_add_file(info.name, e->path, sha) != XPKG_OK) { st = XPKG_ERR_DB; goto out; }
        newpaths[nnew++] = e->path;
        if (xpkg_opts.verbose) printf("  %s\n", e->path);
    }

    if (xpkg_db_put_package(&info, explicit_ || (installed && row.explicit_)) != XPKG_OK) {
        st = XPKG_ERR_DB;
        goto out;
    }

    /* files the previous version had and this one does not */
    qsort(newpaths, nnew, sizeof(*newpaths), cmp_path);
    for (size_t i = 0; i < old.n; i++) {
        const char *op = old.path[i];
        if (bsearch(&op, newpaths, nnew, sizeof(*newpaths), cmp_path)) continue;
        char owner[XPKG_MAX_NAME];
        if (xpkg_db_file_owner(op, owner, sizeof(owner))) continue;   /* another package has it now */
        char full[XPKG_MAX_PATH], cur[65];
        snprintf(full, sizeof(full), "%s%s", xpkg_root(), op);
        if (is_config(op) && disk_sha(full, cur) == 0 && strcmp(cur, old.sha[i])) {
            printf("  kept your edited %s\n", op);
            continue;
        }
        struct stat ls;
        if (lstat(full, &ls) == 0 && !S_ISDIR(ls.st_mode)) {
            unlink(full);
            prune_dirs(op);
        }
    }

    /* scripts */
    if (xpkg_mkdir_p(xpkg_scripts_dir(), 0755) == 0) {
        char src[XPKG_MAX_PATH], dst[XPKG_MAX_PATH];
        snprintf(src, sizeof(src), "%s/pre-remove", scratch);
        script_path(info.name, "pre-remove", dst, sizeof(dst));
        if (access(src, R_OK) == 0) copy_file_atomic(src, dst, 0700);
        else unlink(dst);
    }

    if (xpkg_db_commit() != XPKG_OK) { st = XPKG_ERR_DB; goto out; }
    in_txn = 0;

    snprintf(p, sizeof(p), "%s/post-install", scratch);
    run_script(p, installed ? "upgrade" : "install", installed ? row.version : NULL, info.version);

out:
    if (in_txn) xpkg_db_rollback();
    free(newpaths);
    ent_free(&ents);
    old_free(&old);
    xpkg_rm_tree(scratch);
    return st;
}

/* --- remove -------------------------------------------------------------- */

xpkg_status_t xpkg_remove_package(const char *name) {
    if (xpkg_db_open() != XPKG_OK) return XPKG_ERR_DB;
    xpkg_pkg_row_t row;
    if (!xpkg_db_get(name, &row)) {
        xpkg_err("%s is not installed", name);
        return XPKG_ERR_NOT_FOUND;
    }
    if (xpkg_opts.dry_run) {
        printf("  would remove %s %s\n", name, row.version);
        return XPKG_OK;
    }
    printf("%s(remove)%s %s %s\n", xpkg_color("\033[1;31m"), xpkg_color("\033[0m"), name, row.version);

    char script[XPKG_MAX_PATH];
    script_path(name, "pre-remove", script, sizeof(script));
    run_script(script, "remove", row.version, NULL);

    oldfiles_t files = { 0 };
    xpkg_db_each_file(name, old_cb, &files);
    /* deepest paths first, so directories empty out bottom-up */
    for (size_t i = files.n; i-- > 0;) {
        char full[XPKG_MAX_PATH], cur[65];
        snprintf(full, sizeof(full), "%s%s", xpkg_root(), files.path[i]);
        if (is_config(files.path[i]) && disk_sha(full, cur) == 0 && strcmp(cur, files.sha[i])) {
            printf("  kept your edited %s\n", files.path[i]);
            continue;
        }
        struct stat ls;
        if (lstat(full, &ls) == 0 && !S_ISDIR(ls.st_mode)) {
            if (unlink(full) != 0) xpkg_warn("cannot remove %s: %s", full, strerror(errno));
            else if (xpkg_opts.verbose) printf("  removed %s\n", files.path[i]);
        }
        prune_dirs(files.path[i]);
    }
    old_free(&files);

    if (xpkg_db_begin() != XPKG_OK) return XPKG_ERR_DB;
    if (xpkg_db_delete_package(name) != XPKG_OK) { xpkg_db_rollback(); return XPKG_ERR_DB; }
    if (xpkg_db_commit() != XPKG_OK) return XPKG_ERR_DB;
    unlink(script);
    return XPKG_OK;
}
