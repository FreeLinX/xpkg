/* repo.c - repo index, update, dependency resolution, and package fetching.
 *
 * A repo is a directory of *.xpkg files plus an index.json, served over
 * HTTP(S). repos.conf (config dir /repos.conf) lists one base repo URL per
 * line; each line's index.json is at <url>/index.json and each package's
 * archive at <url>/<file> (the "file" field of its index entry).
 *
 * index.json layout (written by tools/xpkg-create.c):
 *   { "packages": { "<name>": {
 *     "version": "1.0", "description": "...", "arch": "x86_64",
 *     "file": "<name>-<ver>.xpkg", "size": 12345, "sha256": "<hex>"
 *   } } }
 *
 * Freshness model:
 *   - `xpkg update` fetches each repo's index.json into the cache dir
 *     (<cache>/idx/<repo>.index.json) and reports what changed vs the
 *     previous cached copy.
 *   - install/upgrade read the cached index if present (so they work after
 *     a single `xpkg update`), otherwise fetch a fresh one on first use.
 *
 * Dependencies:
 *   index.json carries no depends field; a package's DEPENDS lives in the
 *   pkg-info file inside its .xpkg archive. The resolver downloads the
 *   archives involved, reads their pkg-info (xpkg_pkginfo_from_archive),
 *   and works out a dependency-first install order, detecting cycles.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include "xpkg.h"

#define REPO_MAX_LINE XPKG_MAX_LINE
#define MAX_REPOS 32
#define MAX_PKGNAMES 2048
#define MAX_RESOLVED 256

typedef struct {
    char name[XPKG_MAX_NAME];
    char version[XPKG_MAX_VERSION];
    char file[XPKG_MAX_PATH];
    char sha[65];
    char repo[XPKG_MAX_PATH];
} pkg_entry_t;

/* --- repos.conf -------------------------------------------------------- */

static int repos_load(char repos[][XPKG_MAX_PATH], int max) {
    FILE *f = fopen(xpkg_repos_conf(), "r");
    int n = 0;
    if (!f) {
        return 0;
    }
    char line[REPO_MAX_LINE];
    while (n < max && fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;
        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == '/' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
            p[--len] = '\0';
        }
        if (len > 0) {
            strncpy(repos[n], p, XPKG_MAX_PATH - 1);
            repos[n][XPKG_MAX_PATH - 1] = '\0';
            n++;
        }
    }
    fclose(f);
    return n;
}

static void repos_save(char repos[][XPKG_MAX_PATH], int n) {
    mkdir(xpkg_config_dir(), 0755);
    FILE *f = fopen(xpkg_repos_conf(), "w");
    if (!f) return;
    for (int i = 0; i < n; i++) {
        fprintf(f, "%s\n", repos[i]);
    }
    fclose(f);
}

/* --- tiny JSON helpers --------------------------------------------------- */

/* Reads an entire file into a malloc'd NUL-terminated buffer (or NULL). */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    long sz;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (buf) {
        size_t got = fread(buf, 1, (size_t)sz, f);
        buf[got] = '\0';
    }
    fclose(f);
    return buf;
}

/* Given a buffer containing index.json, find the value of a package field.
 * `name` is the package key; `field` the field key. Returns the field value
 * (may be empty) in out. Handles the specific flat structure xpkg-create
 * emits, without pulling in a full JSON library. */
