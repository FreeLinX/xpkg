/* version.c - version string comparison.
 *
 * Compares dotted/numeric version strings so xpkg can decide whether an
 * upgrade is actually newer ("1.10" beats "1.9", "2.9.2" < "2.10.0").
 * '.' '-', and '_' all separate components; components that are purely
 * numeric are compared numerically, everything else lexically. A shorter
 * string is older when all its components match ("1.0" < "1.0.1").
 */
#include <string.h>
#include "xpkg.h"

static int is_sep(char c) {
    return c == '.' || c == '-' || c == '_';
}

static int is_digits(const char *s, const char *end) {
    for (const char *p = s; p < end; p++) {
        if (*p < '0' || *p > '9') return 0;
    }
    return 1;
}

/* Compare two numeric component substrings ('007' == '7'). */
static int num_cmp(const char *a, const char *b, size_t na, size_t nb) {
    while (na > 0 && a[0] == '0') { a++; na--; }
    while (nb > 0 && b[0] == '0') { b++; nb--; }
    if (na != nb) return na < nb ? -1 : 1;
    return memcmp(a, b, na);
}

int xpkg_version_cmp(const char *a, const char *b) {
    const char *pa = a, *pb = b;

    for (;;) {
        while (*pa && is_sep(*pa)) pa++;
        while (*pb && is_sep(*pb)) pb++;

        if (!*pa && !*pb) return 0;
        if (!*pa) return -1;
        if (!*pb) return 1;

        const char *ta = pa;
        while (*ta && !is_sep(*ta)) ta++;
        const char *tb = pb;
        while (*tb && !is_sep(*tb)) tb++;

        int c;
        if (is_digits(pa, ta) && is_digits(pb, tb)) {
            c = num_cmp(pa, pb, (size_t)(ta - pa), (size_t)(tb - pb));
        } else {
            size_t na = (size_t)(ta - pa), nb = (size_t)(tb - pb);
            size_t m = na < nb ? na : nb;
            c = memcmp(pa, pb, m);
            if (c == 0) c = na < nb ? -1 : (na > nb ? 1 : 0);
        }
        if (c) return c;

        pa = ta;
        pb = tb;
    }
}