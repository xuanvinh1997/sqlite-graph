/*
** sqlite-graph: graph traversal table-valued functions over ordinary edge tables.
**
**   graph_walk(edges, src, dst, seeds [, max_depth, direction, filters, row_limit])
**       -> node, depth, parent        breadth-first, minimum depth per node
**   graph_ppr(edges, src, dst, seeds [, direction, filters, alpha, iterations, row_limit])
**       -> node, score                personalized PageRank, scores sum to ~1
**
** Identifiers are quoted, never spliced; filter values are bound, never spliced.
*/
#ifdef SQLITE_CORE
#include <sqlite3.h>
#else
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite_graph.h"

#define MAX_SLOTS 9
#define DEFAULT_ROW_LIMIT 10000
#define MAX_DEPTH_CAP 1000

/* ---------------------------------------------------------------- vectors */

typedef struct {
  int *v;
  int n, cap;
} IntVec;

static int ivec_push(IntVec *a, int x) {
  if (a->n == a->cap) {
    int ncap = a->cap ? a->cap * 2 : 64;
    int *nv = sqlite3_realloc64(a->v, (sqlite3_uint64)ncap * sizeof(int));
    if (!nv) return SQLITE_NOMEM;
    a->v = nv;
    a->cap = ncap;
  }
  a->v[a->n++] = x;
  return SQLITE_OK;
}

static void ivec_free(IntVec *a) {
  sqlite3_free(a->v);
  a->v = 0;
  a->n = a->cap = 0;
}

/* --------------------------------------------------------------- interner */

typedef struct {
  char **name;
  int n, cap;
  int *tab;
  int tcap;
} Interner;

static unsigned hash_str(const char *s) {
  unsigned h = 2166136261u;
  for (; *s; s++) {
    h ^= (unsigned char)*s;
    h *= 16777619u;
  }
  return h;
}

static int in_rehash(Interner *in, int newcap) {
  int *t = sqlite3_malloc64((sqlite3_uint64)newcap * sizeof(int));
  int i, j;
  if (!t) return SQLITE_NOMEM;
  for (i = 0; i < newcap; i++) t[i] = -1;
  for (i = 0; i < in->n; i++) {
    j = (int)(hash_str(in->name[i]) % (unsigned)newcap);
    while (t[j] >= 0) j = (j + 1) % newcap;
    t[j] = i;
  }
  sqlite3_free(in->tab);
  in->tab = t;
  in->tcap = newcap;
  return SQLITE_OK;
}

/* Index of `s`, adding it when absent. Returns -1 on out-of-memory. */
static int in_intern(Interner *in, const char *s, int *isNew) {
  int j;
  char *copy;
  if (in->tcap == 0 || (in->n + 1) * 2 > in->tcap) {
    if (in_rehash(in, in->tcap ? in->tcap * 2 : 64) != SQLITE_OK) return -1;
  }
  j = (int)(hash_str(s) % (unsigned)in->tcap);
  while (in->tab[j] >= 0) {
    if (strcmp(in->name[in->tab[j]], s) == 0) {
      *isNew = 0;
      return in->tab[j];
    }
    j = (j + 1) % in->tcap;
  }
  if (in->n == in->cap) {
    int ncap = in->cap ? in->cap * 2 : 64;
    char **nn = sqlite3_realloc64(in->name, (sqlite3_uint64)ncap * sizeof(char *));
    if (!nn) return -1;
    in->name = nn;
    in->cap = ncap;
  }
  copy = sqlite3_mprintf("%s", s);
  if (!copy) return -1;
  in->name[in->n] = copy;
  in->tab[j] = in->n;
  *isNew = 1;
  return in->n++;
}

static void in_free(Interner *in) {
  int i;
  for (i = 0; i < in->n; i++) sqlite3_free(in->name[i]);
  sqlite3_free(in->name);
  sqlite3_free(in->tab);
  memset(in, 0, sizeof(*in));
}