static int json_pkg_field(const char *json, const char *name,
                          const char *field, char *out, size_t outsz) {
    out[0] = '\0';
    char keypat[256];
    snprintf(keypat, sizeof(keypat), "\"%s\"", name);
    const char *pk = strstr(json, keypat);
    if (!pk) return -1;
    const char *brace = strchr(pk, '{');
    if (!brace) return -1;
    const char *end = strchr(brace, '}');
    if (!end) return -1;

    char fpat[128];
    snprintf(fpat, sizeof(fpat), "\"%s\"", field);
    char fk[128];
    snprintf(fk, sizeof(fk), "%s", fpat);

    const char *pos = brace;
    while (pos < end) {
        const char *f = strstr(pos, fpat);
        if (!f || f >= end) break;
        if (strncmp(f, fk, strlen(fk)) != 0) { pos = f + strlen(fpat); continue; }
        const char *colon = f + strlen(fpat);
        while (colon < end && *colon != ':' && *colon != '}') colon++;
        if (colon < end && *colon == ':') {
            colon++;
            while (colon < end && isspace((unsigned char)*colon)) colon++;
            if (colon < end && *colon == '"') {
                colon++;
                const char *vstart = colon;
                while (colon < end && *colon != '"') colon++;
                size_t len = (size_t)(colon - vstart);
                if (len >= outsz) len = outsz - 1;
                memcpy(out, vstart, len);
                out[len] = '\0';
                return 0;
            } else {
                const char *vstart = colon;
                while (colon < end && *colon != ',' && *colon != '}' && *colon != '\n') colon++;
                size_t len = (size_t)(colon - vstart);
                if (len >= outsz) len = outsz - 1;
                memcpy(out, vstart, len);
                out[len] = '\0';
                return 0;
            }
        }
        pos = f + strlen(fpat);
    }
    return -1;
}

/* Collect every package key from an index.json buffer into names[]. */
static int json_name_list(const char *buf, char names[][XPKG_MAX_NAME], int max) {
    int n = 0;
    const char *line = buf;
    while (*line) {
        const char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) : strlen(line);

        const char *p = line;
        while (p < line + len && *p == ' ') p++;
        if (p < line + len && *p == '"') {
            const char *k = p + 1;
            const char *ke = k;
            while (ke < line + len && *ke && *ke != '"') ke++;
            if (ke < line + len && ke + 1 < line + len && ke[1] == ':') {
                const char *after = ke + 1;
                while (after < line + len && (*after == ' ' || *after == ':')) after++;
                if (after < line + len && *after == '{') {
                    size_t kl = (size_t)(ke - k);
                    if (kl > 0 && kl < XPKG_MAX_NAME && n < max &&
                        !(kl == 8 && memcmp(k, "packages", 8) == 0)) {
                        memcpy(names[n], k, kl);
                        names[n][kl] = '\0';
                        n++;
                    }
                }
            }
        }
        if (!nl) break;
        line = nl + 1;
    }
    return n;
}

/* --- cache layout -------------------------------------------------------- */

/* Map an arbitrary repo URL to a safe cache token (letters/digits/_ ). */
static void cache_token(const char *repo, char *out, size_t n) {
    size_t j = 0;
    for (const char *p = repo; *p && j + 2 < n; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
        } else {
            out[j++] = '_';
        }
    }
    out[j] = '\0';
}

static void repo_index_cache_path(const char *repo, char *out, size_t n) {
    char tok[XPKG_MAX_PATH];
    cache_token(repo, tok, sizeof(tok));
    snprintf(out, n, "%s/idx/%s.index.json", xpkg_cache_dir(), tok);
}

static void repo_pkg_cache_dir(const char *repo, char *out, size_t n) {
    char tok[XPKG_MAX_PATH];
    cache_token(repo, tok, sizeof(tok));
    snprintf(out, n, "%s/%s", xpkg_cache_dir(), tok);
}

/* Return the index.json for a repo: use the cached copy if present (install
 * honours the last `xpkg update`), otherwise fetch one now and cache it. */
static char *repo_get_index(const char *repo) {
    char path[XPKG_MAX_PATH];
    repo_index_cache_path(repo, path, sizeof(path));

    char *buf = read_file(path);
    if (!buf) {
        mkdir(xpkg_cache_dir(), 0755);
        char idxdir[XPKG_MAX_PATH];
        snprintf(idxdir, sizeof(idxdir), "%s/idx", xpkg_cache_dir());
        mkdir(idxdir, 0755);

        char url[XPKG_MAX_PATH];
        snprintf(url, sizeof(url), "%s/index.json", repo);
        if (xpkg_net_get(url, path) == XPKG_OK) {
            buf = read_file(path);
        }
    }
    return buf;
}

