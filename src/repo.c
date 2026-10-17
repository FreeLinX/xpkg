/* repo.c - repositories: index download, signature policy, lookups.
 *
 * A repo is a URL serving index.json, index.json.sig and the archives:
 *
 *   { "generated": 1790000000,
 *     "packages": { "<name>": { "version": "1.0", "description": "...",
 *         "arch": "x86_64", "file": "<name>-1.0.xpkg", "size": 123,
 *         "sha256": "<hex>", "depends": ["a", "b"] } } }
 *
 * Trust: every index must carry a valid Ed25519 signature by one of the keys
 * in /etc/xpkg/keys; no keys means no repo is accepted (--allow-unsigned
 * overrides both).
 * The signature covers the whole index, and the index pins every archive's
 * size and sha256, so nothing unsigned is ever installed.  `generated`
 * must not go backwards between updates, which stops a mirror from
 * replaying an old signed index to hold back security fixes.
 *
 * Repos are consulted in repos.conf order; the first one that lists a
 * name provides it.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include "xpkg.h"

#define MAX_REPOS 32

static char g_repos[MAX_REPOS][XPKG_MAX_PATH];
static int g_nrepos = -1;
static xpkg_entry_t *g_idx;
static int g_nidx, g_capidx;
static int g_loaded;

/* --- repos.conf ----------------------------------------------------------- */

static void repos_load(void) {
    if (g_nrepos >= 0) return;
    g_nrepos = 0;
    FILE *f = fopen(xpkg_repos_conf(), "r");
    if (!f) return;
    char line[XPKG_MAX_PATH];
    while (g_nrepos < MAX_REPOS && fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        size_t l = strlen(p);
        while (l > 0 && (isspace((unsigned char)p[l - 1]) || p[l - 1] == '/')) p[--l] = '\0';
        if (!*p || *p == '#') continue;
        snprintf(g_repos[g_nrepos++], XPKG_MAX_PATH, "%s", p);
    }
    fclose(f);
}

static int repos_save(void) {
    if (xpkg_mkdir_p(xpkg_config_dir(), 0755) != 0) return -1;
    char *buf = malloc((size_t)(g_nrepos + 2) * XPKG_MAX_PATH);
    if (!buf) return -1;
    size_t o = (size_t)sprintf(buf, "# xpkg repositories, one URL per line, highest priority first\n");
    for (int i = 0; i < g_nrepos; i++) o += (size_t)sprintf(buf + o, "%s\n", g_repos[i]);
    int r = xpkg_write_file_atomic(xpkg_repos_conf(), buf, o);
    free(buf);
    return r;
}

int xpkg_repos_count(void) { repos_load(); return g_nrepos; }
const char *xpkg_repo_url(int i) { repos_load(); return (i >= 0 && i < g_nrepos) ? g_repos[i] : ""; }

static void repo_token(const char *url, char *out, size_t n) {
    size_t j = 0;
    for (const char *p = url; *p && j + 1 < n; p++)
        out[j++] = isalnum((unsigned char)*p) ? *p : '_';
    out[j] = '\0';
}

static void idx_paths(const char *url, char *idx, char *sig, size_t n) {
    char tok[XPKG_MAX_PATH];
    repo_token(url, tok, sizeof(tok));
    snprintf(idx, n, "%s/idx/%s.index.json", xpkg_cache_dir(), tok);
    snprintf(sig, n, "%s/idx/%s.index.json.sig", xpkg_cache_dir(), tok);
}

/* --- signature policy --------------------------------------------------- */

