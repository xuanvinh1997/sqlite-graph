#ifndef SQLITE_GRAPH_H
#define SQLITE_GRAPH_H

#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the graph_walk and graph_ppr table-valued functions on one connection. */
int sqlite3_graph_register(sqlite3 *db);

#ifdef SQLITE_CORE
/* Registers them on every connection opened afterwards (sqlite3_auto_extension). Call once, before opening. */
int sqlite3_graph_auto_register(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
