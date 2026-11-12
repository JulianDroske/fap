#include "stdio.h"
#include "stdarg.h"
#include "sqlite3.h"

/* in sqlite3, integer primary key implies ROWID which is a simplified version of autoincrement */
const char *db_initsql =
  "create table if not exists pm("
  "  pm_id integer primary key not null,"
  "  pm_name text not null);"
  "create table if not exists pkg("
  "  pkg_id integer primary key not null,"
  "  pm_id integer not null,"
  "  pkg_name text not null,"
  "  pkg_ignored boolean);"
  "create table if not exists exe("
  "  exe_id integer primary key not null,"
  "  exe_path text not null,"
  "  exe_ignored boolean,"
  "  pkg_id integer);"
  "create table if not exists file("
  "  file_path text primary key not null,"
  "  exe_id_create integer not null,"
  "  file_time_create integer not null,"
  "  file_exist boolean,"
  "  exe_id_delete integer,"
  "  file_time_delete integer);"
;

void db_logerror(void *p, int code, const char *msg) {
  fprintf(stderr, "sqlite3: %s (%d)\n", msg, code);
}

sqlite3_stmt *db_stmt_va(sqlite3 *db, const char *sql, const char *spec, va_list args) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) return NULL;

  if (!spec) return stmt;

  int pos = 1;
  for (const char *p = spec; *p; ++p) {
    switch(*p) {
    case 'i': sqlite3_bind_int(stmt, pos++, va_arg(args, int)); break;
    case 'I': sqlite3_bind_int64(stmt, pos++, va_arg(args, int64_t)); break;
    case 's': sqlite3_bind_text(stmt, pos++, va_arg(args, const char *), -1, NULL); break;
    default: break;
    }
  }

  return stmt;
}

sqlite3_stmt *db_stmt(sqlite3 *db, const char *sql, const char *spec, ...) {
  va_list args; va_start(args, spec);
  sqlite3_stmt *stmt = db_stmt_va(db, sql, spec, args);
  va_end(args);
  return stmt;
}

void db_stmt_columns_va(sqlite3_stmt *stmt, const char *spec, va_list args) {
  if (!spec) return;

  int pos = 0;
  for (const char *p = spec; *p; ++p) {
    switch (*p) {
    case 'i': *va_arg(args, int *) = sqlite3_column_int(stmt, pos++); break;
    case 'I': *va_arg(args, int64_t *) = sqlite3_column_int64(stmt, pos++); break;
    case 's': *va_arg(args, const char **) = sqlite3_column_text(stmt, pos++); break;
    // case 'S': strncpy(va_arg(args, char *), sqlite3_column_text(stmt, pos), va_arg(args, size_t)); break;
    default: break;
    }
  }
}

void db_stmt_columns(sqlite3_stmt *stmt, const char *spec, ...) {
  va_list args; va_start(args, spec);
  db_stmt_columns_va(stmt, spec, args);
  va_end(args);
}

sqlite3 *db_open(const char *pathname) {
  sqlite3 *db = NULL;
  if (sqlite3_open(pathname, &db)) {
    fprintf(stderr, "failed to open sqlite database %s\n", pathname);
    return NULL;
  }

  char *sqlerr = NULL;
  if (sqlite3_exec(db, db_initsql, NULL, NULL, &sqlerr)) {
    fprintf(stderr, "failed to init database: %s\n", sqlerr);
    sqlite3_free(sqlerr);
    sqlite3_close(db);
    return NULL;
  }

  return db;
}

int db_query_intbystr(sqlite3 *db, const char *sql, const char *string) {
  int i = 0;
  sqlite3_stmt *stmt;
  if (!sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
    sqlite3_bind_text(stmt, 1, string, -1, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      if (i) fprintf(stderr, "intbystr: duplicated results received [%s]\n", sql);
      i = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }
  return i;
}

int db_query_int(sqlite3 *db, const char *sql, const char *spec, ...) {
  int i = 0;
  va_list args; va_start(args, spec);

  sqlite3_stmt *stmt = db_stmt_va(db, sql, spec, args);
  if (stmt) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      if (i) {
        fprintf(stderr, "db_query_int: duplicated results received [%s]\n", sql);
        break;
      }
      i = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }

  va_end(args);
  return i;
}

int db_query(sqlite3 *db, const char *sql, const char *spec, ...) {
  int has_row = 0;
  va_list args; va_start(args, spec);

  sqlite3_stmt *stmt = db_stmt_va(db, sql, spec, args);
  if (stmt) {
    const char *out_spec = va_arg(args, const char *);

    /* only runs once so no need to reset args */
    if (sqlite3_step(stmt) == SQLITE_ROW)
      db_stmt_columns_va(stmt, out_spec, args);
    sqlite3_finalize(stmt);
  }

  va_end(args);
  return has_row;
}