static xpkg_status_t check_signature(const char *url, const char *data, size_t len, const char *sig) {
    /* no keys means nothing can be verified: refuse, like a bad signature
     * (a deleted key directory must not quietly turn checking off) */
    if (!xpkg_have_keys()) {
        if (xpkg_opts.allow_unsigned) {
            static int warned;
            if (!warned) xpkg_warn("no trusted keys in %s: repository signatures are not checked", xpkg_keys_dir());
            warned = 1;
            return XPKG_OK;
        }
        xpkg_err("no trusted keys in %s: cannot verify %s (use --allow-unsigned to override)", xpkg_keys_dir(), url);
        return XPKG_ERR_SIGNATURE;
    }
    if (!sig) {
        if (xpkg_opts.allow_unsigned) {
            xpkg_warn("%s is not signed (accepted: --allow-unsigned)", url);
            return XPKG_OK;
        }
        xpkg_err("%s has no index signature; refusing it (use --allow-unsigned to override)", url);
        return XPKG_ERR_SIGNATURE;
    }
    char key[128];
    xpkg_status_t st = xpkg_verify_signature(data, len, sig, key, sizeof(key));
    if (st == XPKG_OK) return XPKG_OK;
    if (xpkg_opts.allow_unsigned) {
        xpkg_warn("bad signature on %s (accepted: --allow-unsigned)", url);
        return XPKG_OK;
    }
    xpkg_err("BAD SIGNATURE on the index of %s: it was not signed by a trusted key", url);
    return XPKG_ERR_SIGNATURE;
}

/* --- index parsing -------------------------------------------------------- */

static void idx_free(void) {
    for (int i = 0; i < g_nidx; i++) free(g_idx[i].description);
    free(g_idx);
    g_idx = NULL;
    g_nidx = g_capidx = 0;
    g_loaded = 0;
}

const xpkg_entry_t *xpkg_index_find(const char *name) {
    for (int i = 0; i < g_nidx; i++)
        if (!strcmp(g_idx[i].name, name)) return &g_idx[i];
    return NULL;
}

int xpkg_index_count(void) { return g_nidx; }
const xpkg_entry_t *xpkg_index_at(int i) { return (i >= 0 && i < g_nidx) ? &g_idx[i] : NULL; }

static int valid_file_name(const char *f) {
    return f[0] && f[0] != '.' && !strchr(f, '/') && !strchr(f, '\\') && strlen(f) < 256;
}

static int valid_sha(const char *s) {
    if (strlen(s) != 64) return 0;
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return 0;
    return 1;
}

static int add_entries(int repo, const json_t *doc) {
    const json_t *pk = json_get(doc, "packages");
    if (!pk || pk->type != JSON_OBJ) return -1;
    for (const json_t *p = pk->child; p; p = p->next) {
        if (p->type != JSON_OBJ || !xpkg_valid_name(p->key)) continue;
        if (xpkg_index_find(p->key)) continue;   /* earlier repo wins */
        const char *file = json_str(p, "file", ""), *sha = json_str(p, "sha256", "");
        if (!valid_file_name(file) || !valid_sha(sha)) {
            xpkg_warn("%s: ignoring malformed index entry %s", g_repos[repo], p->key);
            continue;
        }
        if (g_nidx == g_capidx) {
            int nc = g_capidx ? g_capidx * 2 : 512;
            xpkg_entry_t *n = realloc(g_idx, (size_t)nc * sizeof(*n));
            if (!n) return -1;
            g_idx = n;
            g_capidx = nc;
        }
        xpkg_entry_t *e = &g_idx[g_nidx];
        memset(e, 0, sizeof(*e));
        snprintf(e->name, sizeof(e->name), "%s", p->key);
        snprintf(e->version, sizeof(e->version), "%s", json_str(p, "version", "0"));
        e->description = strdup(json_str(p, "description", ""));
        snprintf(e->arch, sizeof(e->arch), "%s", json_str(p, "arch", "x86_64"));
        snprintf(e->file, sizeof(e->file), "%s", file);
        snprintf(e->sha, sizeof(e->sha), "%s", sha);
        for (char *c = e->sha; *c; c++) *c = (char)tolower((unsigned char)*c);
        e->size = (unsigned long long)json_num(p, "size", 0);
        const json_t *deps = json_get(p, "depends");
        if (deps && deps->type == JSON_ARR) {
            e->has_depends = 1;
            for (const json_t *d = deps->child; d && e->depends_count < XPKG_MAX_DEPENDS; d = d->next)
                if (d->type == JSON_STR && d->str[0])
                    snprintf(e->depends[e->depends_count++], XPKG_MAX_NAME, "%s", d->str);
        } else if (deps && deps->type == JSON_STR) {
            xpkg_info_t tmp;
            char line[XPKG_MAX_LINE + 16];
            snprintf(line, sizeof(line), "NAME=x\nVERSION=0\nDEPENDS=%s\n", deps->str);
            if (xpkg_parse_pkginfo_data(line, &tmp) == XPKG_OK) {
                e->has_depends = 1;
                e->depends_count = tmp.depends_count;
                memcpy(e->depends, tmp.depends, sizeof(e->depends));
            }
        }
        e->repo = repo;
        g_nidx++;
    }
    return 0;
}

