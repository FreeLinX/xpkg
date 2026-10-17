/* cmd_install.c - install / reinstall / upgrade: plan, download, deploy.
 *
 * A transaction is planned completely before anything is touched: targets
 * and their missing dependencies are resolved from the (signed) indexes into
 * a dependency-first order, the plan and download size are shown, every
 * archive is downloaded and checked against the index, and only then are
 * the packages deployed one by one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "xpkg.h"

#define MAX_PLAN 4096

typedef struct {
    const xpkg_entry_t *e;
    const char *local;      /* local archive path instead of a repo entry */
    xpkg_info_t *linfo;     /* pkg-info of a local archive */
    int explicit_;
    int allow_same;
    char archive[XPKG_MAX_PATH];
} step_t;

static step_t g_plan[MAX_PLAN];
static int g_nplan;
static char g_visiting[MAX_PLAN][XPKG_MAX_NAME];
static int g_nvisiting;

static int plan_find(const char *name) {
    for (int i = 0; i < g_nplan; i++) {
        const char *n = g_plan[i].e ? g_plan[i].e->name : g_plan[i].linfo->name;
        if (!strcmp(n, name)) return i;
    }
    return -1;
}

static int visiting(const char *name) {
    for (int i = 0; i < g_nvisiting; i++) if (!strcmp(g_visiting[i], name)) return 1;
    return 0;
}

static int plan_name(const char *name, int explicit_, int reinstall, int need_index);

/* Dependencies of an entry: from the index, or (old indexes) from the
 * archive itself. */
static int plan_deps(const char *pkg, char deps[][XPKG_MAX_NAME], int ndeps) {
    for (int i = 0; i < ndeps; i++) {
        char dep[XPKG_MAX_NAME];
        xpkg_dep_name(deps[i], dep, sizeof(dep));
        if (!dep[0]) continue;
        if (visiting(dep)) {
            xpkg_err("dependency cycle: %s -> %s", pkg, dep);
            return -1;
        }
        if (plan_name(dep, 0, 0, 1) != 0) {
            xpkg_err("%s needs %s", pkg, dep);
            return -1;
        }
    }
    return 0;
}

static int plan_name(const char *name, int explicit_, int reinstall, int need_index) {
    int at = plan_find(name);
    if (at >= 0) {
        if (explicit_) g_plan[at].explicit_ = 1;
        return 0;
    }
    xpkg_pkg_row_t row;
    int inst = xpkg_db_is_installed(name) && xpkg_db_get(name, &row);
    if (inst && !reinstall) {
        if (explicit_) {
            if (!row.explicit_ && !xpkg_opts.dry_run) xpkg_db_set_explicit(name, 1);
            printf("  %s %s is already installed\n", name, row.version);
        }
        return 0;
    }
    (void)need_index;
    const xpkg_entry_t *e = xpkg_index_find(name);
    if (!e) {
        xpkg_err("no package named '%s' in the repositories (try: xpkg search %s)", name, name);
        return -1;
    }
    if (g_nplan >= MAX_PLAN || g_nvisiting >= MAX_PLAN) {
        xpkg_err("transaction too large");
        return -1;
    }
    snprintf(g_visiting[g_nvisiting++], XPKG_MAX_NAME, "%s", name);
    int rc = 0;
    if (e->has_depends) {
        rc = plan_deps(name, (char (*)[XPKG_MAX_NAME])e->depends, e->depends_count);
    } else {
        /* index without depends: read them from the archive */
        char path[XPKG_MAX_PATH];
        xpkg_info_t info;
        if (xpkg_fetch_entry(e, path, sizeof(path)) != XPKG_OK ||
            xpkg_pkginfo_from_archive(path, &info) != XPKG_OK) rc = -1;
        else rc = plan_deps(name, info.depends, info.depends_count);
    }
    g_nvisiting--;
    if (rc != 0) return rc;
    step_t *s = &g_plan[g_nplan++];
    memset(s, 0, sizeof(*s));
    s->e = e;
    s->explicit_ = explicit_;
    s->allow_same = reinstall;
    return 0;
}

