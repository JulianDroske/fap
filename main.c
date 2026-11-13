#ifndef _GNU_SOURCE
#  define _GNU_SOURCE 1
#endif

#include "stdint.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

#include "tinycli.c"
#include "fatrack.c"
#include "pstrack.c"

#include "pm.c"
#include "db.c"

// TODO fstat...
void mkdirnames(const char *pathname) {
  static char path[PATH_MAX];
  strncpy(path, pathname, PATH_MAX);
  size_t len = strlen(path);
  for (char *p = path, *end = path + len; p < end; ++p) {
    if (*p != '/') continue;
    *p = 0; mkdir(path, 0755); *p = '/';
  }
}

/* https://en.cppreference.com/cpp/chrono/c/time */
/* https://www.sqlite.org/datatype3.html */
// long int epoch() {
//   struct timespec spec = {};
//   if (clock_gettime(CLOCK_REALTIME, &spec)) return 0;
//   return spec.tv_sec;
// }
int64_t getepochsec() { return (int64_t) time(NULL); }
time_t epochsec_time(int64_t epoch) { return (time_t) epoch; }

const char *epochsec_localstring(int64_t epochsec) {
  static char time[256] = {};
  time_t t = epochsec_time(epochsec);
  if (strftime(time, sizeof(time), "%x %X", localtime(&t)) <= 0) return NULL;
  return time;
}

const char *database_pathname = "/var/lib/fap/log.db";
const char *pm_name = NULL;

void print_help() {
  fprintf(stderr, "%s",
    "fap - file app process monitor (demo)" "\n"
    "usage: fap [options..] <command> [params..]" "\n"
    "options:" "\n"
    "  -h, --help               show this help and exit" "\n"
    "  -D, --database <file>    specify database path" "\n"
    "  -P, --pm <name>          specify package manager to interact" "\n"
    "                           available: apk, dpkg" "\n"
    "commands & params:" "\n"
    "  m, monitor [dirs...]     monitor file changes and save to database" "\n"
    "                           defaults to monitoring all dirs (/)" "\n"
    "    -n, --dry-run            do not write to database" "\n"
    "    -o, --operations <ops>   filter file operations on output" "\n"
    "                               C: create, D: delete, M: move" "\n"
    "                               e.g. fap monitor -o 'CDM'" "\n"
    "    -f, --format <fmt>       output format" "\n"
    "                               %T - time" "\n"
    "                               %E - exe fullpath" "\n"
    "                               %P - exe pid" "\n"
    "                               %N - target filepath" "\n"
    "                               %O - operation(s)" "\n"
    "    -a, --all                also record deleted files" "\n"
    "  l, list                  list known files and related programs" "\n"
    "    -a, --all                also show deleted files" "\n"
    "  s, show [progs...]       show programs and file changes" "\n"
    "                           progs... format: [<pm>:]<pkg> or <exe_path>" "\n"
    "                           exe_path always starts with '/'" "\n"
    "    -p, --packages           catagorise as packages" "\n"
    "    -a, --all                also show deleted files" "\n"
    // "  p, prune                 clean records for deleted files" "\n"
    // "  u, update                update package info and file status" "\n"
  );
}

