/* paths.c - resolved filesystem locations for xpkg.
 *
 * Defaults are /etc/xpkg, /var/lib/xpkg and /var/cache/xpkg.  XPKG_ROOT
 * moves all of them (and the install destination) under another root, which
 * is how a system being installed onto a mounted disk is managed.  The
 * individual XPKG_CONFIG_DIR / XPKG_DB_DIR / XPKG_CACHE_DIR overrides win
 * over that (the host-side tests use them).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xpkg.h"

static const char *env_or_root(const char *key, const char *def, char *buf, size_t n) {
    const char *e = getenv(key);
    if (e && e[0]) return e;
    snprintf(buf, n, "%s%s", xpkg_root(), def);
    return buf;
}

const char *xpkg_root(void) {
    static char buf[XPKG_MAX_PATH];
    static int init = 0;
    if (!init) {
        const char *e = getenv("XPKG_ROOT");
        snprintf(buf, sizeof(buf), "%s", e ? e : "");
        size_t l = strlen(buf);
        while (l > 0 && buf[l - 1] == '/') buf[--l] = '\0';
        init = 1;
    }
    return buf;
}

const char *xpkg_db_dir(void) {
    static char buf[XPKG_MAX_PATH];
    return env_or_root("XPKG_DB_DIR", XPKG_DB_DIR_DEFAULT, buf, sizeof(buf));
}

const char *xpkg_db_path(void) {
    static char buf[XPKG_MAX_PATH];
    snprintf(buf, sizeof(buf), "%s/xpkg.db", xpkg_db_dir());
    return buf;
}

const char *xpkg_scripts_dir(void) {
    static char buf[XPKG_MAX_PATH];
    snprintf(buf, sizeof(buf), "%s/scripts", xpkg_db_dir());
    return buf;
}

const char *xpkg_cache_dir(void) {
    static char buf[XPKG_MAX_PATH];
    return env_or_root("XPKG_CACHE_DIR", XPKG_CACHE_DIR_DEFAULT, buf, sizeof(buf));
}

const char *xpkg_config_dir(void) {
    static char buf[XPKG_MAX_PATH];
    return env_or_root("XPKG_CONFIG_DIR", XPKG_CONFIG_DIR_DEFAULT, buf, sizeof(buf));
}

const char *xpkg_repos_conf(void) {
    static char buf[XPKG_MAX_PATH];
    snprintf(buf, sizeof(buf), "%s/repos.conf", xpkg_config_dir());
    return buf;
}

const char *xpkg_keys_dir(void) {
    static char buf[XPKG_MAX_PATH];
    snprintf(buf, sizeof(buf), "%s/keys", xpkg_config_dir());
    return buf;
}