static int arch_ok(const char *a) {
    return !a[0] || !strcmp(a, "x86_64") || !strcmp(a, "any") || !strcmp(a, "noarch");
}

/* Downloads one repo's index + signature into the cache, verified. */
static xpkg_status_t update_repo(int r, int *added, int *changed, int *removed) {
    const char *url = g_repos[r];
    char idx[XPKG_MAX_PATH], sig[XPKG_MAX_PATH], tidx[XPKG_MAX_PATH], tsig[XPKG_MAX_PATH];
    idx_paths(url, idx, sig, sizeof(idx));
    snprintf(tidx, sizeof(tidx), "%s.new", idx);
    snprintf(tsig, sizeof(tsig), "%s.new", sig);
    char dir[XPKG_MAX_PATH];
    snprintf(dir, sizeof(dir), "%s/idx", xpkg_cache_dir());
    if (xpkg_mkdir_p(dir, 0755) != 0) {
        xpkg_err("cannot create %s: %s", dir, strerror(errno));
        return XPKG_ERR_IO;
    }

    char u[XPKG_MAX_URL];
    snprintf(u, sizeof(u), "%s/index.json", url);
    if (xpkg_net_get(u, tidx, NULL, 64ULL << 20) != XPKG_OK) {
        xpkg_err("cannot fetch the index of %s", url);
        return XPKG_ERR_IO;
    }
    snprintf(u, sizeof(u), "%s/index.json.sig", url);
    xpkg_net_quiet_404 = 1;
    int have_sig = xpkg_net_get(u, tsig, NULL, 64ULL << 10) == XPKG_OK;
    xpkg_net_quiet_404 = 0;
    if (!have_sig) unlink(tsig);

    size_t len = 0;
    char *data = xpkg_read_file(tidx, &len);
    char *sigtext = have_sig ? xpkg_read_file(tsig, NULL) : NULL;
    xpkg_status_t st = data ? check_signature(url, data, len, sigtext) : XPKG_ERR_IO;
    char perr[128];
    json_t *doc = NULL;
    if (st == XPKG_OK) {
        doc = json_parse(data, perr, sizeof(perr));
        if (!doc || !json_get(doc, "packages")) {
            xpkg_err("%s: index is not valid (%s)", url, doc ? "no packages" : perr);
            st = XPKG_ERR_BAD_ARCHIVE;
        }
    }

    /* anti-rollback + change report against the previous cache */
    char *olddata = xpkg_read_file(idx, NULL);
    json_t *old = olddata ? json_parse(olddata, perr, sizeof(perr)) : NULL;
    if (st == XPKG_OK && old) {
        double og = json_num(old, "generated", 0), ng = json_num(doc, "generated", 0);
        if (ng < og && !xpkg_opts.force) {
            xpkg_err("%s: index is older than the one already cached (possible replay); "
                     "use --force to accept it", url);
            st = XPKG_ERR_SIGNATURE;
        }
    }
    if (st == XPKG_OK) {
        const json_t *np = json_get(doc, "packages");
        const json_t *op = old ? json_get(old, "packages") : NULL;
        for (const json_t *p = np->child; p; p = p->next) {
            const json_t *o = op ? json_get(op, p->key) : NULL;
            if (!o) (*added)++;
            else if (strcmp(json_str(o, "version", ""), json_str(p, "version", "")) ||
                     strcmp(json_str(o, "sha256", ""), json_str(p, "sha256", "")))
                (*changed)++;
        }
        if (op)
            for (const json_t *p = op->child; p; p = p->next)
                if (!json_get(np, p->key)) (*removed)++;
        if (rename(tidx, idx) != 0) st = XPKG_ERR_IO;
        if (have_sig) rename(tsig, sig);
        else unlink(sig);
    }
    unlink(tidx);
    unlink(tsig);
    json_free(doc);
    json_free(old);
    free(olddata);
    free(data);
    free(sigtext);
    return st;
}

