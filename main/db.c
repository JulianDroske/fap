#include "stdio.h"
#include "string.h"
#include "stdint.h"
#include "stdarg.h"
#include "sqlite3.h"

#include "sys/stat.h"
#include "linux/limits.h"

// TODO fstat...
void db_mkdirnames(const char *pathname) {
  static char path[PATH_MAX];
  strncpy(path, pathname, PATH_MAX);
  size_t len = strlen(path);
  for (char *p = path, *end = path + len; p < end; ++p) {
    if (*p != '/') continue;
    *p = 0; mkdir(path, 0755); *p = '/';
  }
}

void db_sqlite3_logger(void *p, int code, const char *msg) {
  fprintf(stderr, "sqlite3: %s (%d)\n", msg, code);
}

void db_logerror(const char *format, ...) {
  va_list args; va_start(args, format);
  fprintf(stderr, "db: "); vfprintf(stderr, format, args); fprintf(stderr, "\n");
  va_end(args);
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
    case 's': *va_arg(args, const char **) = (char *) sqlite3_column_text(stmt, pos++); break;
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

int db_stmt_next(sqlite3_stmt *stmt, const char *spec, ...) {
  if (sqlite3_step(stmt) != SQLITE_ROW) return 0;
  va_list args; va_start(args, spec);
  db_stmt_columns_va(stmt, spec, args);
  va_end(args);
  return 1;
}

int db_stmt_next_or_fini(sqlite3_stmt *stmt, const char *spec, ...) {
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    va_list args; va_start(args, spec);
    db_stmt_columns_va(stmt, spec, args);
    va_end(args);
    return 1;
  }
  sqlite3_finalize(stmt);
  return 0;
}


/* in sqlite3, integer primary key implies ROWID, a simplified version of autoincrement */
const char *db_initsql =
  "create table if not exists pm("
  "  pm_id integer primary key not null,"
  "  pm_name text not null);"
  "create table if not exists pkg("
  "  pkg_id integer primary key not null,"
  "  pm_id integer not null,"
  "  pkg_name text not null,"
  "  pkg_exist boolean not null,"
  "  pkg_ignored boolean);"
  "create table if not exists exe("
  "  exe_id integer primary key not null,"
  "  exe_path text not null,"
  "  exe_exist boolean not null,"
  "  exe_ignored boolean,"
  "  pkg_id integer);"
  "create table if not exists file("
  "  file_path text primary key not null,"
  "  exe_id_create integer not null,"
  "  file_time_create integer not null,"
  "  file_exist boolean not null,"
  "  exe_id_delete integer,"
  "  file_time_delete integer);"
  "pragma journal_mode=WAL;";

sqlite3 *db_open(const char *pathname) {
  if (!pathname) return NULL;
  db_mkdirnames(pathname);

  sqlite3 *db = NULL;
  if (sqlite3_open(pathname, &db)) {
    db_logerror("failed to open sqlite database %s", pathname);
    return NULL;
  }

  char *sqlerr = NULL;
  if (sqlite3_exec(db, db_initsql, NULL, NULL, &sqlerr)) {
    db_logerror("failed to init database: %s", sqlerr);
    sqlite3_free(sqlerr);
    sqlite3_close(db);
    return NULL;
  }

  return db;
}

int db_eval_int_bystr(sqlite3 *db, const char *sql, const char *string) {
  int i = 0;
  sqlite3_stmt *stmt;
  if (!sqlite3_prepare_v2(db, sql, -1, &stmt, NULL)) {
    sqlite3_bind_text(stmt, 1, string, -1, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      if (i) db_logerror("db_eval_int_bystr: duplicated results received [%s]", sql);
      i = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }
  return i;
}

int db_eval_int(sqlite3 *db, const char *sql, const char *spec, ...) {
  int i = 0;
  va_list args; va_start(args, spec);

  sqlite3_stmt *stmt = db_stmt_va(db, sql, spec, args);
  if (stmt) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      if (i) {
        db_logerror("db_eval_int: duplicated results received [%s]", sql);
        break;
      }
      i = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
  }

  va_end(args);
  return i;
}

