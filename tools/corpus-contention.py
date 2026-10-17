#!/usr/bin/env python3
"""Can the contended fields of an object be separated by reordering at all?

A finding says two fields share a line and two threads touch them. It does
not say the layout can be fixed. Read the findings for one object as a
graph -- a field is a vertex, a reported pair an edge -- and the question
becomes whether the graph can be split across cache lines so that no edge
stays inside one.

A sparse graph can: group the fields by which thread owns them and the
edges fall between the groups, which costs nothing. A dense one cannot:
if every field is contended with nearly every other, every arrangement
leaves most of the edges intact, and the only remedies left cost memory
(padding each field to its own line) or change the program (a copy per
thread, summed on read).

So this decides which transformation an object needs, and it is worth
knowing before writing either of them.
"""
import csv, sys
from collections import defaultdict


def main():
    rows = [r for r in csv.DictReader(open(sys.argv[1], newline=''))
            if r['verdict'] == 'definite']
    adj = defaultdict(lambda: defaultdict(set))
    for r in rows:
        adj[r['object']][r['field_a']].add(r['field_b'])
        adj[r['object']][r['field_b']].add(r['field_a'])

    print(f"{'object':<22}{'fields':>7}{'edges':>7}{'density':>9}{'max deg':>9}")
    print('-' * 54)
    for o, d in sorted(adj.items(), key=lambda kv: -len(kv[1])):
        n = len(d)
        edges = sum(len(v) for v in d.values()) // 2
        full = n * (n - 1) // 2
        print(f"{o:<22}{n:>7}{edges:>7}{edges / full if full else 0:>9.2f}"
              f"{max(len(v) for v in d.values()):>9}")
    print('-' * 54)
    print("\ndensity is edges over all possible pairs: 1.00 means every field is\n"
          "contended with every other, and no ordering separates them.")


if __name__ == '__main__':
    main()