/* Backticks, not double quotes: an unknown "name" silently becomes a string literal, a `name` is an error. */
static char *qident(const char *s) {
  size_t n = strlen(s), i, j = 0;
  char *out = sqlite3_malloc64((sqlite3_uint64)(2 * n + 3));
  if (!out) return 0;
  out[j++] = '`';
  for (i = 0; i < n; i++) {
    if (s[i] == '`') out[j++] = '`';
    out[j++] = s[i];
  }
  out[j++] = '`';
  out[j] = 0;
  return out;
}

/* ---------------------------------------------------------------- filters */

typedef struct {
  char *sql;   /* " AND "col" = ?N AND "col2" IN (?M,?M+1)" */
  char **vals; /* bound in order starting at first param */
  int nvals, capvals;
} Filters;

static void filters_free(Filters *f) {
  int i;
  for (i = 0; i < f->nvals; i++) sqlite3_free(f->vals[i]);
  sqlite3_free(f->vals);
  sqlite3_free(f->sql);
  memset(f, 0, sizeof(*f));
}

static int filters_addval(Filters *f, const char *text) {
  char *copy;
  if (f->nvals == f->capvals) {
    int ncap = f->capvals ? f->capvals * 2 : 8;
    char **nv = sqlite3_realloc64(f->vals, (sqlite3_uint64)ncap * sizeof(char *));
    if (!nv) return SQLITE_NOMEM;
    f->vals = nv;
    f->capvals = ncap;
  }
  copy = sqlite3_mprintf("%s", text ? text : "");
  if (!copy) return SQLITE_NOMEM;
  f->vals[f->nvals++] = copy;
  return SQLITE_OK;
}

static int append_sql(char **dst, char *piece) {
  if (!piece) return SQLITE_NOMEM;
  *dst = sqlite3_mprintf("%z%z", *dst, piece);
  return *dst ? SQLITE_OK : SQLITE_NOMEM;
}

/* Parses {"col": value | [values]} into an AND-ed predicate; parameters start at `first`. */
static int filters_build(sqlite3 *db, const char *json, int first, Filters *f, char **err) {
  sqlite3_stmt *outer = 0;
  int rc;
  f->sql = sqlite3_mprintf("%s", "");
  if (!f->sql) return SQLITE_NOMEM;
  if (!json || !*json) return SQLITE_OK;
  rc = sqlite3_prepare_v2(db, "SELECT key, type, value FROM json_each(?1)", -1, &outer, 0);
  if (rc != SQLITE_OK) {
    *err = sqlite3_mprintf("graph: JSON support is required for filters (%s)", sqlite3_errmsg(db));
    return SQLITE_ERROR;
  }
  sqlite3_bind_text(outer, 1, json, -1, SQLITE_STATIC);
  while ((rc = sqlite3_step(outer)) == SQLITE_ROW) {
    const char *key = (const char *)sqlite3_column_text(outer, 0);
    const char *type = (const char *)sqlite3_column_text(outer, 1);
    const char *value = (const char *)sqlite3_column_text(outer, 2);
    char *qkey;
    if (!key || !type || !*key) {
      *err = sqlite3_mprintf("graph: filters must be a JSON object of column -> value");
      rc = SQLITE_ERROR;
      goto done;
    }
    qkey = qident(key);
    if (!qkey) {
      rc = SQLITE_NOMEM;
      goto done;
    }
    if (strcmp(type, "array") == 0) {
      sqlite3_stmt *inner = 0;
      int count = 0;
      rc = sqlite3_prepare_v2(db, "SELECT value FROM json_each(?1)", -1, &inner, 0);
      if (rc != SQLITE_OK) {
        sqlite3_free(qkey);
        goto done;
      }
      rc = append_sql(&f->sql, sqlite3_mprintf(" AND %s IN (", qkey));
      sqlite3_bind_text(inner, 1, value, -1, SQLITE_STATIC);
      while (rc == SQLITE_OK && sqlite3_step(inner) == SQLITE_ROW) {
        rc = append_sql(&f->sql, sqlite3_mprintf("%s?%d", count ? "," : "", first + f->nvals));
        if (rc == SQLITE_OK) rc = filters_addval(f, (const char *)sqlite3_column_text(inner, 0));
        count++;
      }
      sqlite3_finalize(inner);
      if (rc == SQLITE_OK) rc = append_sql(&f->sql, sqlite3_mprintf(")"));
    } else if (strcmp(type, "object") == 0 || strcmp(type, "null") == 0) {
      *err = sqlite3_mprintf("graph: filter \"%s\" must be a scalar or an array", key);
      sqlite3_free(qkey);
      rc = SQLITE_ERROR;
      goto done;
    } else {
      rc = append_sql(&f->sql, sqlite3_mprintf(" AND %s = ?%d", qkey, first + f->nvals));
      if (rc == SQLITE_OK) rc = filters_addval(f, value);
    }
    sqlite3_free(qkey);
    if (rc != SQLITE_OK) goto done;
  }
  if (rc == SQLITE_DONE) rc = SQLITE_OK;
  else if (rc != SQLITE_OK && !*err) *err = sqlite3_mprintf("graph: invalid filters JSON (%s)", sqlite3_errmsg(db));
done:
  sqlite3_finalize(outer);
  return rc;
}

