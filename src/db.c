/* db.c - the installed-package database (SQLite).
 *
 *   packages(name PRIMARY KEY, version, description, installed_at,
 *            explicit)          explicit = 1 when the user asked for it,
 *                               0 when it came in as a dependency
 *   files(package_name, path, sha256)   one row per file/symlink owned
 *   depends(package_name, dep)          declared dependencies
 *
 * One connection per process; every package change runs inside a
 * transaction so a crash never leaves a half-registered package.  Older
 * (v1) databases are migrated in place on open.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include "xpkg.h"

#define DB_SCHEMA_VERSION 2

static sqlite3 *g_db;

static int exec(const char *sql) {
    char *msg = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &msg) != SQLITE_OK) {
        xpkg_err("database: %s", msg ? msg : "error");
        sqlite3_free(msg);
        return -1;
    }
    return 0;
}

static int user_version(void) {
    sqlite3_stmt *s;
    int v = 0;
    if (sqlite3_prepare_v2(g_db, "PRAGMA user_version;", -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    return v;
}

static int has_column(const char *table, const char *col) {
    char sql[128];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);
    sqlite3_stmt *s;
    int found = 0;
    if (sqlite3_prepare_v2(g_db, sql, -1, &s, NULL) == SQLITE_OK) {
        while (sqlite3_step(s) == SQLITE_ROW)
            if (!strcmp((const char *)sqlite3_column_text(s, 1), col)) found = 1;
        sqlite3_finalize(s);
    }
    return found;
}

xpkg_status_t xpkg_db_open(void) {
    if (g_db) return XPKG_OK;
    if (xpkg_mkdir_p(xpkg_db_dir(), 0755) != 0) {
        xpkg_err("cannot create %s", xpkg_db_dir());
        return XPKG_ERR_DB;
    }
    if (sqlite3_open(xpkg_db_path(), &g_db) != SQLITE_OK) {
        xpkg_err("cannot open database %s: %s", xpkg_db_path(), sqlite3_errmsg(g_db));
        sqlite3_close(g_db);
        g_db = NULL;
        return XPKG_ERR_DB;
    }
    sqlite3_busy_timeout(g_db, 10000);
    if (exec("PRAGMA foreign_keys=OFF; PRAGMA synchronous=FULL;") != 0) return XPKG_ERR_DB;
    if (exec("CREATE TABLE IF NOT EXISTS packages ("
             "  name TEXT PRIMARY KEY, version TEXT NOT NULL, description TEXT,"
             "  installed_at INTEGER NOT NULL, explicit INTEGER NOT NULL DEFAULT 1);"
             "CREATE TABLE IF NOT EXISTS files ("
             "  package_name TEXT NOT NULL, path TEXT NOT NULL, sha256 TEXT NOT NULL);"
             "CREATE TABLE IF NOT EXISTS depends ("
             "  package_name TEXT NOT NULL, dep TEXT NOT NULL);") != 0)
        return XPKG_ERR_DB;
    if (user_version() < DB_SCHEMA_VERSION) {
        if (!has_column("packages", "explicit") &&
            exec("ALTER TABLE packages ADD COLUMN explicit INTEGER NOT NULL DEFAULT 1;") != 0)
            return XPKG_ERR_DB;
        if (exec("CREATE INDEX IF NOT EXISTS files_path ON files(path);"
                 "CREATE INDEX IF NOT EXISTS files_pkg ON files(package_name);"
                 "CREATE INDEX IF NOT EXISTS depends_pkg ON depends(package_name);"
                 "CREATE INDEX IF NOT EXISTS depends_dep ON depends(dep);"
                 "PRAGMA user_version=2;") != 0)
            return XPKG_ERR_DB;
    }
    return XPKG_OK;
}

void xpkg_db_close(void) {
    if (g_db) sqlite3_close(g_db);
    g_db = NULL;
}

xpkg_status_t xpkg_db_begin(void) {
    return exec("BEGIN IMMEDIATE;") == 0 ? XPKG_OK : XPKG_ERR_DB;
}

xpkg_status_t xpkg_db_commit(void) {
    return exec("COMMIT;") == 0 ? XPKG_OK : XPKG_ERR_DB;
}

void xpkg_db_rollback(void) {
    sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
}

static sqlite3_stmt *prep(const char *sql) {
    sqlite3_stmt *s = NULL;
    if (xpkg_db_open() != XPKG_OK) return NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &s, NULL) != SQLITE_OK) {
        xpkg_err("database: %s", sqlite3_errmsg(g_db));
        return NULL;
    }
    return s;
}

static void copy_col(sqlite3_stmt *s, int i, char *out, size_t n) {
    const char *t = (const char *)sqlite3_column_text(s, i);
    snprintf(out, n, "%s", t ? t : "");
}

int xpkg_db_get(const char *name, xpkg_pkg_row_t *out) {
    sqlite3_stmt *s = prep("SELECT name, version, description, installed_at, explicit "
                           "FROM packages WHERE name = ?;");
    if (!s) return 0;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    int found = sqlite3_step(s) == SQLITE_ROW;
    if (found && out) {
        copy_col(s, 0, out->name, sizeof(out->name));
        copy_col(s, 1, out->version, sizeof(out->version));
        copy_col(s, 2, out->description, sizeof(out->description));
        out->installed_at = sqlite3_column_int64(s, 3);
        out->explicit_ = sqlite3_column_int(s, 4);
    }
    sqlite3_finalize(s);
    return found;
}

int xpkg_db_is_installed(const char *name) {
    return xpkg_db_get(name, NULL);
}

static xpkg_status_t step_done(sqlite3_stmt *s) {
    int rc = sqlite3_step(s);
    if (rc != SQLITE_DONE) xpkg_err("database: %s", sqlite3_errmsg(g_db));
    sqlite3_finalize(s);
    return rc == SQLITE_DONE ? XPKG_OK : XPKG_ERR_DB;
}

xpkg_status_t xpkg_db_put_package(const xpkg_info_t *info, int explicit_) {
    sqlite3_stmt *s = prep("INSERT INTO packages (name, version, description, installed_at, explicit) "
                           "VALUES (?1, ?2, ?3, ?4, ?5) ON CONFLICT(name) DO UPDATE SET "
                           "version = ?2, description = ?3, installed_at = ?4, "
                           "explicit = MAX(explicit, ?5);");
    if (!s) return XPKG_ERR_DB;
    sqlite3_bind_text(s, 1, info->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, info->version, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, info->description, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 4, (sqlite3_int64)time(NULL));
    sqlite3_bind_int(s, 5, explicit_ ? 1 : 0);
    if (step_done(s) != XPKG_OK) return XPKG_ERR_DB;

    s = prep("DELETE FROM depends WHERE package_name = ?;");
    if (!s) return XPKG_ERR_DB;
    sqlite3_bind_text(s, 1, info->name, -1, SQLITE_STATIC);
    if (step_done(s) != XPKG_OK) return XPKG_ERR_DB;
    for (int i = 0; i < info->depends_count; i++) {
        char dep[XPKG_MAX_NAME];
        xpkg_dep_name(info->depends[i], dep, sizeof(dep));
        s = prep("INSERT INTO depends (package_name, dep) VALUES (?, ?);");
        if (!s) return XPKG_ERR_DB;
        sqlite3_bind_text(s, 1, info->name, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, dep, -1, SQLITE_TRANSIENT);
        if (step_done(s) != XPKG_OK) return XPKG_ERR_DB;
    }
    return XPKG_OK;
}

xpkg_status_t xpkg_db_set_explicit(const char *name, int explicit_) {
    sqlite3_stmt *s = prep("UPDATE packages SET explicit = ? WHERE name = ?;");
    if (!s) return XPKG_ERR_DB;
    sqlite3_bind_int(s, 1, explicit_);
    sqlite3_bind_text(s, 2, name, -1, SQLITE_STATIC);
    return step_done(s);
}

xpkg_status_t xpkg_db_delete_package(const char *name) {
    const char *sqls[] = {
        "DELETE FROM files WHERE package_name = ?;",
        "DELETE FROM depends WHERE package_name = ?;",
        "DELETE FROM packages WHERE name = ?;",
    };
    for (int i = 0; i < 3; i++) {
        sqlite3_stmt *s = prep(sqls[i]);
        if (!s) return XPKG_ERR_DB;
        sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
        if (step_done(s) != XPKG_OK) return XPKG_ERR_DB;
    }
    return XPKG_OK;
}

xpkg_status_t xpkg_db_clear_files(const char *name) {
    sqlite3_stmt *s = prep("DELETE FROM files WHERE package_name = ?;");
    if (!s) return XPKG_ERR_DB;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    return step_done(s);
}

xpkg_status_t xpkg_db_add_file(const char *name, const char *path, const char *sha256) {
    static sqlite3_stmt *s;   /* hot path: reuse the statement */
    if (!s && !(s = prep("INSERT INTO files (package_name, path, sha256) VALUES (?, ?, ?);")))
        return XPKG_ERR_DB;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 3, sha256, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) != SQLITE_DONE) {
        xpkg_err("database: %s", sqlite3_errmsg(g_db));
        return XPKG_ERR_DB;
    }
    return XPKG_OK;
}

