/* repo.c - repo index + package fetching for `xpkg install <name>` (v2).
 *
 * A repo is a directory of *.xpkg files plus an index.json, served over
 * HTTP(S). repos.conf (default /etc/xpkg/repos.conf) lists one base repo
 * URL per line; each line's index.json is at <url>/index.json and each
 * package's archive at <url>/<file> (the "file" field of its index entry).
 *
 * index.json layout (written by tools/xpkg-create.c):
 *   { "packages": { "<name>": {
 *     "version": "1.0", "description": "...", "arch": "x86_64",
 *     "file": "<name>-<ver>.xpkg", "size": 12345, "sha256": "<hex>"
 *   } } }
 *
 * Resolution order per package name: for each repo (in repos.conf order),
 * fetch its index.json; use the first repo that lists the name. Dependencies
 * (DEPENDS in pkg-info) are installed before the requested package when they
 * are available from the same repo and not already installed.
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

/* --- repos.conf -------------------------------------------------------- */

/* Returns number of repos read (0 if file absent/empty), each as a base URL
 * (trailing slash stripped) in `repos`. */
static int repos_load(char repos[][XPKG_MAX_PATH], int max) {
    FILE *f = fopen(XPKG_REPOS_CONF, "r");
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
        /* strip trailing slashes and whitespace */
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
    mkdir(XPKG_CONFIG_DIR, 0755);
    FILE *f = fopen(XPKG_REPOS_CONF, "w");
    if (!f) return;
    for (int i = 0; i < n; i++) {
        fprintf(f, "%s\n", repos[i]);
    }
    fclose(f);
}

/* --- tiny JSON field extraction ---------------------------------------- */

/* Given a buffer containing index.json, find the value of a package field.
 * `name` is the package key; `field` the field key. Returns the field value
 * (may be empty) in out. Handles the specific flat structure xpkg-create
 * emits, without pulling in a full JSON library. */
static int json_pkg_field(const char *json, const char *name,
                          const char *field, char *out, size_t outsz) {
    out[0] = '\0';
    /* find the package object: "pkgkey" then later "{", then "field": value */
    char keypat[256];
    snprintf(keypat, sizeof(keypat), "\"%s\"", name);
    const char *pk = strstr(json, keypat);
    if (!pk) return -1;
    /* find the '{' for this package (next '{' after the key) */
    const char *brace = strchr(pk, '{');
    if (!brace) return -1;
    /* search within this object for the field key, bounded by the close brace */
    const char *end = strchr(brace, '}');
    if (!end) return -1;

    char fpat[128];
    snprintf(fpat, sizeof(fpat), "\"%s\"", field);
    const char *pos = brace;
    while (pos < end) {
        const char *f = strstr(pos, fpat);
        if (!f || f >= end) break;
        /* must be followed by ':' */
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
                /* numeric/other value: read until comma/brace */
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

/* --- fetch a package by name from the configured repos ------------------ */

static int repo_load_index(const char *repo, const char *dest) {
    char url[XPKG_MAX_PATH];
    snprintf(url, sizeof(url), "%s/index.json", repo);
    return xpkg_net_get(url, dest) == XPKG_OK ? 0 : -1;
}

int xpkg_cmd_install_repo(const char *name) {
    char repos[MAX_REPOS][XPKG_MAX_PATH];
    int nrepos = repos_load(repos, MAX_REPOS);
    if (nrepos == 0) {
        fprintf(stderr, "xpkg: no repos configured (%s)\n", XPKG_REPOS_CONF);
        fprintf(stderr, "xpkg: add one with: xpkg repo add <url>\n");
        return 1;
    }

    mkdir(XPKG_CACHE_DIR, 0755);
    char index_path[XPKG_MAX_PATH];
    snprintf(index_path, sizeof(index_path), "%s/index.json", XPKG_CACHE_DIR);

    /* 1) find which repo has the package */
    const char *chosen_repo = NULL;
    char pfile[XPKG_MAX_PATH] = {0};
    char psha[65] = {0};
    for (int i = 0; i < nrepos; i++) {
        if (repo_load_index(repos[i], index_path) != 0) {
            continue;
        }
        FILE *f = fopen(index_path, "r");
        if (!f) continue;
        /* read whole file to a heap buffer (indexes are small) */
        long sz;
        fseek(f, 0, SEEK_END);
        sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *buf = sz > 0 ? malloc((size_t)sz + 1) : NULL;
        if (buf) {
            size_t got = fread(buf, 1, (size_t)sz, f);
            buf[got] = '\0';
            char tmp[XPKG_MAX_PATH];
            if (json_pkg_field(buf, name, "file", tmp, sizeof(tmp)) == 0) {
                strncpy(pfile, tmp, sizeof(pfile) - 1);
                json_pkg_field(buf, name, "sha256", psha, sizeof(psha));
                chosen_repo = repos[i];
            }
            free(buf);
        }
        fclose(f);
        if (chosen_repo) break;
    }

    if (!chosen_repo) {
        fprintf(stderr, "xpkg: package not found in any repository: %s\n", name);
        return 1;
    }

    /* 2) download the .xpkg to cache */
    char url[XPKG_MAX_PATH];
    snprintf(url, sizeof(url), "%s/%s", chosen_repo, pfile);
    char pkg_path[XPKG_MAX_PATH];
    snprintf(pkg_path, sizeof(pkg_path), "%s/%s", XPKG_CACHE_DIR, pfile);

    printf("xpkg: fetching %s\n", url);
    if (xpkg_net_get(url, pkg_path) != XPKG_OK) {
        fprintf(stderr, "xpkg: failed to download %s\n", url);
        return 1;
    }

    /* 3) verify sha256 from the index */
    if (psha[0]) {
        char actual[65];
        if (xpkg_sha256_file(pkg_path, actual) == XPKG_OK &&
            strcasecmp(actual, psha) != 0) {
            fprintf(stderr, "xpkg: sha256 mismatch for %s\n", pfile);
            unlink(pkg_path);
            return 1;
        }
    }

    /* 4) install from the local cached file (existing v1 install logic) */
    return xpkg_cmd_install(pkg_path);
}

/* --- repo add/remove/list ---------------------------------------------- */

int xpkg_cmd_repo_add(const char *url) {
    char repos[MAX_REPOS][XPKG_MAX_PATH];
    int n = repos_load(repos, MAX_REPOS);
    /* avoid duplicates */
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