/* Download (or reuse) a repo package archive, verifying the index sha256.
 * The resolved local path is returned in out_path. */
static int fetch_pkg_archive(pkg_entry_t *e, char *out_path, size_t n) {
    char dir[XPKG_MAX_PATH];
    repo_pkg_cache_dir(e->repo, dir, sizeof(dir));
    mkdir(dir, 0755);

    snprintf(out_path, n, "%s/%s", dir, e->file);
    if (access(out_path, R_OK) == 0) {
        return 0;
    }

    char url[XPKG_MAX_PATH];
    snprintf(url, sizeof(url), "%s/%s", e->repo, e->file);
    printf("xpkg: fetching %s\n", url);
    if (xpkg_net_get(url, out_path) != XPKG_OK) {
        fprintf(stderr, "xpkg: failed to download %s\n", url);
        return 1;
    }
    if (e->sha[0]) {
        char actual[65];
        if (xpkg_sha256_file(out_path, actual) == XPKG_OK &&
            strcasecmp(actual, e->sha) != 0) {
            fprintf(stderr, "xpkg: sha256 mismatch for %s\n", e->file);
            unlink(out_path);
            return 1;
        }
    }
    return 0;
}

/* Find the first (in repos.conf order) repo that lists `name`. */
static int repo_find(const char *name, pkg_entry_t *out) {
    char repos[MAX_REPOS][XPKG_MAX_PATH];
    int n = repos_load(repos, MAX_REPOS);

    for (int i = 0; i < n; i++) {
        char *buf = repo_get_index(repos[i]);
        if (!buf) continue;

        char f[XPKG_MAX_PATH], v[XPKG_MAX_VERSION], s[65];
        if (json_pkg_field(buf, name, "file", f, sizeof(f)) == 0) {
            strncpy(out->name, name, sizeof(out->name) - 1);
            json_pkg_field(buf, name, "version", v, sizeof(v));
            json_pkg_field(buf, name, "sha256", s, sizeof(s));
            strncpy(out->version, v, sizeof(out->version) - 1);
            strncpy(out->file, f, sizeof(out->file) - 1);
            strncpy(out->sha, s, sizeof(out->sha) - 1);
            strncpy(out->repo, repos[i], sizeof(out->repo) - 1);
            out->name[sizeof(out->name) - 1] = '\0';
            out->version[sizeof(out->version) - 1] = '\0';
            out->file[sizeof(out->file) - 1] = '\0';
            out->sha[sizeof(out->sha) - 1] = '\0';
            out->repo[sizeof(out->repo) - 1] = '\0';
            free(buf);
            return 1;
        }
        free(buf);
    }
    return 0;
}

/* --- dependency resolver ------------------------------------------------ */

static pkg_entry_t g_resolved[MAX_RESOLVED];
static char g_archive[MAX_RESOLVED][XPKG_MAX_PATH];
static int g_rstate[MAX_RESOLVED];     /* 0 new, 1 visiting, 2 done */
static int g_nresolved;
static char g_order[MAX_RESOLVED][XPKG_MAX_NAME];
static int g_norder;

static void resolver_reset(void) {
    memset(g_resolved, 0, sizeof(g_resolved));
    memset(g_archive, 0, sizeof(g_archive));
    memset(g_rstate, 0, sizeof(g_rstate));
    memset(g_order, 0, sizeof(g_order));
    g_nresolved = 0;
    g_norder = 0;
}

static int resolver_find(const char *name) {
    for (int i = 0; i < g_nresolved; i++) {
        if (strcmp(g_resolved[i].name, name) == 0) return i;
    }
    return -1;
}

/* Depth-first resolve of `name` and its not-yet-installed dependencies.
 * g_order ends up in dependency-first install order. */
