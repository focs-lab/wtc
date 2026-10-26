// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Load for the mutex census of SQLite, in one of two shapes.
//
// shared: N threads share one connection to an in-memory database opened in
// serialized mode (SQLITE_OPEN_FULLMUTEX), the configuration a
// multi-threaded program uses when it does not want to think about it;
// each thread inserts rows, one statement per transaction since the
// connection's transaction is shared, and reads them back.
//
// per-thread: each thread opens its own connection, with the library's
// default flags, to one database file in write-ahead-log mode, and does the
// same work; the file's locks are the operating system's, not mutexes.
//
//   cc -O2 -g -fno-omit-frame-pointer -DSQLITE_THREADSAFE=1 sqlite3.c sqlite_driver.c -lpthread -ldl -lm
//   ./a.out <threads> <rows per thread> [shared | per-thread <file>]
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"

static sqlite3 *shared_db;
static const char *file;   /* per-thread connections to this file, or NULL */
static int rows;

static void check_db(sqlite3 *db, int rc, const char *what) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        fprintf(stderr, "%s: %s\n", what, sqlite3_errmsg(db));
        exit(1);
    }
}
#define check(rc, what) check_db(db, rc, what)   /* db: the connection in scope */

static void *work(void *arg) {
    long id = (long)arg;
    sqlite3 *db = shared_db;
    if (file) {
        if (sqlite3_open(file, &db) != SQLITE_OK) { fprintf(stderr, "open %s\n", file); exit(1); }
        sqlite3_busy_timeout(db, 10000);
    }
    sqlite3_stmt *ins, *sel;
    check(sqlite3_prepare_v2(db, "INSERT INTO t(k, v) VALUES (?, ?)", -1, &ins, NULL), "prepare insert");
    check(sqlite3_prepare_v2(db, "SELECT v FROM t WHERE k = ?", -1, &sel, NULL), "prepare select");
    for (int i = 0; i < rows; i++) {
        sqlite3_bind_int64(ins, 1, id * 1000000L + i);
        sqlite3_bind_int(ins, 2, i);
        check(sqlite3_step(ins), "insert");
        sqlite3_reset(ins);
    }
    long sum = 0;
    for (int i = 0; i < rows; i++) {
        sqlite3_bind_int64(sel, 1, id * 1000000L + i);
        if (sqlite3_step(sel) == SQLITE_ROW) sum += sqlite3_column_int(sel, 0);
        sqlite3_reset(sel);
    }
    sqlite3_finalize(ins);
    sqlite3_finalize(sel);
    if (file) sqlite3_close(db);
    return (void *)sum;
}

int main(int argc, char **argv) {
    int threads = argc > 1 ? atoi(argv[1]) : 4;
    rows = argc > 2 ? atoi(argv[2]) : 5000;
    if (argc > 4 && strcmp(argv[3], "per-thread") == 0) {
        file = argv[4];
        remove(file);
    }
    sqlite3 *db = NULL;
    if (file) {
        check(sqlite3_open(file, &db), "open");
        check(sqlite3_exec(db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL), "wal");
    } else {
        check(sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                              NULL), "open");
        shared_db = db;
    }
    check(sqlite3_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER)", NULL, NULL, NULL), "create");
    pthread_t *th = calloc(threads, sizeof *th);
    for (long i = 0; i < threads; i++) pthread_create(&th[i], NULL, work, (void *)i);
    long total = 0;
    for (int i = 0; i < threads; i++) { void *r; pthread_join(th[i], &r); total += (long)r; }
    printf("sqlite: %d threads, %d rows each, %s, checksum %ld\n", threads, rows,
           file ? "a connection per thread" : "one shared connection", total);
    sqlite3_close(db);
    return 0;
}
