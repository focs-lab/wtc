#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Load for the mutex census of memcached: N client threads, each on its own
connection, set then get keys in the text protocol; a stand-in for memtier
where it is not installed. With a fifth argument K, the first client sends
`stats` every K requests, which makes memcached read every worker's counters
from the thread serving that client.

    memcached_load.py <host> <port> <threads> <requests per thread> [stats every]
"""
import socket, sys, threading

host, port, threads, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
stats_every = int(sys.argv[5]) if len(sys.argv) > 5 else 0


def client(t):
    s = socket.create_connection((host, port))
    f = s.makefile("rwb", buffering=0)
    val = b"x" * 64
    for i in range(n):
        key = b"k%d_%d" % (t, i % 1000)
        if t == 0 and stats_every and i % stats_every == 0:
            f.write(b"stats\r\n")
            while f.readline() != b"END\r\n":
                pass
        elif i % 10 == 0:
            f.write(b"set %s 0 0 %d\r\n%s\r\n" % (key, len(val), val))
            assert f.readline().startswith(b"STORED")
        else:
            f.write(b"get %s\r\n" % key)
            line = f.readline()
            if line.startswith(b"VALUE"):
                f.readline(); f.readline()
    s.close()


ts = [threading.Thread(target=client, args=(t,)) for t in range(threads)]
for t in ts: t.start()
for t in ts: t.join()
print("memcached load: %d threads x %d requests%s"
      % (threads, n, ", stats every %d" % stats_every if stats_every else ""))
