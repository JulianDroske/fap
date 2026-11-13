#ifndef _DB_H_
#define _DB_H_

#include "stdint.h"
#include "sqlite3.h"

void db_sqlite3_logerror(void *p, int code, const char *msg);
void db_logerror(const char *format, ...);
sqlite3_stmt *db_stmt_va(sqlite3 *db, const char *sql, const char *spec, va_list args);
sqlite3_stmt *db_stmt(sqlite3 *db, const char *sql, const char *spec, ...);
void db_stmt_columns_va(sqlite3_stmt *stmt, const char *spec, va_list args);
void db_stmt_columns(sqlite3_stmt *stmt, const char *spec, ...);
sqlite3 *db_open(const char *pathname);
int db_query_intbystr(sqlite3 *db, const char *sql, const char *string);
int db_query_int(sqlite3 *db, const char *sql, const char *spec, ...);
int db_query(sqlite3 *db, const char *sql, const char *spec, ...);
int db_add_pm(sqlite3 *db, const char *name);
int db_find_pkgbyexe(sqlite3 *db, const char *exe_path);
int db_find_pkg(sqlite3 *db, int pm_id, const char *pkg_name);
int db_add_pkg(sqlite3 *db, int pm_id, const char *pkg_name);
int db_set_pkg_exist(sqlite3 *db, int pkg_id, int exist);
int db_find_exe(sqlite3 *db, const char *exe_path);
int db_add_exe(sqlite3 *db, const char *pathname);
int db_get_exe_ignored(sqlite3 *db, int exe_id);
int db_get_exe_pkg(sqlite3 *db, int exe_id);
int db_check_exe_ignored(sqlite3 *db, int exe_id);
int db_check_exe_exist(sqlite3 *db, int exe_id);
int db_set_exe_exist(sqlite3 *db, int exe_id, int exist);
int db_set_exe_pkg(sqlite3 *db, int exe_id, int pkg_id);
int db_set_file_exist(sqlite3 *db, const char *file_path, int exist);
int db_record_file_create(sqlite3 *db, const char *file_path, int exe_id, int64_t timestamp);
int db_record_file_delete(sqlite3 *db, const char *file_path, int exe_id, int64_t timestamp);
int db_del_file(sqlite3 *db, const char *pathname);

#endif
