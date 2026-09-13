/* main.c - xpkg entry point: argument parsing and command dispatch.
 *
 * Command set:
 *     xpkg install <file.xpkg>    install from a local .xpkg file
 *     xpkg install <name>         fetch <name> (and its DEPENDS) from a repo
 *     xpkg remove <name>          remove an installed package
 *     xpkg list                   list installed packages
 *     xpkg info <name>            show details for an installed package
 *     xpkg files <name>           list files owned by an installed package
 *     xpkg verify <name>          re-hash installed files, report changes
 *     xpkg update                 refresh the cached repo index
 *     xpkg upgrade <name>         upgrade one installed package
 *     xpkg upgrade-all            upgrade every installed package with a newer
 *                                 version available in a repo
 *     xpkg repo add/remove/list   manage configured repos
 */
#include <stdio.h>
#include <string.h>
#include "xpkg.h"

static void usage(const char *argv0) {
    fprintf(stderr,
        "xpkg %s - FreeLinX package manager\n\n"
        "Usage:\n"
        "  %s install <file.xpkg>   Install from a local .xpkg file\n"
        "  %s install <name>        Fetch and install <name> from a repo (deps auto)\n"
        "  %s remove <name>         Remove an installed package\n"
        "  %s list                  List installed packages\n"
        "  %s info <name>           Show details for an installed package\n"
        "  %s files <name>          List files owned by an installed package\n"
        "  %s verify <name>         Re-hash installed files, report changes\n"
        "  %s update                Refresh the cached repo indexes\n"
        "  %s upgrade <name>        Upgrade one installed package\n"
        "  %s upgrade-all           Upgrade all installed packages\n"
        "  %s repo add <url>        Add a package repo\n"
        "  %s repo remove <url>     Remove a package repo\n"
        "  %s repo list             List configured repos\n",
        XPKG_VERSION,
        argv0, argv0, argv0, argv0, argv0, argv0, argv0, argv0,
        argv0, argv0, argv0, argv0, argv0);
}

/* A "name" (repo fetch) has no path separators and no .xpkg suffix. */
static int looks_like_name(const char *s) {
    if (strchr(s, '/') || strchr(s, '\\')) {
        return 0;
    }
    size_t len = strlen(s);
    if (len >= 5 && strcmp(s + len - 5, ".xpkg") == 0) {
        return 0;
    }
    return 1;
}

static int cmd_install(const char *arg) {
    if (looks_like_name(arg)) {
        return xpkg_cmd_install_repo(arg);
    }
    return xpkg_cmd_install(arg);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[1];

    if (strcmp(cmd, "install") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return cmd_install(argv[2]);
    }
    if (strcmp(cmd, "remove") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return xpkg_cmd_remove(argv[2]);
    }
    if (strcmp(cmd, "list") == 0) {
        return xpkg_cmd_list();
    }
    if (strcmp(cmd, "info") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return xpkg_cmd_info(argv[2]);
    }
    if (strcmp(cmd, "files") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return xpkg_cmd_files(argv[2]);
    }
    if (strcmp(cmd, "verify") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return xpkg_cmd_verify(argv[2]);
    }
    if (strcmp(cmd, "update") == 0) {
        return xpkg_cmd_update();
    }
    if (strcmp(cmd, "upgrade") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        return xpkg_cmd_upgrade(argv[2]);
    }
    if (strcmp(cmd, "upgrade-all") == 0) {
        return xpkg_cmd_upgrade_all();
    }
    if (strcmp(cmd, "repo") == 0) {
        if (argc < 3) { usage(argv[0]); return 1; }
        if (strcmp(argv[2], "add") == 0) {
            if (argc < 4) { usage(argv[0]); return 1; }
            return xpkg_cmd_repo_add(argv[3]);
        }
        if (strcmp(argv[2], "remove") == 0) {
            if (argc < 4) { usage(argv[0]); return 1; }
            return xpkg_cmd_repo_remove(argv[3]);
        }
        if (strcmp(argv[2], "list") == 0) {
            return xpkg_cmd_repo_list();
        }
        fprintf(stderr, "xpkg: unknown repo subcommand: %s\n", argv[2]);
        usage(argv[0]);
        return 1;
    }
    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage(argv[0]);
        return 0;
    }

    fprintf(stderr, "xpkg: unknown command: %s\n", cmd);
    usage(argv[0]);
    return 1;
}