static void filters_bind(sqlite3_stmt *st, int first, const Filters *f) {
  int i;
  for (i = 0; i < f->nvals; i++) sqlite3_bind_text(st, first + i, f->vals[i], -1, SQLITE_STATIC);
}

/* ------------------------------------------------------------- parameters */

typedef enum { DIR_OUT, DIR_IN, DIR_BOTH } Direction;

typedef struct {
  const char *edges, *src, *dst, *seeds, *filters;
  Direction dir;
  int max_depth;
  sqlite3_int64 row_limit;
  double alpha;
  int iterations;
} Params;

static int parse_direction(const char *s, Direction *d, char **err) {
  if (!s || strcmp(s, "both") == 0) *d = DIR_BOTH;
  else if (strcmp(s, "out") == 0) *d = DIR_OUT;
  else if (strcmp(s, "in") == 0) *d = DIR_IN;
  else {
    *err = sqlite3_mprintf("graph: direction must be 'out', 'in' or 'both', got '%s'", s);
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}

static int check_identifiers(const Params *p, char **err) {
  if (!p->edges || !*p->edges || !p->src || !*p->src || !p->dst || !*p->dst) {
    *err = sqlite3_mprintf("graph: edges, src and dst must be non-empty names");
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}

/* Seeds are a JSON array of ids, or one bare id. */
static int load_seeds(sqlite3 *db, const char *seeds, Interner *in, IntVec *order, sqlite3_int64 limit, char **err) {
  const char *p = seeds;
  int isNew, idx, rc = SQLITE_OK;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (*p == '[') {
    sqlite3_stmt *st = 0;
    rc = sqlite3_prepare_v2(db, "SELECT value FROM json_each(?1) WHERE type NOT IN ('null','array','object')", -1, &st, 0);
    if (rc != SQLITE_OK) {
      *err = sqlite3_mprintf("graph: seeds JSON needs JSON support (%s)", sqlite3_errmsg(db));
      return SQLITE_ERROR;
    }
    sqlite3_bind_text(st, 1, seeds, -1, SQLITE_STATIC);
    while (in->n < limit && (rc = sqlite3_step(st)) == SQLITE_ROW) {
      const char *id = (const char *)sqlite3_column_text(st, 0);
      if (!id) continue;
      idx = in_intern(in, id, &isNew);
      if (idx < 0) {
        rc = SQLITE_NOMEM;
        break;
      }
      if (isNew && ivec_push(order, idx) != SQLITE_OK) {
        rc = SQLITE_NOMEM;
        break;
      }
    }
    sqlite3_finalize(st);
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) rc = SQLITE_OK;
    if (rc == SQLITE_ERROR && !*err) *err = sqlite3_mprintf("graph: seeds is not valid JSON");
    return rc;
  }
  idx = in_intern(in, seeds, &isNew);
  if (idx < 0) return SQLITE_NOMEM;
  if (isNew) rc = ivec_push(order, idx);
  return rc;
}

/* ----------------------------------------------------------------- cursor */

typedef struct {
  sqlite3_vtab base;
  sqlite3 *db;
  int ppr;
} GVtab;

typedef struct {
  sqlite3_vtab_cursor base;
  int ppr;
  Interner in;
  IntVec order; /* row -> node index */
  int *depth;   /* walk */
  int *parent;  /* walk */
  double *score; /* ppr, by node index */
  int row;
} GCursor;

static void cursor_clear(GCursor *c) {
  in_free(&c->in);
  ivec_free(&c->order);
  sqlite3_free(c->depth);
  sqlite3_free(c->parent);
  sqlite3_free(c->score);
  c->depth = c->parent = 0;
  c->score = 0;
  c->row = 0;
}

static void set_err(GVtab *v, char **err) {
  if (*err) {
    sqlite3_free(v->base.zErrMsg);
    v->base.zErrMsg = *err;
    *err = 0;
  }
}

/* ------------------------------------------------------------------- walk */

static int run_walk(GVtab *v, GCursor *c, const Params *p, char **err) {
  sqlite3 *db = v->db;
  Filters f;
  sqlite3_stmt *st[2] = {0, 0};
  int nst = 0, i, rc;
  char *qe = 0, *qs = 0, *qd = 0;
  IntVec depth = {0, 0, 0}, parent = {0, 0, 0};
  int head;

  memset(&f, 0, sizeof(f));
  rc = filters_build(db, p->filters, 2, &f, err);
  if (rc != SQLITE_OK) goto done;
  qe = qident(p->edges);
  qs = qident(p->src);
  qd = qident(p->dst);
  if (!qe || !qs || !qd) {
    rc = SQLITE_NOMEM;
    goto done;
  }
  if (p->dir != DIR_IN) {
    char *sql = sqlite3_mprintf("SELECT %s FROM %s WHERE %s = ?1%s", qd, qe, qs, f.sql);
    if (!sql) { rc = SQLITE_NOMEM; goto done; }
    rc = sqlite3_prepare_v2(db, sql, -1, &st[nst], 0);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) { *err = sqlite3_mprintf("graph: %s", sqlite3_errmsg(db)); goto done; }
    filters_bind(st[nst], 2, &f);
    nst++;
  }
  if (p->dir != DIR_OUT) {
    char *sql = sqlite3_mprintf("SELECT %s FROM %s WHERE %s = ?1%s", qs, qe, qd, f.sql);
    if (!sql) { rc = SQLITE_NOMEM; goto done; }
    rc = sqlite3_prepare_v2(db, sql, -1, &st[nst], 0);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) { *err = sqlite3_mprintf("graph: %s", sqlite3_errmsg(db)); goto done; }
    filters_bind(st[nst], 2, &f);
    nst++;
  }

  rc = load_seeds(db, p->seeds, &c->in, &c->order, p->row_limit, err);
  if (rc != SQLITE_OK) goto done;
  for (i = 0; i < c->in.n; i++) {
    if (ivec_push(&depth, 0) || ivec_push(&parent, -1)) { rc = SQLITE_NOMEM; goto done; }
  }

  for (head = 0; head < c->in.n; head++) {
    int s;
    if (depth.v[head] >= p->max_depth) continue;
    for (s = 0; s < nst; s++) {
      sqlite3_reset(st[s]);
      sqlite3_bind_text(st[s], 1, c->in.name[head], -1, SQLITE_STATIC);
      while ((rc = sqlite3_step(st[s])) == SQLITE_ROW) {
        const char *nb = (const char *)sqlite3_column_text(st[s], 0);
        int isNew, idx;
        if (!nb || c->in.n >= p->row_limit) continue;
        idx = in_intern(&c->in, nb, &isNew);
        if (idx < 0) { rc = SQLITE_NOMEM; goto done; }
        if (isNew) {
          if (ivec_push(&c->order, idx) || ivec_push(&depth, depth.v[head] + 1) || ivec_push(&parent, head)) {
            rc = SQLITE_NOMEM;
            goto done;
          }
        }
      }
      if (rc != SQLITE_DONE) { *err = sqlite3_mprintf("graph: %s", sqlite3_errmsg(db)); rc = SQLITE_ERROR; goto done; }
    }
  }
  rc = SQLITE_OK;
  c->depth = depth.v;
  c->parent = parent.v;
  depth.v = parent.v = 0;
