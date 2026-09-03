/* pkginfo.c - parses a package's pkg-info metadata file.
 *
 * Format (plain key=value text, one per line, matching the same style
 * already used by FreeLinX's own /etc/os-release):
 *
 *     NAME=mypackage
 *     VERSION=1.0
 *     DESCRIPTION=Does a thing
 *     ARCH=x86_64
 *     DEPENDS=libfoo,libbar
 *
 * DEPENDS is comma-separated, no spaces. Unknown keys are ignored rather
 * than rejected, so pkg-info can grow new fields later without breaking
 * older xpkg builds parsing newer packages.
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "xpkg.h"

static void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

static void split_depends(xpkg_info_t *out, const char *value) {
    char buf[XPKG_MAX_LINE];
    strncpy(buf, value, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    out->depends_count = 0;
    char *tok = strtok(buf, ",");
    while (tok != NULL && out->depends_count < XPKG_MAX_DEPENDS) {
        while (isspace((unsigned char)*tok)) tok++; /* skip leading space */
        strncpy(out->depends[out->depends_count], tok, XPKG_MAX_NAME - 1);
        out->depends[out->depends_count][XPKG_MAX_NAME - 1] = '\0';
        out->depends_count++;
        tok = strtok(NULL, ",");
    }
}

xpkg_status_t xpkg_parse_pkginfo(const char *path, xpkg_info_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return XPKG_ERR_BAD_PKGINFO;
    }

    memset(out, 0, sizeof(*out));
    strncpy(out->arch, "x86_64", sizeof(out->arch) - 1); /* sensible default */

    char line[XPKG_MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        trim_newline(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue; /* blank line or comment */
        }

        char *eq = strchr(line, '=');
        if (!eq) {
            continue; /* malformed line, skip rather than fail the whole parse */
        }
        *eq = '\0';
        const char *key = line;
        const char *value = eq + 1;

        if (strcmp(key, "NAME") == 0) {
            strncpy(out->name, value, sizeof(out->name) - 1);
        } else if (strcmp(key, "VERSION") == 0) {
            strncpy(out->version, value, sizeof(out->version) - 1);
        } else if (strcmp(key, "DESCRIPTION") == 0) {
            strncpy(out->description, value, sizeof(out->description) - 1);
        } else if (strcmp(key, "ARCH") == 0) {
            strncpy(out->arch, value, sizeof(out->arch) - 1);
        } else if (strcmp(key, "DEPENDS") == 0) {
            split_depends(out, value);
        }
        /* unknown keys: ignored on purpose, see file header comment */
    }

    fclose(f);

    if (out->name[0] == '\0' || out->version[0] == '\0') {
        /* NAME and VERSION are the only truly required fields -- everything
         * else has a sane default or is optional. */
        return XPKG_ERR_BAD_PKGINFO;
    }

    return XPKG_OK;
}
