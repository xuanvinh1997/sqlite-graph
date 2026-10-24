CC      ?= cc
# Loadable builds need a real sqlite3ext.h; Apple's SDK header compiles the extension API out.
SQLITE_INCLUDE ?=
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-missing-field-initializers
UNAME   := $(shell uname -s)
LIBEXT  := $(if $(filter Darwin,$(UNAME)),dylib,so)
SHARED  := $(if $(filter Darwin,$(UNAME)),-dynamiclib -undefined dynamic_lookup,-shared)

test: build/test_graph
	./build/test_graph

build/test_graph: tests/test_graph.c src/sqlite_graph.c include/sqlite_graph.h
	@mkdir -p build
	$(CC) $(CFLAGS) -DSQLITE_CORE -Iinclude tests/test_graph.c src/sqlite_graph.c -lsqlite3 -lm -o $@

# Same tests against an SQLite amalgamation you ship yourself: make test-amalgamation SQLITE_DIR=/path/with/sqlite3.c
test-amalgamation:
	@test -n "$(SQLITE_DIR)" || (echo "set SQLITE_DIR to the directory holding sqlite3.c" && exit 2)
	@mkdir -p build
	$(CC) $(CFLAGS) -DSQLITE_CORE -DSQLITE_GRAPH_OWN_SQLITE $(SQLITE_DEFINES) -DSQLITE_ENABLE_JSON1 -Iinclude -I$(SQLITE_DIR) tests/test_graph.c src/sqlite_graph.c $(SQLITE_DIR)/sqlite3.c -lm -lpthread -ldl -o build/test_graph_amalgamation
	./build/test_graph_amalgamation

# Loadable extension (desktop SQLite built with load_extension enabled).
loadable: build/libsqlitegraph.$(LIBEXT)

build/libsqlitegraph.$(LIBEXT): src/sqlite_graph.c include/sqlite_graph.h
	@mkdir -p build
	$(CC) $(CFLAGS) -fPIC $(SHARED) $(if $(SQLITE_INCLUDE),-I$(SQLITE_INCLUDE)) -Iinclude src/sqlite_graph.c -lm -o $@

test-loadable: loadable
	python3 tests/load_test.py build/libsqlitegraph.$(LIBEXT)

clean:
	rm -rf build

.PHONY: test test-amalgamation loadable test-loadable clean