// int db_has_file(sqlite3 *db, const char *pathname) {
//   return db_query_intbystr("select count(file_path) from file where file_path = ? limit 1;", pathname) > 0;
// }

// int db_query_file_byexe(sqlite3 *db, int exe_id) {
//   return db_query_intbystr(db, "select id from exe where exe_path = ?;", pathname);
// }

/* pm */

int db_add_pm(sqlite3 *db, const char *name) {
  return db_query_intbystr(db, "insert into pm(pm_name) values(?1) on conflict do update set pm_name = ?1 returning pm_id;", name);
}

/* pkg */

int db_find_pkgbyexe(sqlite3 *db, const char *exe_path) {
  return db_query_intbystr(db, "select exe.pkg_id from exe left join pkg on pkg.id = exe.pkg_id where exe.exe_path = ?;", exe_path);
}

int db_find_pkg(sqlite3 *db, int pm_id, const char *pkg_name) {
  return db_query_int(db, "select pkg_id from pkg where pm_id = ? and pkg_name = ?;", "is", pm_id, pkg_name);
}

int db_add_pkg(sqlite3 *db, int pm_id, const char *pkg_name) {
  int pkg_id = db_find_pkg(db, pm_id, pkg_name);
  if (pkg_id) return pkg_id;
  return db_query_int(db, "insert into pkg(pm_id, pkg_name, pkg_ignored) values(?, ?, false) returning pkg_id;", "is", pm_id, pkg_name);
}

// int db_del_exe(sqlite3 *db, const char *pathname) {
//   int exe_id = db_get_exe(db, pathname);
//   if (exe_id)
//     return db_query_intbystr(db,
//       "begin delexe;"
//       "  update from file where file.exe_id = ?1;"
//       "  delete from exe where exe_id = ?1 returning exe_id;"
//       "commit delexe;", pathname);
//   return 0;
// }

/* exe */

int db_find_exe(sqlite3 *db, const char *exe_path) {
  return db_query_intbystr(db, "select exe_id from exe where exe_path = ?;", exe_path);
}

int db_get_exe_ignored(sqlite3 *db, int exe_id) {
  /* defaults to 0 on failure */
  // TODO separate failure and ignored == 0 ?
  return db_query_int(db, "select exe_ignored from exe where exe_id = ?;", "i", exe_id);
}

int db_get_exepkg_ignored(sqlite3 *db, int exe_id) {
  int exe_ignored = 0, pkg_ignored = 0;
  if (!db_query(db, "select exe_ignored, pkg_ignored from exe left join pkg on pkg.pkg_id = exe.pkg_id where exe_id = ?;",
    "i", exe_id, "ii", &exe_ignored, &pkg_ignored))
    return 0;
  return exe_ignored || pkg_ignored;
}

int db_get_exe_pkg(sqlite3 *db, int exe_id) {
  return db_query_int(db, "select pkg_id from exe where exe_id = ?;", "i", exe_id);
}

int db_add_exe(sqlite3 *db, const char *pathname) {
  int exe_id = db_find_exe(db, pathname);
  if (!exe_id) exe_id = db_query_intbystr(db, "insert into exe(exe_path, exe_ignored, pkg_id) values(?, false, 0) returning exe_id;", pathname);
  return exe_id;
}

int db_set_exe_pkg(sqlite3 *db, int exe_id, int pkg_id) {
  return db_query_int(db, "update exe set pkg_id = ?2 where exe_id = ?1 returning exe_id;", "ii", exe_id, pkg_id);
}

/* file */

int db_record_file_create(sqlite3 *db, const char *file_path, int exe_id, int64_t epochsec) {
  return db_query_int(db,
    "insert into file(file_path, exe_id_create, file_time_create, file_exist)"
    "  values(?1, ?2, ?3, true)"
    "  on conflict(file_path) do"
    "  update set exe_id_create = ?2, file_time_create = ?3, file_exist = true returning exe_id_create;",
    "siI", file_path, exe_id, epochsec);
}

int db_record_file_delete(sqlite3 *db, const char *file_path, int exe_id, int64_t epochsec) {
  return db_query_int(db, "update file set file_exist = false, exe_id_delete = ?2, file_time_delete = ?3 where file_path = ?1 returning exe_id_delete;",
    "siI", file_path, exe_id, epochsec);
}

int db_del_file(sqlite3 *db, const char *pathname) {
  return db_query_intbystr(db, "delete from file where file_path = ? returning rowid;", pathname) > 0;
}

// int db_update_file_exist(sqlite3 *db, const char *pathname, int exist) {
//   return db_query_intbystr("update file set file_exist = ?2 where file_path = ?1 returning exe_id;") > 0;
// }


// int db_get_exe_pkg(sqlite3 *db, int exe_id) {
//   return db_query_int(db, "select pkg_id from exe where exe_id = ?", "i", exe_id);
// }