done:
  for (i = 0; i < nst; i++) sqlite3_finalize(st[i]);
  filters_free(&f);
  ivec_free(&depth);
  ivec_free(&parent);
  sqlite3_free(qe);
  sqlite3_free(qs);
  sqlite3_free(qd);
  return rc;
}

/* -------------------------------------------------------------------- ppr */

typedef struct {
  int idx;
  double score;
  const char *name;
} Ranked;

static int ranked_cmp(const void *a, const void *b) {
  const Ranked *x = a, *y = b;
  if (x->score > y->score) return -1;
  if (x->score < y->score) return 1;
  return strcmp(x->name, y->name);
}

static int run_ppr(GVtab *v, GCursor *c, const Params *p, char **err) {
  sqlite3 *db = v->db;
  Filters f;
  sqlite3_stmt *st = 0;
  char *qe = 0, *qs = 0, *qd = 0, *sql = 0;
  IntVec eu = {0, 0, 0}, ev = {0, 0, 0}, seedIdx = {0, 0, 0};
  int *off = 0, *adj = 0, *cur = 0;
  double *r = 0, *nx = 0, *p0 = 0;
  Ranked *rank = 0;
  int n, i, e, it, rc;

  memset(&f, 0, sizeof(f));
  rc = filters_build(db, p->filters, 1, &f, err);
  if (rc != SQLITE_OK) goto done;
  qe = qident(p->edges);
  qs = qident(p->src);
  qd = qident(p->dst);
  if (!qe || !qs || !qd) { rc = SQLITE_NOMEM; goto done; }
  rc = load_seeds(db, p->seeds, &c->in, &seedIdx, p->row_limit < 1000000 ? 1000000 : p->row_limit, err);
  if (rc != SQLITE_OK) goto done;

  sql = sqlite3_mprintf("SELECT %s, %s FROM %s WHERE 1=1%s", qs, qd, qe, f.sql);
  if (!sql) { rc = SQLITE_NOMEM; goto done; }
  rc = sqlite3_prepare_v2(db, sql, -1, &st, 0);
  if (rc != SQLITE_OK) { *err = sqlite3_mprintf("graph: %s", sqlite3_errmsg(db)); goto done; }
  filters_bind(st, 1, &f);
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    const char *a = (const char *)sqlite3_column_text(st, 0);
    const char *b = (const char *)sqlite3_column_text(st, 1);
    int isNew, ia, ib;
    if (!a || !b) continue;
    ia = in_intern(&c->in, a, &isNew);
    ib = in_intern(&c->in, b, &isNew);
    if (ia < 0 || ib < 0) { rc = SQLITE_NOMEM; goto done; }
    if (p->dir != DIR_IN && (ivec_push(&eu, ia) || ivec_push(&ev, ib))) { rc = SQLITE_NOMEM; goto done; }
    if (p->dir != DIR_OUT && (ivec_push(&eu, ib) || ivec_push(&ev, ia))) { rc = SQLITE_NOMEM; goto done; }
  }
  if (rc != SQLITE_DONE) { *err = sqlite3_mprintf("graph: %s", sqlite3_errmsg(db)); rc = SQLITE_ERROR; goto done; }
  rc = SQLITE_OK;

  n = c->in.n;
  off = sqlite3_malloc64((sqlite3_uint64)(n + 1) * sizeof(int));
  cur = sqlite3_malloc64((sqlite3_uint64)(n + 1) * sizeof(int));
  adj = sqlite3_malloc64((sqlite3_uint64)(eu.n ? eu.n : 1) * sizeof(int));
  r = sqlite3_malloc64((sqlite3_uint64)(n ? n : 1) * sizeof(double));
  nx = sqlite3_malloc64((sqlite3_uint64)(n ? n : 1) * sizeof(double));
  p0 = sqlite3_malloc64((sqlite3_uint64)(n ? n : 1) * sizeof(double));
  rank = sqlite3_malloc64((sqlite3_uint64)(n ? n : 1) * sizeof(Ranked));
  if (!off || !cur || !adj || !r || !nx || !p0 || !rank) { rc = SQLITE_NOMEM; goto done; }

  memset(off, 0, (size_t)(n + 1) * sizeof(int));
  for (e = 0; e < eu.n; e++) off[eu.v[e] + 1]++;
  for (i = 0; i < n; i++) off[i + 1] += off[i];
  memcpy(cur, off, (size_t)(n + 1) * sizeof(int));
  for (e = 0; e < eu.n; e++) adj[cur[eu.v[e]]++] = ev.v[e];

  for (i = 0; i < n; i++) p0[i] = 0.0;
  for (i = 0; i < seedIdx.n; i++) p0[seedIdx.v[i]] = 1.0 / (double)seedIdx.n;
  for (i = 0; i < n; i++) r[i] = p0[i];

  for (it = 0; it < p->iterations; it++) {
    double dangling = 0.0, diff = 0.0;
    for (i = 0; i < n; i++) nx[i] = 0.0;
    for (i = 0; i < n; i++) {
      int deg = off[i + 1] - off[i];
      if (deg == 0) {
        dangling += r[i];
      } else {
        double share = (1.0 - p->alpha) * r[i] / (double)deg;
        for (e = off[i]; e < off[i + 1]; e++) nx[adj[e]] += share;
      }
    }
    for (i = 0; i < n; i++) {
      nx[i] += (p->alpha + (1.0 - p->alpha) * dangling) * p0[i];
      diff += fabs(nx[i] - r[i]);
    }
    memcpy(r, nx, (size_t)n * sizeof(double));
    if (diff < 1e-12) break;
  }

  for (i = 0; i < n; i++) {
    rank[i].idx = i;
    rank[i].score = r[i];
    rank[i].name = c->in.name[i];
  }
  qsort(rank, (size_t)n, sizeof(Ranked), ranked_cmp);
  for (i = 0; i < n && i < p->row_limit; i++) {
    if (rank[i].score <= 0.0) break;
    if (ivec_push(&c->order, rank[i].idx)) { rc = SQLITE_NOMEM; goto done; }
  }
  c->score = r;
  r = 0;