static xpkg_status_t load_repo(int r, int fetch_missing) {
    char idx[XPKG_MAX_PATH], sig[XPKG_MAX_PATH];
    idx_paths(g_repos[r], idx, sig, sizeof(idx));
    if (access(idx, R_OK) != 0) {
        if (!fetch_missing) return XPKG_ERR_NOT_FOUND;
        int a = 0, c = 0, d = 0;
        xpkg_msg("fetching the package index of %s", g_repos[r]);
        xpkg_status_t st = update_repo(r, &a, &c, &d);
        if (st != XPKG_OK) return st;
    }
    size_t len = 0;
    char *data = xpkg_read_file(idx, &len);
    if (!data) return XPKG_ERR_IO;
    char *sigtext = xpkg_read_file(sig, NULL);
    xpkg_status_t st = check_signature(g_repos[r], data, len, sigtext);
    free(sigtext);
    if (st != XPKG_OK) { free(data); return st; }
    char perr[128];
    json_t *doc = json_parse(data, perr, sizeof(perr));
    free(data);
    if (!doc) {
        xpkg_err("cached index of %s is corrupt (%s); run xpkg update", g_repos[r], perr);
        return XPKG_ERR_BAD_ARCHIVE;
    }
    int rc = add_entries(r, doc);
    json_free(doc);
    return rc == 0 ? XPKG_OK : XPKG_ERR_IO;
}

xpkg_status_t xpkg_index_load(int fetch_missing) {
    if (g_loaded) return XPKG_OK;
    repos_load();
    if (g_nrepos == 0) {
        xpkg_err("no repositories configured (%s); add one with: xpkg repo add <url>", xpkg_repos_conf());
        return XPKG_ERR_NOT_FOUND;
    }
    int ok = 0;
    for (int r = 0; r < g_nrepos; r++) {
        xpkg_status_t st = load_repo(r, fetch_missing);
        if (st == XPKG_OK) ok++;
        else if (st == XPKG_ERR_SIGNATURE) return st;   /* never silently skip */
    }
    g_loaded = 1;
    return ok ? XPKG_OK : XPKG_ERR_NOT_FOUND;
}

/* --- archive fetch ------------------------------------------------------- */

static int archive_ok(const char *path, const xpkg_entry_t *e) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (e->size && (unsigned long long)st.st_size != e->size) return 0;
    char sha[65];
    if (xpkg_sha256_file(path, sha) != XPKG_OK) return 0;
    return !strcmp(sha, e->sha);
}

xpkg_status_t xpkg_fetch_entry(const xpkg_entry_t *e, char *out, size_t n) {
    char tok[XPKG_MAX_PATH], dir[XPKG_MAX_PATH];
    repo_token(g_repos[e->repo], tok, sizeof(tok));
    snprintf(dir, sizeof(dir), "%s/pkg/%s", xpkg_cache_dir(), tok);
    if (xpkg_mkdir_p(dir, 0755) != 0) {
        xpkg_err("cannot create %s: %s", dir, strerror(errno));
        return XPKG_ERR_IO;
    }
    snprintf(out, n, "%s/%s", dir, e->file);
    if (archive_ok(out, e)) return XPKG_OK;      /* verified cache hit */
    unlink(out);

    char url[XPKG_MAX_URL], label[160];
    snprintf(url, sizeof(url), "%s/%s", g_repos[e->repo], e->file);
    snprintf(label, sizeof(label), "%s-%s", e->name, e->version);
    if (!xpkg_is_tty() && !xpkg_opts.quiet) printf("  downloading %s\n", label);
    if (xpkg_net_get(url, out, label, e->size ? e->size : 4ULL << 30) != XPKG_OK) {
        xpkg_err("download of %s failed", label);
        return XPKG_ERR_IO;
    }
    if (!archive_ok(out, e)) {
        unlink(out);
        xpkg_err("%s: checksum mismatch - the download does not match the repository index", label);
        return XPKG_ERR_VERIFY_FAILED;
    }
    return XPKG_OK;
}

/* --- commands ------------------------------------------------------------ */