int db_eval(sqlite3 *db, const char *sql, const char *spec, ...) {
  int has_row = 0;
  va_list args; va_start(args, spec);

  sqlite3_stmt *stmt = db_stmt_va(db, sql, spec, args);
  if (stmt) {
    const char *out_spec = va_arg(args, const char *);

    /* only runs once so no need to reset args */
    if (sqlite3_step(stmt) == SQLITE_ROW)
      { db_stmt_columns_va(stmt, out_spec, args); has_row = 1; }
    sqlite3_finalize(stmt);
  }

  va_end(args);
  return has_row;
}

// int db_has_file(sqlite3 *db, const char *pathname) {
//   return db_eval_int_bystr("select count(file_path) from file where file_path = ? limit 1;", pathname) > 0;
// }

// int db_eval_file_byexe(sqlite3 *db, int exe_id) {
//   return db_eval_int_bystr(db, "select id from exe where exe_path = ?;", pathname);
// }

/* pm */

int db_add_pm(sqlite3 *db, const char *name) {
  int pm_id = db_eval_int_bystr(db, "select pm_id from pm where pm_name = ?;", name);
  if (!pm_id) pm_id = db_eval_int_bystr(db, "insert into pm(pm_name) values(?1) returning pm_id;", name);
  return pm_id;
}

/* pkg */

int db_find_pkg_byexe(sqlite3 *db, const char *exe_path) {
  return db_eval_int_bystr(db, "select exe.pkg_id from exe left join pkg on pkg.id = exe.pkg_id where exe.exe_path = ?;", exe_path);
}

int db_find_pkg(sqlite3 *db, int pm_id, const char *pkg_name) {
  return db_eval_int(db, "select pkg_id from pkg where pm_id = ? and pkg_name = ?;", "is", pm_id, pkg_name);
}

int db_add_pkg(sqlite3 *db, int pm_id, const char *pkg_name) {
  int pkg_id = db_find_pkg(db, pm_id, pkg_name);
  if (pkg_id) return pkg_id;
  return db_eval_int(db, "insert into pkg(pm_id, pkg_name, pkg_exist, pkg_ignored) values(?, ?, true, false) returning pkg_id;", "is", pm_id, pkg_name);
}

int db_set_pkg_exist(sqlite3 *db, int pkg_id, int exist) {
  return db_eval_int(db, "update pkg set pkg_exist = ?2 where pkg_id = ?1 returning pkg_id;", "ii", pkg_id, exist);
}

int db_set_pkg_ignored(sqlite3 *db, int pkg_id, int ignored) {
  return db_eval_int(db, "update pkg set pkg_ignored = ?2 where pkg_id = ?1 returning pkg_id;", "ii", pkg_id, ignored);
}

// int db_del_exe(sqlite3 *db, const char *pathname) {
//   int exe_id = db_get_exe(db, pathname);
//   if (exe_id)
//     return db_eval_int_bystr(db,
//       "begin delexe;"
//       "  update from file where file.exe_id = ?1;"
//       "  delete from exe where exe_id = ?1 returning exe_id;"
//       "commit delexe;", pathname);
//   return 0;
// }

/* exe */

int db_find_exe(sqlite3 *db, const char *exe_path) {
  return db_eval_int_bystr(db, "select exe_id from exe where exe_path = ?;", exe_path);
}

int db_add_exe(sqlite3 *db, const char *pathname) {
  int exe_id = db_find_exe(db, pathname);
  if (!exe_id) exe_id = db_eval_int_bystr(db, "insert into exe(exe_path, exe_exist, exe_ignored, pkg_id) values(?, true, false, 0) returning exe_id;", pathname);
  return exe_id;
}

int db_get_exe_ignored(sqlite3 *db, int exe_id) {
  /* defaults to 0 on failure */
  // TODO separate failure and ignored == 0 ?
  return db_eval_int(db, "select exe_ignored from exe where exe_id = ?;", "i", exe_id);
}

int db_get_exe_pkg(sqlite3 *db, int exe_id) {
  return db_eval_int(db, "select pkg_id from exe where exe_id = ?;", "i", exe_id);
}

int db_check_exe_ignored(sqlite3 *db, int exe_id) {
  int exe_ignored = 0, pkg_ignored = 0;
  if (!db_eval(db, "select exe_ignored, pkg_ignored from exe left join pkg on pkg.pkg_id = exe.pkg_id where exe_id = ?;",
    "i", exe_id, "ii", &exe_ignored, &pkg_ignored))
    return 0;
  return exe_ignored || pkg_ignored;
}