done:
  sqlite3_finalize(st);
  filters_free(&f);
  ivec_free(&eu);
  ivec_free(&ev);
  ivec_free(&seedIdx);
  sqlite3_free(off);
  sqlite3_free(cur);
  sqlite3_free(adj);
  sqlite3_free(r);
  sqlite3_free(nx);
  sqlite3_free(p0);
  sqlite3_free(rank);
  sqlite3_free(qe);
  sqlite3_free(qs);
  sqlite3_free(qd);
  sqlite3_free(sql);
  return rc;
}

/* ----------------------------------------------------------------- module */

typedef struct {
  const char *decl;
  int h0;   /* first hidden column */
  int nh;   /* hidden column count */
  int nreq; /* leading hidden columns that must be supplied */
} Spec;

static const Spec WALK_SPEC = {
    "CREATE TABLE x(node TEXT, depth INTEGER, parent TEXT, edges HIDDEN, src HIDDEN, dst HIDDEN, "
    "seeds HIDDEN, max_depth HIDDEN, direction HIDDEN, filters HIDDEN, row_limit HIDDEN)",
    3, 8, 4};
static const Spec PPR_SPEC = {
    "CREATE TABLE x(node TEXT, score REAL, edges HIDDEN, src HIDDEN, dst HIDDEN, seeds HIDDEN, "
    "direction HIDDEN, filters HIDDEN, alpha HIDDEN, iterations HIDDEN, row_limit HIDDEN)",
    2, 9, 4};