static xpkg_status_t resolve_name(const char *name) {
    int idx = resolver_find(name);
    if (idx >= 0) {
        if (g_rstate[idx] == 1) {
            fprintf(stderr, "xpkg: dependency cycle detected involving: %s\n", name);
            return XPKG_ERR_CYCLE;
        }
        return XPKG_OK; /* already fully resolved */
    }
    if (g_nresolved >= MAX_RESOLVED) {
        fprintf(stderr, "xpkg: dependency graph too deep (limit %d)\n", MAX_RESOLVED);
        return XPKG_ERR_IO;
    }

    pkg_entry_t e;
    if (!repo_find(name, &e)) {
        fprintf(stderr, "xpkg: package not found in any repository: %s\n", name);
        return XPKG_ERR_NOT_FOUND;
    }

    idx = g_nresolved++;
    g_resolved[idx] = e;
    g_rstate[idx] = 1;

    char arch[XPKG_MAX_PATH];
    if (fetch_pkg_archive(&g_resolved[idx], arch, sizeof(arch)) != 0) {
        return XPKG_ERR_IO;
    }
    strncpy(g_archive[idx], arch, sizeof(g_archive[idx]) - 1);
    g_archive[idx][sizeof(g_archive[idx]) - 1] = '\0';

    xpkg_info_t info;
    if (xpkg_pkginfo_from_archive(arch, &info) != XPKG_OK) {
        fprintf(stderr, "xpkg: cannot read package metadata from %s\n", arch);
        return XPKG_ERR_BAD_ARCHIVE;
    }

    for (int i = 0; i < info.depends_count; i++) {
        int dep_installed;
        xpkg_db_is_installed(info.depends[i], &dep_installed);
        if (!dep_installed) {
            xpkg_status_t st = resolve_name(info.depends[i]);
            if (st != XPKG_OK) return st;
        }
    }

    g_rstate[idx] = 2;
    if (g_norder < MAX_RESOLVED) {
        strncpy(g_order[g_norder], name, sizeof(g_order[g_norder]) - 1);
        g_order[g_norder][sizeof(g_order[g_norder]) - 1] = '\0';
        g_norder++;
    }
    return XPKG_OK;
}

/* --- commands ------------------------------------------------------------ */

