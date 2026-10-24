"""Smoke test for the loadable build: python3 tests/load_test.py build/libsqlitegraph.<dylib|so>"""
import sqlite3
import sys

con = sqlite3.connect(":memory:")
con.enable_load_extension(True)
con.load_extension(sys.argv[1])
con.executescript("CREATE TABLE e(s, t); INSERT INTO e VALUES ('a','b'),('b','c');")
walk = con.execute("SELECT node, depth FROM graph_walk('e','s','t','a',2,'out')").fetchall()
assert walk == [("a", 0), ("b", 1), ("c", 2)], walk
ppr = con.execute("SELECT node FROM graph_ppr('e','s','t','a','out')").fetchall()
assert ppr[0] == ("a",), ppr
print("loadable ok")