static int g_connect(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **out, char **err) {
  const Spec *spec = aux;
  GVtab *v;
  int rc;
  (void)argc;
  (void)argv;
  (void)err;
  rc = sqlite3_declare_vtab(db, spec->decl);
  if (rc != SQLITE_OK) return rc;
  v = sqlite3_malloc64(sizeof(*v));
  if (!v) return SQLITE_NOMEM;
  memset(v, 0, sizeof(*v));
  v->db = db;
  v->ppr = spec == &PPR_SPEC;
  *out = &v->base;
  return SQLITE_OK;
}

static int g_disconnect(sqlite3_vtab *vt) {
  sqlite3_free(vt);
  return SQLITE_OK;
}

static int g_best_index(sqlite3_vtab *vt, sqlite3_index_info *info) {
  GVtab *v = (GVtab *)vt;
  const Spec *spec = v->ppr ? &PPR_SPEC : &WALK_SPEC;
  int slot_cons[MAX_SLOTS], i, argc = 0, mask = 0;
  for (i = 0; i < MAX_SLOTS; i++) slot_cons[i] = -1;
  for (i = 0; i < info->nConstraint; i++) {
    const struct sqlite3_index_constraint *c = &info->aConstraint[i];
    int slot = c->iColumn - spec->h0;
    if (!c->usable || c->op != SQLITE_INDEX_CONSTRAINT_EQ || slot < 0 || slot >= spec->nh) continue;
    slot_cons[slot] = i;
  }
  for (i = 0; i < spec->nh; i++) {
    if (slot_cons[i] < 0) continue;
    info->aConstraintUsage[slot_cons[i]].argvIndex = ++argc;
    info->aConstraintUsage[slot_cons[i]].omit = 1;
    mask |= 1 << i;
  }
  if ((mask & ((1 << spec->nreq) - 1)) != (1 << spec->nreq) - 1) return SQLITE_CONSTRAINT;
  info->idxNum = mask;
  info->estimatedCost = 1000.0;
  info->estimatedRows = 100;
  return SQLITE_OK;
}