int xpkg_db_file_owner(const char *path, char *owner, size_t ownersz) {
    sqlite3_stmt *s = prep("SELECT package_name FROM files WHERE path = ? LIMIT 1;");
    if (!s) return 0;
    sqlite3_bind_text(s, 1, path, -1, SQLITE_STATIC);
    int found = sqlite3_step(s) == SQLITE_ROW;
    if (found && owner) copy_col(s, 0, owner, ownersz);
    sqlite3_finalize(s);
    return found;
}

int xpkg_db_file_sha(const char *name, const char *path, char out[65]) {
    sqlite3_stmt *s = prep("SELECT sha256 FROM files WHERE package_name = ? AND path = ? LIMIT 1;");
    if (!s) return 0;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, path, -1, SQLITE_STATIC);
    int found = sqlite3_step(s) == SQLITE_ROW;
    if (found) copy_col(s, 0, out, 65);
    sqlite3_finalize(s);
    return found;
}

xpkg_status_t xpkg_db_each_package(xpkg_row_cb cb, void *user) {
    sqlite3_stmt *s = prep("SELECT name, version, description, installed_at, explicit "
                           "FROM packages ORDER BY name;");
    if (!s) return XPKG_ERR_DB;
    xpkg_pkg_row_t row;
    while (sqlite3_step(s) == SQLITE_ROW) {
        copy_col(s, 0, row.name, sizeof(row.name));
        copy_col(s, 1, row.version, sizeof(row.version));
        copy_col(s, 2, row.description, sizeof(row.description));
        row.installed_at = sqlite3_column_int64(s, 3);
        row.explicit_ = sqlite3_column_int(s, 4);
        if (cb(&row, user)) break;
    }
    sqlite3_finalize(s);
    return XPKG_OK;
}