int db_check_exe_exist(sqlite3 *db, int exe_id) {
  return db_eval_int(db,
    "select exe.exe_id from exe left join pkg on pkg.pkg_id = exe.pkg_id"
    "  where exe_id = ? and exe.exe_exist and (pkg.pkg_exist is null or pkg.pkg_exist)",
    "i", exe_id);
}

int db_set_exe_exist(sqlite3 *db, int exe_id, int exist) {
  return db_eval_int(db, "update exe set exe_exist = ?2 where exe_id = ?1 returning exe_id;", "ii", exe_id, exist);
}

int db_set_exe_ignored(sqlite3 *db, int exe_id, int ignored) {
  return db_eval_int(db, "update exe set exe_ignored = ?2 where exe_id = ?1 returning exe_id;", "ii", exe_id, ignored);
}

int db_set_exe_pkg(sqlite3 *db, int exe_id, int pkg_id) {
  return db_eval_int(db, "update exe set pkg_id = ?2 where exe_id = ?1 returning exe_id;", "ii", exe_id, pkg_id);
}

struct db_exepkgpm_query {
  struct { const char *exe_path, *pkg_name, *pm_name; } args;
  sqlite3_stmt *stmt;

  int exe_id, pkg_id;
  int exe_ignored, pkg_ignored;
  const char *exe_path, *pkg_name, *pm_name;
};

int db_query_exepkgpm(sqlite3 *db, struct db_exepkgpm_query *q) {
  if (!q->stmt) {
    /*
      sqlite3 will mock an automatic index when joining a non-pk column
      for instance,
      | select * from pkg left join exe on exe.pkg_id = pkg.pkg_id;
      where sqlite3 needs to do a full scan on exe to get matched rows
      so optimizations on [pm:]pkg queries are not preferred for now

      https://stackoverflow.com/questions/39128296
    */
    q->stmt = db_stmt(db,
      "select exe.exe_id, exe.exe_path, exe.exe_ignored,"
      "       pkg.pkg_id, pkg.pkg_name, pkg.pkg_ignored,"
      "       pm.pm_name from exe"
      "  left join pkg on pkg.pkg_id = exe.pkg_id"
      "  left join pm on pm.pm_id = pkg.pm_id"
      "  where (?1 is null or ?1 = exe.exe_path) and (?2 is null or ?2 = pkg.pkg_name) and (?3 is null or ?3 = pm.pm_name)"
      "  order by exe.pkg_id;", "sss", q->args.exe_path, q->args.pkg_name, q->args.pm_name);
    if (!q->stmt) return 0;
  }

  return db_stmt_next(q->stmt, "isi isi s",
    &q->exe_id, &q->exe_path, &q->exe_ignored,
    &q->pkg_id, &q->pkg_name, &q->pkg_ignored, &q->pm_name);
}

/* file */

int db_set_file_exist(sqlite3 *db, const char *file_path, int exist) {
  return db_eval_int(db, "update file set file_exist = ?2 where file_path = ?1 returning file_path is not null;", "si", file_path, exist);
}

int db_record_file_create(sqlite3 *db, const char *file_path, int exe_id, int64_t timestamp) {
  return db_eval_int(db,
    "insert into file(file_path, exe_id_create, file_time_create, file_exist)"
    "  values(?1, ?2, ?3, true)"
    "  on conflict(file_path) do"
    "  update set exe_id_create = ?2, file_time_create = ?3, file_exist = true returning exe_id_create;",
    "siI", file_path, exe_id, timestamp);
}

int db_record_file_delete(sqlite3 *db, const char *file_path, int exe_id, int64_t timestamp) {
  return db_eval_int(db, "update file set file_exist = false, exe_id_delete = ?2, file_time_delete = ?3 where file_path = ?1 returning exe_id_delete;",
    "siI", file_path, exe_id, timestamp);
}

int db_del_file(sqlite3 *db, const char *pathname) {
  return db_eval_int_bystr(db, "delete from file where file_path = ? returning rowid;", pathname) > 0;
}

// int db_update_file_exist(sqlite3 *db, const char *pathname, int exist) {
//   return db_eval_int_bystr("update file set file_exist = ?2 where file_path = ?1 returning exe_id;") > 0;
// }


// int db_get_exe_pkg(sqlite3 *db, int exe_id) {
//   return db_eval_int(db, "select pkg_id from exe where exe_id = ?", "i", exe_id);
// }