int main_monitor(int argc, const char **argv) {
  const char **opt_dryrun = NULL, **opt_operations = NULL,
    **opt_format = NULL, **opt_all = NULL;
  struct tinycli_option opts[] = {
    { "dry-run", 'd', 0, &opt_dryrun },
    { "operations", 'o', 1, &opt_operations },
    { "format", 'f', 1, &opt_format },
    { "all", 'a', 0, &opt_all },
    { NULL }
  };

  int cliopts_len = tinycli_parse(opts, --argc, ++argv);
  if (cliopts_len < 0) {
    fprintf(stderr, "invalid argument around %s\n", argv[-cliopts_len - 1]);
    print_help(); return 1;
  }
  argc -= cliopts_len; argv += cliopts_len;

  int save_db = opt_dryrun == NULL;
  unsigned int filter_ops = FAN_CREATE | FAN_DELETE | FAN_MOVE;
  if (opt_operations) {
    filter_ops = 0;
    const char *spec = opt_operations[1];
    for (const char *s = spec; *s; ++s) {
      switch (*s) {
      case 'C': case 'c': filter_ops |= FAN_CREATE; break;
      case 'D': case 'd': filter_ops |= FAN_DELETE; break;
      case 'M': case 'm': filter_ops |= FAN_MOVE; break;
      default: fprintf(stderr, "unrecognized operation: %c\n", *s);
        print_help(); return 1;
      }
    }
  }
  const char *output_format = "[%T] %E(%P): %O %F";
  if (opt_format) output_format = opt_format[1];
  int keep_deleted = opt_all != NULL;

  sqlite3 *db = NULL;
  if (save_db) {
    mkdirnames(database_pathname);
    db = db_open(database_pathname);
    if (!db) {
      fprintf(stderr, "failed to open sqlite database %s\n", database_pathname);
      return 1;
    }
  }

  int pm_id = 0;
  struct pm *pm = NULL;
  if (save_db) {
    pm = pm_get(pm_name);
    if (pm) {
      pm_id = db_add_pm(db, pm->name);
      if (pm_id) fprintf(stderr, "using package manager '%s'\n", pm->name);
      else fprintf(stderr, "failed to record package manager\n");
    }
    else fprintf(stderr, "warning: failed to find pm '%s'\n", pm_name);
  }

  struct pstrack pstrack = {};
  if (!pstrack_init(&pstrack, 64)) {
    fprintf(stderr, "failed to init pstrack: %d\n", errno);
    return 1;
  }

  // TODO fini
  struct fatrack fatrack = {};
  if (!fatrack_init(&fatrack, FAN_ONDIR | FAN_EVENT_ON_CHILD | FAN_CREATE | FAN_DELETE | FAN_MOVE)) {
    fprintf(stderr, "failed to init fatrack: %s\n", fatrack.error); return 1;
  }
  // if (fatrack_haserror(&fatrack))
  //   fprintf(stderr, "fatrack: %s\n", fatrack.error);

  while (argc) {
    static char path[PATH_MAX];
    struct stat st;
    if (realpath(*argv, path) && !stat(path, &st) && S_ISDIR(st.st_mode)) {
      if (!fatrack_markpath(&fatrack, path)) {
        fprintf(stderr, "failed to record target directory: %s -> %s: %s\n", *argv, path, fatrack.error);
        return 1;
      }
    }
    else {
      fprintf(stderr, "invalid directory in argument: %s\n", *argv);
      return 1;
    }
    --argc; ++argv;
  }

  fatrack_marksubmounts(&fatrack);
  // if (fatrack_haserror(&fatrack))
  //   fprintf(stderr, "failed to mark mountpoints: %s\n", fatrack.error);

  pid_t self_pid = getpid();

  main_loop:
  while (pstrack_alive(&pstrack)) {
    int64_t epochsec = getepochsec();

    struct fatrack_event ev = {};
    int stat = fatrack_next(&fatrack, &ev);
    if (fatrack_haserror(&fatrack))
      fprintf(stderr, "fatrack: %s\n", fatrack.error);
    if (stat < 0) break; if (!stat) continue;

    struct fanotify_event_metadata *metadata = &ev.metadata;

    // TODO handle move

    struct pstrack_procinfo proc = {};
    if (!pstrack_query(&pstrack, &proc, metadata->pid)) {
      fprintf(stderr, "pstrack: failed to query pid %d: %d\n", metadata->pid, errno);
      continue;
    }

    static char ops[6];
    char *o = ops;
    if (metadata->mask & FAN_OPEN) *o++ = 'O';
    if (metadata->mask & FAN_ACCESS) *o++ = 'A';
    if (metadata->mask & FAN_CREATE) *o++ = 'C';
    if (metadata->mask & FAN_DELETE) *o++ = 'D';
    if (metadata->mask & FAN_MOVE) *o++ = 'M';
    *o = 0;

    // TODO buffered output
    for (const char *f = output_format; *f; ++f) {
      if (*f == '%') {
        switch (*++f) {
        case 'T': printf("%s", epochsec_localstring(epochsec)); break;
        case 'E': printf("%s", proc.pathname); break;
        case 'P': printf("%d", metadata->pid); break;
        case 'F': printf("%s", ev.filepath); break;
        case 'O': printf("%s", ops); break;
        default: printf("%c", *f); break;
        }
      }
      else printf("%c", *f);
    }
    puts("");

    if (!save_db) continue;

    /* update database */

    int exe_id = db_add_exe(db, proc.pathname);
    if (!exe_id)
      { fprintf(stderr, "failed to record exe %s\n", proc.pathname); break; }
    /* use parent process if ignored */
    int exe_pid = metadata->pid;
    while (db_get_exepkg_ignored(db, exe_id)) {
      exe_pid = proc.ppid;
      if (exe_pid == self_pid) continue main_loop;
      if (!pstrack_query(&pstrack, &proc, exe_pid)) {
        fprintf(stderr, "pstrack: failed to query pid %d: %d\n", exe_pid, errno);
        break;
      }
      exe_id = db_add_exe(db, proc.pathname);
      if (!exe_id) {
        fprintf(stderr, "failed to add exe %s\n", proc.pathname);
        continue main_loop;
      }
    }

    int pkg_id = db_get_exe_pkg(db, exe_id);
    if (!pkg_id && pm && pm_id) {
      static char name[PM_NAME_MAX];
      if (pm_queryfile(pm, name, proc.pathname))
        pkg_id = db_add_pkg(db, pm_id, name);
      if (pkg_id)
        if (!db_set_exe_pkg(db, exe_id, pkg_id))
          fprintf(stderr, "failed to assign exe %d with pkg %d\n", exe_id, pkg_id);
    }

    const char *filepath = ev.filepath;
    /* fanotify may report an event with CD */
    if (!keep_deleted && metadata->mask & FAN_DELETE)
      db_del_file(db, filepath);
    else {
      if (metadata->mask & FAN_CREATE) {
        if (!db_record_file_create(db, filepath, exe_id, epochsec))
          { fprintf(stderr, "failed to record file creation %s\n", filepath); break; }
      }
      if (metadata->mask & FAN_DELETE)
        db_record_file_delete(db, filepath, exe_id, epochsec);
    }
  }

  if (save_db) sqlite3_close(db);

  return 0;
}