static int g_open(sqlite3_vtab *vt, sqlite3_vtab_cursor **out) {
  GCursor *c = sqlite3_malloc64(sizeof(*c));
  if (!c) return SQLITE_NOMEM;
  memset(c, 0, sizeof(*c));
  c->ppr = ((GVtab *)vt)->ppr;
  *out = &c->base;
  return SQLITE_OK;
}

static int g_close(sqlite3_vtab_cursor *cur) {
  cursor_clear((GCursor *)cur);
  sqlite3_free(cur);
  return SQLITE_OK;
}

static const char *arg_text(sqlite3_value *v) {
  return v && sqlite3_value_type(v) != SQLITE_NULL ? (const char *)sqlite3_value_text(v) : 0;
}

static int g_filter(sqlite3_vtab_cursor *cur, int idxNum, const char *idxStr, int argc, sqlite3_value **argv) {
  GCursor *c = (GCursor *)cur;
  GVtab *v = (GVtab *)cur->pVtab;
  const Spec *spec = v->ppr ? &PPR_SPEC : &WALK_SPEC;
  sqlite3_value *a[MAX_SLOTS];
  Params p;
  char *err = 0;
  int slot, k = 0, rc;
  (void)idxStr;
  (void)argc;

  cursor_clear(c);
  for (slot = 0; slot < MAX_SLOTS; slot++) a[slot] = (slot < spec->nh && (idxNum & (1 << slot))) ? argv[k++] : 0;

  memset(&p, 0, sizeof(p));
  p.edges = arg_text(a[0]);
  p.src = arg_text(a[1]);
  p.dst = arg_text(a[2]);
  p.seeds = arg_text(a[3]);
  p.row_limit = DEFAULT_ROW_LIMIT;
  if (!p.seeds) p.seeds = "[]";
  if (!v->ppr) {
    p.max_depth = a[4] && sqlite3_value_type(a[4]) != SQLITE_NULL ? sqlite3_value_int(a[4]) : 1;
    if (p.max_depth < 0) p.max_depth = 0;
    if (p.max_depth > MAX_DEPTH_CAP) p.max_depth = MAX_DEPTH_CAP;
    rc = parse_direction(arg_text(a[5]), &p.dir, &err);
    p.filters = arg_text(a[6]);
    if (a[7] && sqlite3_value_type(a[7]) != SQLITE_NULL) p.row_limit = sqlite3_value_int64(a[7]);
  } else {
    rc = parse_direction(arg_text(a[4]), &p.dir, &err);
    p.filters = arg_text(a[5]);
    p.alpha = a[6] && sqlite3_value_type(a[6]) != SQLITE_NULL ? sqlite3_value_double(a[6]) : 0.15;
    p.iterations = a[7] && sqlite3_value_type(a[7]) != SQLITE_NULL ? sqlite3_value_int(a[7]) : 30;
    if (a[8] && sqlite3_value_type(a[8]) != SQLITE_NULL) p.row_limit = sqlite3_value_int64(a[8]);
    if (rc == SQLITE_OK && (!(p.alpha > 0.0) || p.alpha >= 1.0)) {
      err = sqlite3_mprintf("graph_ppr: alpha must be in (0, 1)");
      rc = SQLITE_ERROR;
    }
    if (p.iterations < 1) p.iterations = 1;
    if (p.iterations > 10000) p.iterations = 10000;
  }
  if (p.row_limit < 0) p.row_limit = 0;
  if (rc == SQLITE_OK) rc = check_identifiers(&p, &err);
  if (rc == SQLITE_OK) rc = v->ppr ? run_ppr(v, c, &p, &err) : run_walk(v, c, &p, &err);
  if (rc != SQLITE_OK) {
    cursor_clear(c);
    set_err(v, &err);
    if (rc == SQLITE_ERROR && !v->base.zErrMsg) v->base.zErrMsg = sqlite3_mprintf("graph: %s", sqlite3_errmsg(v->db));
    sqlite3_free(err);
  }
  return rc;
}