static xpkg_status_t each_pair(const char *sql, const char *name, xpkg_str_cb cb, void *user, int two) {
    sqlite3_stmt *s = prep(sql);
    if (!s) return XPKG_ERR_DB;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    while (sqlite3_step(s) == SQLITE_ROW) {
        const char *a = (const char *)sqlite3_column_text(s, 0);
        const char *b = two ? (const char *)sqlite3_column_text(s, 1) : NULL;
        if (cb(a ? a : "", b, user)) break;
    }
    sqlite3_finalize(s);
    return XPKG_OK;
}

xpkg_status_t xpkg_db_each_file(const char *name, xpkg_str_cb cb, void *user) {
    return each_pair("SELECT path, sha256 FROM files WHERE package_name = ? ORDER BY path;",
                     name, cb, user, 1);
}

xpkg_status_t xpkg_db_each_depend(const char *name, xpkg_str_cb cb, void *user) {
    return each_pair("SELECT dep FROM depends WHERE package_name = ? ORDER BY dep;",
                     name, cb, user, 0);
}

xpkg_status_t xpkg_db_each_rdepend(const char *name, xpkg_str_cb cb, void *user) {
    return each_pair("SELECT d.package_name FROM depends d JOIN packages p "
                     "ON p.name = d.package_name WHERE d.dep = ? ORDER BY 1;",
                     name, cb, user, 0);
}

int xpkg_db_count_files(const char *name) {
    sqlite3_stmt *s = prep("SELECT COUNT(*) FROM files WHERE package_name = ?;");
    if (!s) return 0;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    int n = sqlite3_step(s) == SQLITE_ROW ? sqlite3_column_int(s, 0) : 0;
    sqlite3_finalize(s);
    return n;
}