int xpkg_cmd_update(void) {
    char repos[MAX_REPOS][XPKG_MAX_PATH];
    int n = repos_load(repos, MAX_REPOS);
    if (n == 0) {
        fprintf(stderr, "xpkg: no repos configured (%s)\n", xpkg_repos_conf());
        fprintf(stderr, "xpkg: add one with: xpkg repo add <url>\n");
        return 1;
    }

    mkdir(xpkg_cache_dir(), 0755);
    char idxdir[XPKG_MAX_PATH];
    snprintf(idxdir, sizeof(idxdir), "%s/idx", xpkg_cache_dir());
    mkdir(idxdir, 0755);

    int failures = 0;
    for (int i = 0; i < n; i++) {
        const char *repo = repos[i];

        char cachepath[XPKG_MAX_PATH];
        repo_index_cache_path(repo, cachepath, sizeof(cachepath));

        /* stash the previous cached index so we can report a diff */
        char oldpath[XPKG_MAX_PATH];
        snprintf(oldpath, sizeof(oldpath), "%s.prev", cachepath);
        if (access(cachepath, R_OK) == 0) {
            if (rename(cachepath, oldpath) != 0) {
                remove(cachepath);
            }
        }

        char url[XPKG_MAX_PATH];
        snprintf(url, sizeof(url), "%s/index.json", repo);
        printf("xpkg: fetching %s\n", url);
        if (xpkg_net_get(url, cachepath) != XPKG_OK) {
            fprintf(stderr, "xpkg: update failed for repo %s\n", repo);
            remove(cachepath);
            if (access(oldpath, R_OK) == 0) {
                rename(oldpath, cachepath); /* keep last good cache */
            }
            failures++;
            continue;
        }

        char *newbuf = read_file(cachepath);
        if (!newbuf) {
            fprintf(stderr, "xpkg: could not read cached index for %s\n", repo);
            failures++;
            continue;
        }

        char names[MAX_PKGNAMES][XPKG_MAX_NAME];
        int nn = json_name_list(newbuf, names, MAX_PKGNAMES);
        printf("xpkg: repo %s: %d packages\n", repo, nn);

        char *oldbuf = read_file(oldpath);
        if (oldbuf) {
            /* diff old vs new */
            char oldnames[MAX_PKGNAMES][XPKG_MAX_NAME];
            int on = json_name_list(oldbuf, oldnames, MAX_PKGNAMES);

            int removed = 0, added = 0, changed = 0;
            /* removed: in old, not in new */
            for (int a = 0; a < on; a++) {
                int still = 0;
                for (int b = 0; b < nn; b++) {
                    if (strcmp(oldnames[a], names[b]) == 0) { still = 1; break; }
                }
                if (!still) {
                    printf("xpkg:   removed: %s\n", oldnames[a]);
                    removed++;
                }
            }
            /* added / changed */
            for (int b = 0; b < nn; b++) {
                int was = 0;
                for (int a = 0; a < on; a++) {
                    if (strcmp(oldnames[a], names[b]) == 0) { was = 1; break; }
                }
                if (!was) {
                    printf("xpkg:   added: %s\n", names[b]);
                    added++;
                } else {
                    char oldver[XPKG_MAX_VERSION], newver[XPKG_MAX_VERSION];
                    json_pkg_field(oldbuf, names[b], "version", oldver, sizeof(oldver));
                    json_pkg_field(newbuf, names[b], "version", newver, sizeof(newver));
                    if (strcmp(oldver, newver) != 0) {
                        printf("xpkg:   changed: %s %s -> %s\n", names[b], oldver, newver);
                        changed++;
                    }
                }
            }
            if (added || removed || changed) {
                if (added) printf("xpkg:   +%d added", added);
                if (removed) printf("xpkg:   -%d removed", removed);
                if (changed) printf("xpkg:   ~%d changed", changed);
                printf("\n");
            } else {
                printf("xpkg:   (no changes)\n");
            }
            free(oldbuf);
            remove(oldpath);
        }

        free(newbuf);
    }
    return failures ? 1 : 0;
}

int xpkg_cmd_install_repo(const char *name) {
    char repos[MAX_REPOS][XPKG_MAX_PATH];
    int nrepos = repos_load(repos, MAX_REPOS);
    if (nrepos == 0) {
        fprintf(stderr, "xpkg: no repos configured (%s)\n", xpkg_repos_conf());
        fprintf(stderr, "xpkg: add one with: xpkg repo add <url>\n");
        return 1;
    }

    int installed;
    xpkg_db_is_installed(name, &installed);
    if (installed) {
        fprintf(stderr, "xpkg: %s is already installed (use 'xpkg upgrade %s' to update)\n",
                name, name);
        return 1;
    }

    resolver_reset();
    xpkg_status_t st = resolve_name(name);
    if (st != XPKG_OK) {
        return 1;
    }

    for (int i = 0; i < g_norder; i++) {
        int idx = resolver_find(g_order[i]);
        if (idx < 0) {
            fprintf(stderr, "xpkg: internal resolver error for %s\n", g_order[i]);
            return 1;
        }
        if (xpkg_cmd_install_ex(g_archive[idx], 1) != 0) {
            return 1;
        }
    }
    return 0;
}

int xpkg_cmd_upgrade(const char *name) {
    int installed;
    xpkg_db_is_installed(name, &installed);
    if (!installed) {
        fprintf(stderr, "xpkg: %s is not installed\n", name);
        return 1;
    }

    pkg_entry_t e;
    if (!repo_find(name, &e)) {
        fprintf(stderr, "xpkg: no package named %s in any repo\n", name);
        return 1;
    }

    char arch[XPKG_MAX_PATH];
    if (fetch_pkg_archive(&e, arch, sizeof(arch)) != 0) {
        return 1;
    }
    return xpkg_cmd_upgrade_archive(arch);
}

