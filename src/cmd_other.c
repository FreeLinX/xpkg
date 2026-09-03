/* cmd_remove.c, cmd_list.c, cmd_info.c, cmd_files.c, cmd_verify.c
 * combined into one file for now, since each is small; split out once
 * any of them grows enough to warrant its own file.
 */
#include <stdio.h>
#include <string.h>
#include <sqlite3.h>
#include "xpkg.h"

int xpkg_cmd_remove(const char *name) {
    int installed;
    xpkg_db_is_installed(name, &installed);
    if (!installed) {
        fprintf(stderr, "xpkg: %s is not installed\n", name);
        return 1;
    }

    /* Delete every file this package owns, using the same connection
     * pattern as db.c: open, query, close. Kept here rather than in db.c
     * since actually unlinking files is a filesystem operation, not a
     * database one -- db.c only owns the sqlite schema/queries. */
    sqlite3 *db;
    if (sqlite3_open(XPKG_DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "xpkg: cannot open database\n");
        return 1;
    }

    sqlite3_stmt *stmt;
    const char *sql = "SELECT path FROM files WHERE package_name = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(stmt, 0);
            printf("  removing %s\n", path);
            remove(path);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);

    xpkg_db_remove_package(name);
    printf("xpkg: %s removed\n", name);
    return 0;
}

int xpkg_cmd_list(void) {
    return (xpkg_db_list() == XPKG_OK) ? 0 : 1;
}

int xpkg_cmd_info(const char *name) {
    return (xpkg_db_info(name) == XPKG_OK) ? 0 : 1;
}

int xpkg_cmd_files(const char *name) {
    return (xpkg_db_files(name) == XPKG_OK) ? 0 : 1;
}

int xpkg_cmd_verify(const char *name) {
    int installed;
    xpkg_db_is_installed(name, &installed);
    if (!installed) {
        fprintf(stderr, "xpkg: %s is not installed\n", name);
        return 1;
    }

    sqlite3 *db;
    if (sqlite3_open(XPKG_DB_PATH, &db) != SQLITE_OK) {
        fprintf(stderr, "xpkg: cannot open database\n");
        return 1;
    }

    sqlite3_stmt *stmt;
    const char *sql = "SELECT path, sha256 FROM files WHERE package_name = ?;";
    int ok = 1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(stmt, 0);
            const char *expected = (const char *)sqlite3_column_text(stmt, 1);

            char actual[65];
            if (xpkg_sha256_file(path, actual) != XPKG_OK) {
                printf("MISSING  %s\n", path);
                ok = 0;
                continue;
            }
            if (strcmp(actual, expected) != 0) {
                printf("MODIFIED %s\n", path);
                ok = 0;
            } else {
                printf("OK       %s\n", path);
            }
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);

    return ok ? 0 : 1;
}
