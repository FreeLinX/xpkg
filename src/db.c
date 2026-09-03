/* db.c - the installed-package database, backed by SQLite.
 *
 * Schema (see docs/DATABASE.md for the full write-up):
 *
 *     CREATE TABLE packages (
 *         name         TEXT PRIMARY KEY,
 *         version      TEXT NOT NULL,
 *         description  TEXT,
 *         installed_at INTEGER NOT NULL
 *     );
 *
 *     CREATE TABLE files (
 *         package_name TEXT NOT NULL,
 *         path         TEXT NOT NULL,
 *         sha256       TEXT NOT NULL,
 *         FOREIGN KEY(package_name) REFERENCES packages(name)
 *     );
 *
 * Every function here opens its own sqlite3 connection to XPKG_DB_PATH and
 * closes it before returning. SQLite connections are cheap to open, and
 * this keeps every command in cmd_*.c simple (no connection object to
 * thread through call chains) at negligible cost for a package manager
 * that isn't handling high call volume.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include "xpkg.h"

static xpkg_status_t open_db(sqlite3 **db) {
    /* XPKG_DB_DIR must exist before sqlite3_open can create the db file
     * inside it. mkdir failing because the directory already exists is
     * fine and expected on every call after the first; any other mkdir
     * failure will surface naturally when sqlite3_open itself fails. */
    mkdir(XPKG_DB_DIR, 0755);

    if (sqlite3_open(XPKG_DB_PATH, db) != SQLITE_OK) {
        fprintf(stderr, "xpkg: cannot open database at %s: %s\n",
                XPKG_DB_PATH, sqlite3_errmsg(*db));
        sqlite3_close(*db);
        return XPKG_ERR_DB;
    }
    return XPKG_OK;
}

xpkg_status_t xpkg_db_init(void) {
    sqlite3 *db;
    xpkg_status_t st = open_db(&db);
    if (st != XPKG_OK) return st;

    const char *schema =
        "CREATE TABLE IF NOT EXISTS packages ("
        "  name TEXT PRIMARY KEY,"
        "  version TEXT NOT NULL,"
        "  description TEXT,"
        "  installed_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS files ("
        "  package_name TEXT NOT NULL,"
        "  path TEXT NOT NULL,"
        "  sha256 TEXT NOT NULL,"
        "  FOREIGN KEY(package_name) REFERENCES packages(name)"
        ");";

    char *errmsg = NULL;
    int rc = sqlite3_exec(db, schema, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "xpkg: schema init failed: %s\n", errmsg);
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return XPKG_ERR_DB;
    }

    sqlite3_close(db);
    return XPKG_OK;
}

xpkg_status_t xpkg_db_is_installed(const char *pkg_name, int *out_installed) {
    sqlite3 *db;
    xpkg_status_t st = open_db(&db);
    if (st != XPKG_OK) return st;

    sqlite3_stmt *stmt;
    const char *sql = "SELECT 1 FROM packages WHERE name = ? LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return XPKG_ERR_DB;
    }
    sqlite3_bind_text(stmt, 1, pkg_name, -1, SQLITE_STATIC);

    *out_installed = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 0;

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return XPKG_OK;
}

xpkg_status_t xpkg_db_register_package(const xpkg_info_t *info) {
    sqlite3 *db;
    xpkg_status_t st = open_db(&db);
    if (st != XPKG_OK) return st;

    sqlite3_stmt *stmt;
    const char *sql =
        "INSERT INTO packages (name, version, description, installed_at) "
        "VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "xpkg: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return XPKG_ERR_DB;
    }

    sqlite3_bind_text(stmt, 1, info->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, info->version, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, info->description, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 4, (sqlite3_int64)time(NULL));

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return (rc == SQLITE_DONE) ? XPKG_OK : XPKG_ERR_DB;
}

xpkg_status_t xpkg_db_register_file(const char *pkg_name, const char *path, const char *sha256) {
    sqlite3 *db;
    xpkg_status_t st = open_db(&db);
    if (st != XPKG_OK) return st;

    sqlite3_stmt *stmt;
    const char *sql =
        "INSERT INTO files (package_name, path, sha256) VALUES (?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return XPKG_ERR_DB;
    }

    sqlite3_bind_text(stmt, 1, pkg_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, sha256, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return (rc == SQLITE_DONE) ? XPKG_OK : XPKG_ERR_DB;
}

xpkg_status_t xpkg_db_remove_package(const char *pkg_name) {
    sqlite3 *db;
    xpkg_status_t st = open_db(&db);
    if (st != XPKG_OK) return st;

    sqlite3_stmt *stmt;

    /* Delete files first (no cascading delete configured -- keep it
     * explicit and simple rather than relying on SQLite foreign-key
     * pragmas that may not be enabled by default). */
    if (sqlite3_prepare_v2(db, "DELETE FROM files WHERE package_name = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, pkg_name, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    if (sqlite3_prepare_v2(db, "DELETE FROM packages WHERE name = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, pkg_name, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    return XPKG_OK;
}

xpkg_status_t xpkg_db_list(void) {
    sqlite3 *db;
    xpkg_status_t st = open_db(&db);
    if (st != XPKG_OK) return st;

    sqlite3_stmt *stmt;
    const char *sql = "SELECT name, version FROM packages ORDER BY name;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return XPKG_ERR_DB;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%s %s\n",
               sqlite3_column_text(stmt, 0),
               sqlite3_column_text(stmt, 1));
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return XPKG_OK;
}

xpkg_status_t xpkg_db_info(const char *pkg_name) {
    sqlite3 *db;
    xpkg_status_t st = open_db(&db);
    if (st != XPKG_OK) return st;

    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT name, version, description, installed_at "
        "FROM packages WHERE name = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return XPKG_ERR_DB;
    }
    sqlite3_bind_text(stmt, 1, pkg_name, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("Name:        %s\n", sqlite3_column_text(stmt, 0));
        printf("Version:     %s\n", sqlite3_column_text(stmt, 1));
        printf("Description: %s\n", sqlite3_column_text(stmt, 2));
        printf("Installed:   %lld\n", (long long)sqlite3_column_int64(stmt, 3));
    } else {
        fprintf(stderr, "xpkg: package not installed: %s\n", pkg_name);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return XPKG_ERR_NOT_FOUND;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return XPKG_OK;
}

xpkg_status_t xpkg_db_files(const char *pkg_name) {
    sqlite3 *db;
    xpkg_status_t st = open_db(&db);
    if (st != XPKG_OK) return st;

    sqlite3_stmt *stmt;
    const char *sql = "SELECT path FROM files WHERE package_name = ? ORDER BY path;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return XPKG_ERR_DB;
    }
    sqlite3_bind_text(stmt, 1, pkg_name, -1, SQLITE_STATIC);

    int any = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        printf("%s\n", sqlite3_column_text(stmt, 0));
        any = 1;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (!any) {
        fprintf(stderr, "xpkg: no files recorded for %s (installed?)\n", pkg_name);
        return XPKG_ERR_NOT_FOUND;
    }
    return XPKG_OK;
}
