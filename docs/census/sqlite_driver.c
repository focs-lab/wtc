// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Load for the mutex census of SQLite: N threads share one connection
// opened in serialized mode (SQLITE_OPEN_FULLMUTEX), the configuration a
// multi-threaded program uses when it does not want to think about it;
// each thread inserts rows, one statement per transaction since the
// connection's transaction is shared, and reads them back.
//
//   cc -O2 -g -fno-omit-frame-pointer -DSQLITE_THREADSAFE=1 sqlite3.c sqlite_driver.c -lpthread -ldl -lm
//   ./a.out <threads> <rows per thread>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "sqlite3.h"

static sqlite3 *db;
static int rows;

static void check(int rc, const char *what) {
    if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW) {
        fprintf(stderr, "%s: %s\n", what, sqlite3_errmsg(db));
        exit(1);
    }
}

static void *work(void *arg) {
    long id = (long)arg;
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
    return (void *)sum;
}

int main(int argc, char **argv) {
    int threads = argc > 1 ? atoi(argv[1]) : 4;
    rows = argc > 2 ? atoi(argv[2]) : 5000;
    check(sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, NULL), "open");
    check(sqlite3_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER)", NULL, NULL, NULL), "create");
    pthread_t *th = calloc(threads, sizeof *th);
    for (long i = 0; i < threads; i++) pthread_create(&th[i], NULL, work, (void *)i);
    long total = 0;
    for (int i = 0; i < threads; i++) { void *r; pthread_join(th[i], &r); total += (long)r; }
    printf("sqlite: %d threads, %d rows each, checksum %ld\n", threads, rows, total);
    sqlite3_close(db);
    return 0;
}