int xpkg_cmd_update(void) {
    repos_load();
    if (g_nrepos == 0) {
        xpkg_err("no repositories configured; add one with: xpkg repo add <url>");
        return 1;
    }
    int fails = 0;
    for (int r = 0; r < g_nrepos; r++) {
        int a = 0, c = 0, d = 0;
        xpkg_msg("updating %s", g_repos[r]);
        if (update_repo(r, &a, &c, &d) != XPKG_OK) { fails++; continue; }
        if (!xpkg_opts.quiet)
            printf("  %d new, %d updated, %d removed\n", a, c, d);
    }
    idx_free();
    return fails ? 1 : 0;
}

static int ci_contains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (; *hay; hay++)
        if (!strncasecmp(hay, needle, nl)) return 1;
    return 0;
}

static int cmp_entry_name(const void *a, const void *b) {
    const xpkg_entry_t *const *x = a, *const *y = b;
    return strcmp((*x)->name, (*y)->name);
}

int xpkg_cmd_search(const char *term) {
    if (xpkg_index_load(1) != XPKG_OK) return 1;
    const xpkg_entry_t **hits = malloc((size_t)(g_nidx + 1) * sizeof(*hits));
    if (!hits) return 1;
    int n = 0;
    for (int i = 0; i < g_nidx; i++)
        if (ci_contains(g_idx[i].name, term) || ci_contains(g_idx[i].description, term))
            hits[n++] = &g_idx[i];
    qsort(hits, (size_t)n, sizeof(*hits), cmp_entry_name);
    xpkg_db_open();
    for (int i = 0; i < n; i++) {
        xpkg_pkg_row_t row;
        int inst = xpkg_db_get(hits[i]->name, &row);
        printf("%s%s%s %s%s%s", xpkg_color("\033[1m"), hits[i]->name, xpkg_color("\033[0m"),
               xpkg_color("\033[32m"), hits[i]->version, xpkg_color("\033[0m"));
        if (inst) {
            if (strcmp(row.version, hits[i]->version)) printf(" [installed: %s]", row.version);
            else printf(" [installed]");
        }
        printf("\n    %s\n", hits[i]->description[0] ? hits[i]->description : "(no description)");
    }
    free(hits);
    if (n == 0) {
        fprintf(stderr, "no packages match '%s'\n", term);
        return 1;
    }
    return 0;
}

int xpkg_cmd_show(const char *name) {
    if (xpkg_index_load(1) != XPKG_OK) return 1;
    const xpkg_entry_t *e = xpkg_index_find(name);
    xpkg_db_open();
    xpkg_pkg_row_t row;
    int inst = xpkg_db_get(name, &row);
    if (!e) {
        if (inst) return xpkg_cmd_info(name);
        xpkg_err("no package named %s", name);
        return 1;
    }
    char size[32];
    xpkg_human_size(e->size, size, sizeof(size));
    printf("Name        : %s\n", e->name);
    printf("Version     : %s\n", e->version);
    printf("Description : %s\n", e->description[0] ? e->description : "-");
    printf("Architecture: %s\n", e->arch);
    printf("Depends on  : ");
    if (!e->has_depends) printf("(listed in the package)");
    else if (!e->depends_count) printf("-");
    for (int i = 0; i < e->depends_count; i++) printf("%s%s", i ? " " : "", e->depends[i]);
    printf("\nDownload    : %s\n", size);
    printf("Repository  : %s\n", g_repos[e->repo]);
    printf("Installed   : %s\n", inst ? row.version : "no");
    if (!arch_ok(e->arch)) printf("Note        : not built for this machine\n");
    return 0;
}

typedef struct { int n; } outdated_ctx_t;

static int outdated_cb(const xpkg_pkg_row_t *row, void *user) {
    outdated_ctx_t *c = user;
    const xpkg_entry_t *e = xpkg_index_find(row->name);
    if (e && xpkg_version_cmp(e->version, row->version) > 0) {
        printf("%s %s -> %s\n", row->name, row->version, e->version);
        c->n++;
    }
    return 0;
}

int xpkg_cmd_outdated(void) {
    if (xpkg_index_load(1) != XPKG_OK) return 1;
    if (xpkg_db_open() != XPKG_OK) return 1;
    outdated_ctx_t c = { 0 };
    xpkg_db_each_package(outdated_cb, &c);
    if (!c.n && !xpkg_opts.quiet) printf("everything is up to date\n");
    return 0;
}