static int looks_like_file(const char *s) {
    size_t l = strlen(s);
    return strchr(s, '/') || (l > 5 && !strcmp(s + l - 5, ".xpkg"));
}

/* Puts every step after the steps it depends on (local archives are added
 * in command-line order, which need not be dependency order). */
static int order_plan(void) {
    static step_t sorted[MAX_PLAN];
    static char placed[MAX_PLAN];
    memset(placed, 0, sizeof(placed));
    int n = 0;
    while (n < g_nplan) {
        int progress = 0;
        for (int i = 0; i < g_nplan; i++) {
            if (placed[i]) continue;
            const step_t *s = &g_plan[i];
            int ndeps = s->e ? s->e->depends_count : s->linfo->depends_count;
            int ready = 1;
            for (int d = 0; d < ndeps && ready; d++) {
                char dep[XPKG_MAX_NAME];
                xpkg_dep_name(s->e ? s->e->depends[d] : s->linfo->depends[d], dep, sizeof(dep));
                int at = plan_find(dep);
                if (at >= 0 && at != i && !placed[at]) ready = 0;
            }
            if (!ready) continue;
            sorted[n++] = *s;
            placed[i] = 1;
            progress = 1;
        }
        if (!progress) {
            xpkg_err("dependency cycle among the packages to install");
            return -1;
        }
    }
    memcpy(g_plan, sorted, (size_t)n * sizeof(*sorted));
    return 0;
}

static int run_plan(void) {
    if (g_nplan && order_plan() != 0) return 1;
    if (g_nplan == 0) {
        if (!xpkg_opts.quiet) printf("nothing to do\n");
        return 0;
    }
    unsigned long long dl = 0;
    printf("\n%sPackages (%d):%s", xpkg_color("\033[1m"), g_nplan, xpkg_color("\033[0m"));
    for (int i = 0; i < g_nplan; i++) {
        if (g_plan[i].e) {
            printf(" %s-%s", g_plan[i].e->name, g_plan[i].e->version);
            dl += g_plan[i].e->size;
        } else {
            printf(" %s-%s", g_plan[i].linfo->name, g_plan[i].linfo->version);
        }
    }
    char hs[32];
    xpkg_human_size(dl, hs, sizeof(hs));
    printf("\n%sDownload size:%s %s\n\n", xpkg_color("\033[1m"), xpkg_color("\033[0m"), hs);
    if (xpkg_opts.dry_run) return 0;

    xpkg_msg("downloading");
    for (int i = 0; i < g_nplan; i++) {
        step_t *s = &g_plan[i];
        if (s->local) { snprintf(s->archive, sizeof(s->archive), "%s", s->local); continue; }
        if (xpkg_fetch_entry(s->e, s->archive, sizeof(s->archive)) != XPKG_OK) return 1;
    }
    xpkg_msg("installing");
    int fails = 0;
    for (int i = 0; i < g_nplan; i++) {
        if (xpkg_deploy_archive(g_plan[i].archive, g_plan[i].explicit_, g_plan[i].allow_same) != XPKG_OK) {
            fails++;
            break;   /* later steps may depend on this one */
        }
    }
    return fails ? 1 : 0;
}

static void plan_reset(void) {
    for (int i = 0; i < g_nplan; i++) free(g_plan[i].linfo);
    g_nplan = 0;
    g_nvisiting = 0;
}

