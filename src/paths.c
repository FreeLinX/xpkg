/* paths.c - resolved filesystem locations for xpkg.
 *
 * Every hardcoded default above (/etc/xpkg, /var/lib/xpkg, /var/cache/xpkg)
 * can be redirected at runtime via XPKG_CONFIG_DIR / XPKG_DB_DIR /
 * XPKG_CACHE_DIR. On a real FreeLinX device nothing sets these, so the
 * defaults apply and behaviour is identical to v1; the overrides exist so
 * the host-side headless test suite can run as a non-root user against a
 * tmpdir without touching system paths.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xpkg.h"

static const char *env_or(const char *key, const char *def) {
    const char *e = getenv(key);
    return (e && e[0]) ? e : def;
}

const char *xpkg_db_dir(void) {
    return env_or("XPKG_DB_DIR", XPKG_DB_DIR_DEFAULT);
}

const char *xpkg_db_path(void) {
    static char buf[XPKG_MAX_PATH];
    static int init = 0;
    if (!init) {
        snprintf(buf, sizeof(buf), "%s/xpkg.db", xpkg_db_dir());
        init = 1;
    }
    return buf;
}

const char *xpkg_cache_dir(void) {
    return env_or("XPKG_CACHE_DIR", XPKG_CACHE_DIR_DEFAULT);
}

const char *xpkg_config_dir(void) {
    return env_or("XPKG_CONFIG_DIR", XPKG_CONFIG_DIR_DEFAULT);
}

const char *xpkg_repos_conf(void) {
    static char buf[XPKG_MAX_PATH];
    static int init = 0;
    if (!init) {
        snprintf(buf, sizeof(buf), "%s/repos.conf", xpkg_config_dir());
        init = 1;
    }
    return buf;
}

/* Install destination root. Empty string on a real device (files go
 * straight under /); the host-side tests set XPKG_ROOT to a tmpdir so the
 * non-root test user can exercise full install/remove/upgrade flows. */
const char *xpkg_root(void) {
    return env_or("XPKG_ROOT", "");
}