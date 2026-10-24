#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite_graph.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...)                                  \
  do {                                                    \
    checks++;                                             \
    if (!(cond)) {                                        \
      failures++;                                         \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
    }                                                     \
  } while (0)

static sqlite3 *open_db(void) {
  sqlite3 *db;
  char *err = 0;
  sqlite3_open(":memory:", &db);
  if (sqlite3_graph_register(db) != SQLITE_OK) {
    printf("register failed\n");
    exit(2);
  }
  sqlite3_exec(db,
               "CREATE TABLE graph_edges(id INTEGER PRIMARY KEY, project_id TEXT, source_id TEXT, target_id TEXT, relation TEXT);"
               "CREATE INDEX e_src ON graph_edges(source_id);"
               "CREATE INDEX e_dst ON graph_edges(target_id);"
               "INSERT INTO graph_edges(project_id, source_id, target_id, relation) VALUES"
               " ('p1','a','b','supports'),('p1','b','c','supports'),('p1','b','e','defines'),"
               " ('p1','c','d','extends'),('p2','c','z','supports'),('p1','a','d','related');",
               0, 0, &err);
  if (err) {
    printf("setup: %s\n", err);
    exit(2);
  }
  return db;
}

/* "node:depth" pairs joined by space, ordered by depth then node. */
static char *query_string(sqlite3 *db, const char *sql, char **errOut) {
  sqlite3_stmt *st;
  char buf[2048] = "";
  int rc = sqlite3_prepare_v2(db, sql, -1, &st, 0);
  if (rc != SQLITE_OK) {
    if (errOut) *errOut = strdup(sqlite3_errmsg(db));
    return strdup("");
  }
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    if (buf[0]) strcat(buf, " ");
    strcat(buf, (const char *)sqlite3_column_text(st, 0));
  }
  if (rc != SQLITE_DONE && errOut) *errOut = strdup(sqlite3_errmsg(db));
  sqlite3_finalize(st);
  return strdup(buf);
}

static void expect(sqlite3 *db, const char *sql, const char *want) {
  char *err = 0;
  char *got = query_string(db, sql, &err);
  CHECK(strcmp(got, want) == 0, "\n  sql : %s\n  want: %s\n  got : %s%s%s", sql, want, got, err ? "\n  err : " : "", err ? err : "");
  free(got);
  free(err);
}

static void expect_error(sqlite3 *db, const char *sql, const char *fragment) {
  char *err = 0;
  char *got = query_string(db, sql, &err);
  CHECK(err && strstr(err, fragment), "\n  sql : %s\n  want error containing: %s\n  got: %s", sql, fragment, err ? err : "(no error)");
  free(got);
  free(err);
}

#define WALK(args) "SELECT node || ':' || depth FROM graph_walk('graph_edges','source_id','target_id'," args ") ORDER BY depth, node"

static void test_walk(void) {
  sqlite3 *db = open_db();
  expect(db, WALK("'a',2,'out','{\"project_id\":\"p1\"}'"), "a:0 b:1 d:1 c:2 e:2");
  expect(db, WALK("'a',1,'out'"), "a:0 b:1 d:1");
  expect(db, WALK("'a',0"), "a:0");
  expect(db, WALK("'c',1,'both'"), "c:0 b:1 d:1 z:1");
  expect(db, WALK("'c',1,'both','{\"project_id\":\"p1\"}'"), "c:0 b:1 d:1");
  expect(db, WALK("'c',1,'in'"), "c:0 b:1");
  expect(db, WALK("'a',3,'out','{\"relation\":[\"supports\",\"defines\"]}'"), "a:0 b:1 c:2 e:2 z:3");
  expect(db, WALK("'a',3,'out','{\"project_id\":\"p1\",\"relation\":\"supports\"}'"), "a:0 b:1 c:2");
  expect(db, WALK("'[\"a\",\"e\"]',1,'out'"), "a:0 e:0 b:1 d:1");
  expect(db, WALK("'a',5,'out','{\"relation\":[]}'"), "a:0");
  expect(db, WALK("'ghost',3"), "ghost:0");
  expect(db, WALK("'a',10,'out','{\"project_id\":\"p1\"}',3"), "a:0 b:1 d:1");
  /* minimum depth, not first-found path: d is one hop from a */
  expect(db, "SELECT depth FROM graph_walk('graph_edges','source_id','target_id','a',3,'out') WHERE node='d'", "1");
  /* parent column */
  expect(db, "SELECT parent FROM graph_walk('graph_edges','source_id','target_id','a',2,'out') WHERE node='c'", "b");
  sqlite3_close(db);
}