int xpkg_cmd_install(char **names, int n, int reinstall) {
    if (xpkg_require_root() != 0 || xpkg_lock() != 0) return 1;
    if (xpkg_db_open() != XPKG_OK) return 1;
    plan_reset();

    int need_index = 0;
    for (int i = 0; i < n; i++) if (!looks_like_file(names[i])) need_index = 1;
    int have_index = xpkg_index_load(need_index) == XPKG_OK;
    if (need_index && !have_index) return 1;

    int rc = 0;
    /* local archives first, so they can satisfy each other's dependencies */
    for (int i = 0; i < n && rc == 0; i++) {
        if (!looks_like_file(names[i])) continue;
        xpkg_info_t *info = malloc(sizeof(*info));
        if (!info || xpkg_pkginfo_from_archive(names[i], info) != XPKG_OK) {
            xpkg_err("%s is not a valid .xpkg package", names[i]);
            free(info);
            rc = 1;
            break;
        }
        if (plan_find(info->name) >= 0) { free(info); continue; }
        if (g_nplan >= MAX_PLAN) { free(info); xpkg_err("transaction too large"); rc = 1; break; }
        step_t *s = &g_plan[g_nplan++];
        memset(s, 0, sizeof(*s));
        s->local = names[i];
        s->linfo = info;
        s->explicit_ = 1;
        s->allow_same = reinstall;
    }
    int nlocal = g_nplan;
    for (int i = 0; i < nlocal && rc == 0; i++) {
        xpkg_info_t *info = g_plan[i].linfo;
        for (int d = 0; d < info->depends_count && rc == 0; d++) {
            char dep[XPKG_MAX_NAME];
            xpkg_dep_name(info->depends[d], dep, sizeof(dep));
            if (xpkg_db_is_installed(dep) || plan_find(dep) >= 0) continue;
            if (!have_index) {
                xpkg_err("%s needs %s (not installed, and no repository to fetch it from)", info->name, dep);
                rc = 1;
            } else if (plan_name(dep, 0, 0, 1) != 0) rc = 1;
        }
    }
    for (int i = 0; i < n && rc == 0; i++) {
        if (looks_like_file(names[i])) continue;
        if (!xpkg_valid_name(names[i])) { xpkg_err("invalid package name '%s'", names[i]); rc = 1; break; }
        if (plan_name(names[i], 1, reinstall, 1) != 0) rc = 1;
    }
    if (rc == 0) rc = run_plan();
    plan_reset();
    return rc;
}

typedef struct { char **names; int n; int rc; } up_ctx_t;

static int want(up_ctx_t *c, const char *name) {
    if (!c->n) return 1;
    for (int i = 0; i < c->n; i++) if (!strcmp(c->names[i], name)) return 1;
    return 0;
}

static int upgrade_cb(const xpkg_pkg_row_t *row, void *user) {
    up_ctx_t *c = user;
    if (!want(c, row->name)) return 0;
    const xpkg_entry_t *e = xpkg_index_find(row->name);
    if (!e || xpkg_version_cmp(e->version, row->version) <= 0) return 0;
    if (plan_find(row->name) >= 0) return 0;
    snprintf(g_visiting[g_nvisiting++], XPKG_MAX_NAME, "%s", row->name);
    int rc = e->has_depends ? plan_deps(row->name, (char (*)[XPKG_MAX_NAME])e->depends, e->depends_count) : 0;
    g_nvisiting--;
    if (rc != 0) { c->rc = 1; return 1; }
    if (g_nplan >= MAX_PLAN) { c->rc = 1; return 1; }
    step_t *s = &g_plan[g_nplan++];
    memset(s, 0, sizeof(*s));
    s->e = e;
    s->explicit_ = row->explicit_;
    return 0;
}

int xpkg_cmd_upgrade(char **names, int n) {
    if (xpkg_require_root() != 0 || xpkg_lock() != 0) return 1;
    if (xpkg_db_open() != XPKG_OK) return 1;
    if (!xpkg_opts.dry_run && xpkg_cmd_update() != 0)
        xpkg_warn("some repositories could not be refreshed; using the cached indexes");
    if (xpkg_index_load(1) != XPKG_OK) return 1;
    for (int i = 0; i < n; i++)
        if (!xpkg_db_is_installed(names[i])) { xpkg_err("%s is not installed", names[i]); return 1; }
    plan_reset();
    up_ctx_t c = { names, n, 0 };
    xpkg_db_each_package(upgrade_cb, &c);
    int rc = c.rc ? 1 : run_plan();
    plan_reset();
    return rc;
}