typedef struct {
    int upgraded;
    int uptodate;
    int norepo;
    int failed;
} upgrade_stats_t;

static int upgrade_one(const char *name, void *user) {
    upgrade_stats_t *s = (upgrade_stats_t *)user;

    pkg_entry_t e;
    if (!repo_find(name, &e)) {
        printf("xpkg: %s: no update available in any repo\n", name);
        s->norepo++;
        return 0;
    }

    char arch[XPKG_MAX_PATH];
    if (fetch_pkg_archive(&e, arch, sizeof(arch)) != 0) {
        s->failed++;
        return 0;
    }

    xpkg_info_t info;
    if (xpkg_pkginfo_from_archive(arch, &info) != XPKG_OK) {
        printf("xpkg: %s: bad package metadata\n", name);
        s->failed++;
        return 0;
    }

    char oldver[XPKG_MAX_VERSION] = {0};
    xpkg_db_get_version(name, oldver, sizeof(oldver));

    if (xpkg_version_cmp(info.version, oldver) <= 0) {
        printf("xpkg: %s %s up to date\n", name, info.version);
        s->uptodate++;
        return 0;
    }

    printf("xpkg: upgrading %s %s -> %s\n", name, oldver, info.version);
    if (xpkg_cmd_upgrade_archive(arch) == 0) {
        s->upgraded++;
    } else {
        s->failed++;
    }
    return 0;
}

int xpkg_cmd_upgrade_all(void) {
    char repos[MAX_REPOS][XPKG_MAX_PATH];
    int nrepos = repos_load(repos, MAX_REPOS);
    if (nrepos == 0) {
        fprintf(stderr, "xpkg: no repos configured (%s)\n", xpkg_repos_conf());
        fprintf(stderr, "xpkg: add one with: xpkg repo add <url>\n");
        return 1;
    }

    upgrade_stats_t s = {0, 0, 0, 0};
    xpkg_db_foreach(upgrade_one, &s);

    printf("xpkg: upgrade results: %d upgraded, %d up to date, %d no repo package, %d failed\n",
           s.upgraded, s.uptodate, s.norepo, s.failed);
    return s.failed ? 1 : 0;
}

/* --- repo add/remove/list ---------------------------------------------- */

int xpkg_cmd_repo_add(const char *url) {
    char repos[MAX_REPOS][XPKG_MAX_PATH];
    int n = repos_load(repos, MAX_REPOS);
    for (int i = 0; i < n; i++) {
        if (strcmp(repos[i], url) == 0) {
            printf("xpkg: repo already present: %s\n", url);
            return 0;
        }
    }
    if (n >= MAX_REPOS) {
        fprintf(stderr, "xpkg: too many repos (max %d)\n", MAX_REPOS);
        return 1;
    }
    strncpy(repos[n], url, XPKG_MAX_PATH - 1);
    repos[n][XPKG_MAX_PATH - 1] = '\0';
    repos_save(repos, n + 1);
    printf("xpkg: added repo: %s\n", url);
    return 0;
}

int xpkg_cmd_repo_remove(const char *url) {
    char repos[MAX_REPOS][XPKG_MAX_PATH];
    int n = repos_load(repos, MAX_REPOS);
    int removed = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(repos[i], url) == 0) {
            for (int j = i; j < n - 1; j++) {
                strcpy(repos[j], repos[j + 1]);
            }
            n--;
            removed = 1;
            i--;
        }
    }
    repos_save(repos, n);
    if (!removed) {
        fprintf(stderr, "xpkg: repo not found: %s\n", url);
        return 1;
    }
    printf("xpkg: removed repo: %s\n", url);
    return 0;
}

int xpkg_cmd_repo_list(void) {
    char repos[MAX_REPOS][XPKG_MAX_PATH];
    int n = repos_load(repos, MAX_REPOS);
    if (n == 0) {
        printf("(no repos configured; add with: xpkg repo add <url>)\n");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        printf("%s\n", repos[i]);
    }
    return 0;
}