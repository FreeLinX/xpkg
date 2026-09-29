/* main.c - xpkg command line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xpkg.h"

xpkg_opts_t xpkg_opts;

static void usage(FILE *f) {
    fprintf(f,
        "xpkg %s - the FreeLinX package manager\n\n"
        "Usage: xpkg [options] <command> [arguments]\n\n"
        "Software\n"
        "  search <word>            find packages by name or description\n"
        "  show <name>              details of a package in the repositories\n"
        "  install <name|file>...   install packages (and what they need)\n"
        "  reinstall <name|file>... install again over the current version\n"
        "  remove <name>...         remove packages\n"
        "  autoremove               remove dependencies nothing needs any more\n"
        "  update                   refresh the package indexes\n"
        "  upgrade [name...]        refresh, then upgrade everything (or the names)\n"
        "  outdated                 list packages with a newer version available\n\n"
        "Installed packages\n"
        "  list [-e]                installed packages (-e: only ones you asked for)\n"
        "  info <name>              details of an installed package\n"
        "  files <name>             files a package installed\n"
        "  owns <path>              which package a file belongs to\n"
        "  verify [name...]         check installed files against the database\n"
        "  clean                    delete downloaded package files\n\n"
        "Repositories\n"
        "  repo list | repo add <url> | repo remove <url>\n\n"
        "Options\n"
        "  -n, --dry-run            show what would happen, change nothing\n"
        "  -f, --force              override conflicts, dependency and downgrade checks\n"
        "  -q, --quiet              less output      -v, --verbose   list every file\n"
        "  -y, --yes                accepted for compatibility (xpkg never prompts)\n"
        "  --root <dir>             manage the system mounted at <dir>\n"
        "  --allow-unsigned         accept repositories without a valid signature\n"
        "  --no-scripts             do not run package scripts\n"
        "  -V, --version            print the version\n",
        XPKG_VERSION);
}

int main(int argc, char **argv) {
    char *args[1024];
    int nargs = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-n") || !strcmp(a, "--dry-run")) xpkg_opts.dry_run = 1;
        else if (!strcmp(a, "-f") || !strcmp(a, "--force")) xpkg_opts.force = 1;
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) xpkg_opts.quiet = 1;
        else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) xpkg_opts.verbose = 1;
        else if (!strcmp(a, "-y") || !strcmp(a, "--yes") || !strcmp(a, "--noconfirm")) {}
        else if (!strcmp(a, "--allow-unsigned")) xpkg_opts.allow_unsigned = 1;
        else if (!strcmp(a, "--no-scripts")) xpkg_opts.no_scripts = 1;
        else if (!strcmp(a, "--root") && i + 1 < argc) setenv("XPKG_ROOT", argv[++i], 1);
        else if (!strncmp(a, "--root=", 7)) setenv("XPKG_ROOT", a + 7, 1);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help") || !strcmp(a, "help")) { usage(stdout); return 0; }
        else if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("xpkg %s\n", XPKG_VERSION); return 0; }
        else if (!strcmp(a, "-e") && nargs == 1 && !strcmp(args[0], "list")) args[nargs++] = argv[i];
        else if (a[0] == '-' && a[1]) { fprintf(stderr, "xpkg: unknown option %s\n", a); return 2; }
        else if (nargs < 1024) args[nargs++] = argv[i];
    }
    if (nargs == 0) { usage(stderr); return 2; }

    const char *cmd = args[0];
    char **rest = args + 1;
    int nrest = nargs - 1;
    int rc;

#define NEED(k) do { if (nrest < (k)) { fprintf(stderr, "xpkg: '%s' needs an argument (see xpkg --help)\n", cmd); return 2; } } while (0)

    if (!strcmp(cmd, "install") || !strcmp(cmd, "add") || !strcmp(cmd, "in")) { NEED(1); rc = xpkg_cmd_install(rest, nrest, 0); }
    else if (!strcmp(cmd, "reinstall")) { NEED(1); rc = xpkg_cmd_install(rest, nrest, 1); }
    else if (!strcmp(cmd, "remove") || !strcmp(cmd, "rm") || !strcmp(cmd, "del")) { NEED(1); rc = xpkg_cmd_remove(rest, nrest); }
    else if (!strcmp(cmd, "autoremove")) rc = xpkg_cmd_autoremove();
    else if (!strcmp(cmd, "update")) {
        if (xpkg_lock() != 0) return 1;
        rc = xpkg_cmd_update();
    }
    else if (!strcmp(cmd, "upgrade") || !strcmp(cmd, "up")) rc = xpkg_cmd_upgrade(rest, nrest);
    else if (!strcmp(cmd, "upgrade-all")) rc = xpkg_cmd_upgrade(NULL, 0);
    else if (!strcmp(cmd, "outdated")) rc = xpkg_cmd_outdated();
    else if (!strcmp(cmd, "search") || !strcmp(cmd, "find")) rc = xpkg_cmd_search(nrest ? rest[0] : "");
    else if (!strcmp(cmd, "show")) { NEED(1); rc = xpkg_cmd_show(rest[0]); }
    else if (!strcmp(cmd, "query")) rc = xpkg_cmd_query();
    else if (!strcmp(cmd, "list") || !strcmp(cmd, "ls")) rc = xpkg_cmd_list(nrest && !strcmp(rest[0], "-e"));
    else if (!strcmp(cmd, "info")) { NEED(1); rc = xpkg_cmd_info(rest[0]); }
    else if (!strcmp(cmd, "files")) { NEED(1); rc = xpkg_cmd_files(rest[0]); }
    else if (!strcmp(cmd, "owns")) { NEED(1); rc = xpkg_cmd_owns(rest[0]); }
    else if (!strcmp(cmd, "verify")) rc = xpkg_cmd_verify(rest, nrest);
    else if (!strcmp(cmd, "clean")) rc = xpkg_cmd_clean();
    else if (!strcmp(cmd, "repo")) {
        NEED(1);
        if (!strcmp(rest[0], "list")) rc = xpkg_cmd_repo_list();
        else if (!strcmp(rest[0], "add")) {
            NEED(2);
            if (xpkg_require_root() != 0) return 1;
            rc = xpkg_cmd_repo_add(rest[1]);
        } else if (!strcmp(rest[0], "remove") || !strcmp(rest[0], "rm")) {
            NEED(2);
            if (xpkg_require_root() != 0) return 1;
            rc = xpkg_cmd_repo_remove(rest[1]);
        } else { fprintf(stderr, "xpkg: unknown repo command %s\n", rest[0]); return 2; }
    }
    else { fprintf(stderr, "xpkg: unknown command '%s' (see xpkg --help)\n", cmd); return 2; }

    xpkg_db_close();
    xpkg_unlock();
    return rc;
}