int main_list(int argc, const char **argv) {
  const char **opt_all = NULL;
  struct tinycli_option opts[] = {
    { "all", 'a', 0, &opt_all },
    { NULL }
  };

  int cliopts_len = tinycli_parse(opts, --argc, ++argv);
  if (cliopts_len < 0) {
    fprintf(stderr, "invalid argument around %s\n", argv[-cliopts_len - 1]);
    print_help(); return 1;
  }
  argc -= cliopts_len; argv += cliopts_len;

  sqlite3 *db = db_open(database_pathname);
  if (!db) {
    fprintf(stderr, "failed to open sqlite database %s\n", database_pathname);
    return 1;
  }

  sqlite3_stmt *stmt = db_stmt(db,
    "select file.file_exist, file.file_path, file.file_time_create, file.file_time_delete,"
    "       exe.exe_path, pkg.pkg_name, pm.pm_name, delexe.exe_path, delpkg.pkg_name, delpm.pm_name from file"
    "  left join exe on exe.exe_id = file.exe_id_create"
    "  left join pkg on pkg.pkg_id = exe.pkg_id"
    "  left join pm on pm.pm_id = pkg.pm_id"
    "  left join exe delexe on delexe.exe_id = file.exe_id_delete"
    "  left join pkg delpkg on delpkg.pkg_id = delexe.pkg_id"
    "  left join pm delpm on delpm.pm_id = delpkg.pm_id"
    "  where ? or file.file_exist;", "i", opt_all != NULL);
  if (!stmt) { fprintf(stderr, "failed to prepare sql\n"); return 1; }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    int exist;
    const char *file_path, *exe_path, *pkg_name, *pm_name,
      *delexe_path, *delpkg_name, *delpm_name;
    int64_t time_create, time_delete;
    db_stmt_columns(stmt, "isII sss sss", &exist, &file_path, &time_create, &time_delete,
      &exe_path, &pkg_name, &pm_name, &delexe_path, &delpkg_name, &delpm_name);

    // TODO buffered output
    printf("%s", file_path);
    if (!exist) {
      printf(" (deleted");
      if (delexe_path) printf(" by %s", delexe_path);
      if (delpkg_name) printf(" [%s:%s]", delpm_name, delpkg_name);
      if (time_delete) printf(" @ %s", epochsec_localstring(time_delete));
      printf(")");
    }
    printf(": %s", exe_path? exe_path: "(unknown)");
    if (pkg_name) printf(" [%s:%s]", pm_name, pkg_name);
    if (time_create) printf(" @ %s", epochsec_localstring(time_create));
    puts("");
  }
  sqlite3_finalize(stmt);

  sqlite3_close(db);

  return 0;
}