static void test_walk_errors_and_safety(void) {
  sqlite3 *db = open_db();
  expect_error(db, "SELECT * FROM graph_walk('graph_edges','source_id','target_id')", "");
  expect_error(db, "SELECT * FROM graph_walk('no_such_table','source_id','target_id','a')", "no such table");
  expect_error(db, "SELECT * FROM graph_walk('graph_edges','nope','target_id','a')", "no such column");
  expect_error(db, WALK("'a',1,'sideways'"), "direction");
  expect_error(db, WALK("'a',1,'out','not json'"), "");
  expect_error(db, WALK("'a',1,'out','{\"relation\":{\"x\":1}}'"), "scalar or an array");
  expect_error(db, WALK("'a',1,'out','{\"missing_col\":\"x\"}'"), "no such column");

  /* identifier injection: quoted, so it is just a missing table and nothing is dropped */
  expect_error(db, "SELECT * FROM graph_walk('graph_edges\"; DROP TABLE graph_edges; --','source_id','target_id','a')", "no such table");
  expect(db, "SELECT count(*) FROM graph_edges", "6");
  /* value injection: bound, never spliced */
  expect(db, WALK("'a',2,'out','{\"project_id\":\"p1\\\" OR 1=1 --\"}'"), "a:0");
  sqlite3_close(db);
}

static double score_of(sqlite3 *db, const char *args, const char *node) {
  char sql[1024];
  char *got;
  double v;
  snprintf(sql, sizeof sql, "SELECT score FROM graph_ppr('graph_edges','source_id','target_id',%s) WHERE node='%s'", args, node);
  got = query_string(db, sql, 0);
  v = got[0] ? atof(got) : -1.0;
  free(got);
  return v;
}

static void test_ppr(void) {
  sqlite3 *db = open_db();
  sqlite3_stmt *st;
  double total = 0.0, prev = 2.0;
  int n = 0, ordered = 1;

  sqlite3_prepare_v2(db, "SELECT score FROM graph_ppr('graph_edges','source_id','target_id','a','out','{\"project_id\":\"p1\"}')", -1, &st, 0);
  while (sqlite3_step(st) == SQLITE_ROW) {
    double s = sqlite3_column_double(st, 0);
    total += s;
    if (s > prev) ordered = 0;
    prev = s;
    n++;
  }
  sqlite3_finalize(st);
  CHECK(fabs(total - 1.0) < 1e-6, "scores must sum to 1, got %.9f", total);
  CHECK(ordered, "rows must be ordered by score descending");
  CHECK(n == 5, "a,b,c,d,e reachable, got %d", n);

  CHECK(score_of(db, "'a','out','{\"project_id\":\"p1\"}'", "a") > score_of(db, "'a','out','{\"project_id\":\"p1\"}'", "b"), "seed ranks first");
  CHECK(score_of(db, "'a','out','{\"project_id\":\"p1\"}'", "b") > score_of(db, "'a','out','{\"project_id\":\"p1\"}'", "e"), "nearer ranks higher");
  CHECK(score_of(db, "'a','out','{\"project_id\":\"p1\"}'", "z") < 0, "p2 node is filtered out of the graph");
  CHECK(score_of(db, "'a','out'", "z") > 0, "unfiltered graph reaches z");

  /* a larger restart probability keeps more mass on the seed */
  CHECK(score_of(db, "'a','out',NULL,0.5", "a") > score_of(db, "'a','out',NULL,0.1", "a"), "alpha controls locality");
  /* two seeds split the restart mass */
  CHECK(fabs(score_of(db, "'[\"a\",\"e\"]','out'", "e") - score_of(db, "'e','out'", "e")) > 1e-9, "multi-seed differs from single seed");
  /* isolated seed keeps all mass */
  CHECK(fabs(score_of(db, "'ghost','out'", "ghost") - 1.0) < 1e-9, "isolated seed scores 1");
  expect(db, "SELECT node FROM graph_ppr('graph_edges','source_id','target_id','a','out','{\"project_id\":\"p1\"}',0.15,30,2)", "a d"); /* d has two routes from a */
  expect_error(db, "SELECT * FROM graph_ppr('graph_edges','source_id','target_id','a','out',NULL,1.5)", "alpha");
  sqlite3_close(db);
}

static void test_auto_register(void) {
  sqlite3 *db;
#if defined(__APPLE__) && !defined(SQLITE_GRAPH_OWN_SQLITE)
  /* Apple's libsqlite3 ignores process-global auto extensions; build with SQLITE_GRAPH_OWN_SQLITE to test it. */
  (void)db;
  printf("skip: auto_extension on Apple system SQLite (use `make test-amalgamation`)\n");
#else
  CHECK(sqlite3_graph_auto_register() == SQLITE_OK, "auto register");
  sqlite3_open(":memory:", &db);
  expect(db, "SELECT count(*) FROM graph_walk('sqlite_master','name','name','x')", "1");
  sqlite3_close(db);
#endif
}

int main(void) {
  test_walk();
  test_walk_errors_and_safety();
  test_ppr();
  test_auto_register();
  printf("%d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
