# sqlite-graph

Graph traversal for SQLite, over the edge tables you already have. No new storage format, no server:
two table-valued functions written in C that read an ordinary `(source, target)` table.

| Function | Returns | Use it for |
| --- | --- | --- |
| `graph_walk(edges, src, dst, seeds [, max_depth, direction, filters, row_limit])` | `node, depth, parent` | Neighbourhood expansion: everything within N hops, minimum depth per node |
| `graph_ppr(edges, src, dst, seeds [, direction, filters, alpha, iterations, row_limit])` | `node, score` | Personalized PageRank: rank nodes by how strongly they connect to the seeds (graph-based retrieval, HippoRAG-style) |

```sql
CREATE TABLE edges(id INTEGER PRIMARY KEY, project_id TEXT, source_id TEXT, target_id TEXT, relation TEXT);
CREATE INDEX edges_src ON edges(source_id);   -- graph_walk looks edges up by endpoint: index both
CREATE INDEX edges_dst ON edges(target_id);

-- Two hops out of "a", inside one project, following only some relations.
SELECT node, depth, parent
FROM graph_walk('edges', 'source_id', 'target_id',
                'a',                                      -- seeds: one id, or '["a","b"]'
                2,                                        -- max_depth (default 1)
                'out',                                    -- 'out' | 'in' | 'both' (default)
                '{"project_id":"p1","relation":["supports","defines"]}');

-- Which nodes matter most to a question that matched nodes a and e?
SELECT node, score
FROM graph_ppr('edges', 'source_id', 'target_id', '["a","e"]', 'both',
               '{"project_id":"p1"}', 0.15, 30, 20);
```

## Semantics

- **`graph_walk`** is breadth-first, so `depth` is the minimum hop count. Seeds come back at depth 0 even when
  they have no edges. Rows are in discovery order; `parent` is the node that reached it first.
- **`graph_ppr`** restarts at the seeds with probability `alpha` (default 0.15), iterates up to `iterations`
  (default 30, stops early when converged), and sends dangling-node mass back to the seeds. Scores sum to 1;
  unreachable nodes are omitted. Parallel edges count as weight. Rows are ordered by score, ties by node id.
- **`filters`** is a JSON object `{"column": value | [values]}`, AND-ed together. Values are bound parameters.
- **`row_limit`** defaults to 10 000. `max_depth` is clamped to 0…1000.
- Node ids are compared as text, so integer and text id columns both work.

## Safety

Table and column names are quoted as identifiers (backticks, so a misspelled column is an error rather than
SQLite's silent "double-quoted string literal" fallback), and filter values are bound, never spliced into SQL.
Table/column names are still chosen by the caller: do not pass untrusted input as `edges`/`src`/`dst`.

## Use it

**Statically linked (iOS, macOS, anything that ships its own SQLite).** Add `src/sqlite_graph.c` and
`include/` to your build with `SQLITE_CORE` defined, or depend on the Swift package product `CSQLiteGraph`.

```c
#include "sqlite_graph.h"
sqlite3_graph_auto_register();          // once, before opening connections
// or, per connection:
sqlite3_graph_register(db);
```

`sqlite3_graph_auto_register` uses `sqlite3_auto_extension`, which Apple's system `libsqlite3` ignores; with
the system library call `sqlite3_graph_register` on each connection instead (this is also what you want if a
wrapper such as GRDB opens connections for you: register in its connection-preparation hook).

**Loadable extension** (desktop SQLite with `load_extension` enabled): `make loadable`, then
`.load ./build/libsqlitegraph sqlite3_sqlitegraph_init`. On macOS, Apple's SDK `sqlite3ext.h` compiles the
extension API out (the library would call `sqlite3_*` directly and crash when loaded), so point the build at
real headers: `make loadable SQLITE_INCLUDE=$(brew --prefix sqlite)/include`. `make test-loadable` loads it into
Python's `sqlite3` as a smoke test.

Requires SQLite 3.38+ with JSON functions (built in by default).

## Tests

```
make test                                              # against the system SQLite
make test-amalgamation SQLITE_DIR=/path/to/sqlite3.c/  # against an amalgamation you ship (also covers auto-register)
make test-loadable                                     # loadable build, loaded from Python (macOS: set SQLITE_INCLUDE)
```

The suite covers depth, direction, project/relation filters, limits, error paths, identifier and value
injection, PPR normalisation and ordering. It was also run under AddressSanitizer + UBSan. Performance has not
been benchmarked yet.

## License

MIT