static int g_next(sqlite3_vtab_cursor *cur) {
  ((GCursor *)cur)->row++;
  return SQLITE_OK;
}

static int g_eof(sqlite3_vtab_cursor *cur) {
  GCursor *c = (GCursor *)cur;
  return c->row >= c->order.n;
}

static int g_column(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int col) {
  GCursor *c = (GCursor *)cur;
  int node = c->order.v[c->row];
  if (col == 0) {
    sqlite3_result_text(ctx, c->in.name[node], -1, SQLITE_TRANSIENT);
  } else if (c->ppr) {
    if (col == 1) sqlite3_result_double(ctx, c->score[node]);
  } else if (col == 1) {
    sqlite3_result_int(ctx, c->depth[node]);
  } else if (col == 2 && c->parent[node] >= 0) {
    sqlite3_result_text(ctx, c->in.name[c->parent[node]], -1, SQLITE_TRANSIENT);
  }
  return SQLITE_OK;
}

static int g_rowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *rowid) {
  *rowid = ((GCursor *)cur)->row;
  return SQLITE_OK;
}

static sqlite3_module graph_module = {
    0, 0, g_connect, g_best_index, g_disconnect, 0, g_open, g_close, g_filter, g_next, g_eof, g_column, g_rowid,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

int sqlite3_graph_register(sqlite3 *db) {
  int rc = sqlite3_create_module(db, "graph_walk", &graph_module, (void *)&WALK_SPEC);
  if (rc == SQLITE_OK) rc = sqlite3_create_module(db, "graph_ppr", &graph_module, (void *)&PPR_SPEC);
  return rc;
}

#ifdef SQLITE_CORE
#if defined(__clang__) && defined(__APPLE__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
static int graph_auto_init(sqlite3 *db, char **err, const sqlite3_api_routines *api) {
  (void)err;
  (void)api;
  return sqlite3_graph_register(db);
}

int sqlite3_graph_auto_register(void) {
  return sqlite3_auto_extension((void (*)(void))graph_auto_init);
}
#else
#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_sqlitegraph_init(sqlite3 *db, char **err, const sqlite3_api_routines *api) {
  (void)err;
  SQLITE_EXTENSION_INIT2(api);
  return sqlite3_graph_register(db);
}
#endif