int main_show(int argc, const char **argv) {
  const char **opt_organise_packages = NULL, **opt_all = NULL;
  struct tinycli_option opts[] = {
    { "packages", 'p', 0, &opt_organise_packages },
    { "all", 'a', 0, &opt_all },
    { NULL }
  };

  int cliopts_len = tinycli_parse(opts, --argc, ++argv);
  if (cliopts_len < 0) {
    fprintf(stderr, "invalid argument around %s\n", argv[-cliopts_len - 1]);
    print_help(); return 1;
  }
  argc -= cliopts_len; argv += cliopts_len;

  sqlite3 *db = db_open(database_pathname);
  if (!db) {
    fprintf(stderr, "failed to open sqlite database %s\n", database_pathname);
    return 1;
  }

  int last_exe_id = 0, last_pkg_id = 0;
  do {
    const char *exe_path = NULL, *pkg_name = NULL, *pm_name = NULL;
    if (argc) {
      const char *arg = *argv++; --argc;
      if (*arg == '/') exe_path = arg;
      else {
        /* pkg_name with optional pm_name */
        static char name[PATH_MAX]; strncpy(name, arg, sizeof(name));
        pkg_name = name;
        char *colon = strchr(pkg_name, ':');
        if (colon) { *colon = 0; pm_name = pkg_name; pkg_name = colon + 1; }
      }
    }

    /*
      sqlite3 will mock an automatic index when joining a non-pk column
      for instance,
      | select * from pkg left join exe on exe.pkg_id = pkg.pkg_id;
      where sqlite3 needs to do a full scan on exe to get matched rows
      so optimizations on [pm:]pkg queries are not preferred for now

      https://stackoverflow.com/questions/39128296
    */
    sqlite3_stmt *stmt = db_stmt(db,
      "select exe.exe_id, exe.exe_path, exe.pkg_id, pkg.pkg_name, pm.pm_name from exe"
      "  left join pkg on pkg.pkg_id = exe.pkg_id"
      "  left join pm on pm.pm_id = pkg.pm_id"
      "  where (?1 is null or ?1 = exe.exe_path) and (?2 is null or ?2 = pkg.pkg_name) and (?3 is null or ?3 = pm.pm_name);",
      "sss", exe_path, pkg_name, pm_name);
    if (!stmt) { fprintf(stderr, "failed to prepare sql query\n"); return 1; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
      int exe_id, pkg_id; const char *exe_path, *pkg_name, *pm_name;
      db_stmt_columns(stmt, "isiss", &exe_id, &exe_path, &pkg_id, &pkg_name, &pm_name);

      if (opt_organise_packages && pkg_id) {
        if (last_pkg_id != pkg_id)
          printf("\n[%s:%s]\n", pm_name, pkg_name);
      }
      else {
        if (exe_id != last_exe_id) {
          printf("\n%s", exe_path);
          if (pkg_name) printf(" [%s:%s]", pm_name, pkg_name);
          puts(":");
        }
      }
      last_exe_id = exe_id; last_pkg_id = pkg_id;

      sqlite3_stmt *file_stmt = db_stmt(db,
        "select file_exist, file_path, exe_id_create, exe_id_delete from file"
        "  where exe_id_create = ?1 or exe_id_delete = ?1;", "i", exe_id);
      if (!file_stmt) { fprintf(stderr, "failed to prepare sql query\n"); continue; }

      while (sqlite3_step(file_stmt) == SQLITE_ROW) {
        int exist, exe_id_create, exe_id_delete; const char *file_path;
        db_stmt_columns(file_stmt, "isii", &exist, &file_path, &exe_id_create, &exe_id_delete);

        static char ops[4] = {}; char *o = ops;
        if (exe_id == exe_id_create) *o++ = 'C';
        if (exe_id == exe_id_delete) *o++ = 'D';
        *o++ = 0;
        printf("%s %s\n", ops, file_path);
      }
      sqlite3_finalize(file_stmt);
    }
    sqlite3_finalize(stmt);
  } while (argc);
  sqlite3_close(db);

  return 0;
}

struct main_command {
  const char *name; char alias;
  int (*func)(int argc, const char **argv);
} commands[] = {
  { "monitor", 'm', main_monitor },
  { "list", 'l', main_list },
  { "show", 's', main_show },
  { NULL }
};

int main(int argc, const char **argv) {
  sqlite3_config(SQLITE_CONFIG_LOG, db_logerror, NULL);

  const char **opt_help = NULL, **opt_database = NULL, **opt_pm = NULL;
  struct tinycli_option opts[] = {
    { "help", 'h', 0, &opt_help },
    { "database", 'D', 1, &opt_database },
    { "pm", 'p', 1, &opt_pm },
    { NULL }
  };

  int cliopts_len = tinycli_parse(opts, --argc, ++argv);
  if (cliopts_len < 0) {
    fprintf(stderr, "invalid argument around %s\n", argv[-cliopts_len - 1]);
    print_help(); return 1;
  }

  if (opt_help) { print_help(); return 0; }

  if (opt_database) { database_pathname = *opt_database; }
  else {
    const char *path = getenv("FAP_DB");
    if (path && *path) database_pathname = path;
  }

  if (opt_pm) pm_name = opt_pm[1];
  else pm_name = getenv("FAP_PM");
  if (pm_name && !*pm_name) pm_name = NULL;

  argc -= cliopts_len; argv += cliopts_len;
  if (argc <= 0) {
    fprintf(stderr, "needs a command\n"); print_help(); return 1;
  }

  const char *command = argv[0];
  if (command[0]) for (struct main_command *cmd = commands; cmd->name; ++cmd) {
    if (!command[1]) {
      if (cmd->alias && command[0] == cmd->alias) return cmd->func(argc, argv);
    }
    else if (!strcmp(command, cmd->name)) return cmd->func(argc, argv);
  }

  fprintf(stderr, "unrecognized command: %s\n", command);
  print_help();
  return 1;
}


