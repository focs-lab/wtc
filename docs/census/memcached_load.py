#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
"""Load for the mutex census of memcached: N client threads, each on its own
connection, set then get keys in the text protocol; a stand-in for memtier
where it is not installed.

    memcached_load.py <host> <port> <threads> <requests per thread>
"""
import socket, sys, threading

host, port, threads, n = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])


def client(t):
    s = socket.create_connection((host, port))
    f = s.makefile("rwb", buffering=0)
    val = b"x" * 64
    for i in range(n):
        key = b"k%d_%d" % (t, i % 1000)
        if i % 10 == 0:
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
print("memcached load: %d threads x %d requests" % (threads, n))