int xpkg_cmd_repo_add(const char *url) {
    if (strncasecmp(url, "https://", 8) && strncasecmp(url, "http://", 7)) {
        xpkg_err("repository URLs start with https:// (or http://)");
        return 1;
    }
    if (!strncasecmp(url, "http://", 7))
        xpkg_warn("plain HTTP: only the index signature protects this repository");
    repos_load();
    char clean[XPKG_MAX_PATH];
    snprintf(clean, sizeof(clean), "%s", url);
    size_t l = strlen(clean);
    while (l > 0 && clean[l - 1] == '/') clean[--l] = '\0';
    for (int i = 0; i < g_nrepos; i++)
        if (!strcmp(g_repos[i], clean)) { printf("already configured: %s\n", clean); return 0; }
    if (g_nrepos >= MAX_REPOS) { xpkg_err("too many repositories (max %d)", MAX_REPOS); return 1; }
    snprintf(g_repos[g_nrepos++], XPKG_MAX_PATH, "%s", clean);
    if (repos_save() != 0) { xpkg_err("cannot write %s", xpkg_repos_conf()); return 1; }
    printf("added %s (run: xpkg update)\n", clean);
    return 0;
}

int xpkg_cmd_repo_remove(const char *url) {
    repos_load();
    char clean[XPKG_MAX_PATH];
    snprintf(clean, sizeof(clean), "%s", url);
    size_t l = strlen(clean);
    while (l > 0 && clean[l - 1] == '/') clean[--l] = '\0';
    int found = 0;
    for (int i = 0; i < g_nrepos; i++) {
        if (strcmp(g_repos[i], clean)) continue;
        found = 1;
        char idx[XPKG_MAX_PATH], sig[XPKG_MAX_PATH];
        idx_paths(clean, idx, sig, sizeof(idx));
        unlink(idx);
        unlink(sig);
        memmove(g_repos[i], g_repos[i + 1], (size_t)(g_nrepos - i - 1) * XPKG_MAX_PATH);
        g_nrepos--;
        break;
    }
    if (!found) { xpkg_err("not configured: %s", clean); return 1; }
    if (repos_save() != 0) { xpkg_err("cannot write %s", xpkg_repos_conf()); return 1; }
    printf("removed %s\n", clean);
    return 0;
}

int xpkg_cmd_repo_list(void) {
    repos_load();
    if (!g_nrepos) { printf("(no repositories; add one with: xpkg repo add <url>)\n"); return 0; }
    for (int i = 0; i < g_nrepos; i++) {
        char idx[XPKG_MAX_PATH], sig[XPKG_MAX_PATH];
        idx_paths(g_repos[i], idx, sig, sizeof(idx));
        const char *state = access(idx, R_OK) ? "not fetched yet"
                          : access(sig, R_OK) ? "unsigned" : "signed";
        printf("%d  %s  (%s)\n", i + 1, g_repos[i], state);
    }
    return 0;
}

/* Machine-readable listing for front ends (flxpkg):
 *   name TAB repo-version TAB installed-version TAB size TAB description
 * Every repository package, then installed packages no repository has. */
static void tsv_field(const char *s) {
    for (; *s; s++) putchar(*s == '\t' || *s == '\n' ? ' ' : *s);
}

typedef struct { int dummy; } query_ctx_t;

static int query_local_cb(const xpkg_pkg_row_t *row, void *user) {
    (void)user;
    if (xpkg_index_find(row->name)) return 0;
    printf("%s\t\t%s\t0\t", row->name, row->version);
    tsv_field(row->description);
    putchar('\n');
    return 0;
}

int xpkg_cmd_query(void) {
    int have_index = xpkg_index_load(0) == XPKG_OK;
    if (xpkg_db_open() != XPKG_OK) return 1;
    for (int i = 0; have_index && i < g_nidx; i++) {
        const xpkg_entry_t *e = &g_idx[i];
        xpkg_pkg_row_t row;
        int inst = xpkg_db_get(e->name, &row);
        printf("%s\t%s\t%s\t%llu\t", e->name, e->version, inst ? row.version : "", e->size);
        tsv_field(e->description);
        putchar('\n');
    }
    query_ctx_t c;
    xpkg_db_each_package(query_local_cb, &c);
    return 0;
}
