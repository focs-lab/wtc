/* Reference counts with a known census, through wtc_rc_note as wtc-cc
 * --census calls it: block a has 1000 operations, all on the main thread;
 * block b has 250 on each of four threads. Then a is destroyed and a new
 * block at the same address gets 10 operations from a second thread: it
 * is counted apart, so a stays single-thread. Last, 200000 short-lived
 * blocks get one operation each and are destroyed, the way a program that
 * allocates a shared_ptr per message does; their slots are reused, so this
 * takes milliseconds rather than the minutes a table that kept them would
 * take to probe past them. The report must say 200002 single-thread blocks
 * of 200003, 201010 of 202010 operations, and list a, destroyed, among the
 * busiest. Block c is made by the main thread and released by a second:
 * two threads. Block d is made and copied three times by the main thread,
 * then made again at the same address, without being destroyed, by a
 * second thread, which copies it once: two single-thread blocks. The
 * report must say 200004 single-thread blocks of 200006 and 201016 of
 * 202018 operations; and the same with a table of 4096 entries, three
 * times smaller than the number of blocks made, which only fits if
 * retired slots are reused. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

void wtc_rc_note(void* ctrl, int op);

static long a[2], b[2], c[2], d[2];

static void* worker(void* arg) {
    (void)arg;
    for (int i = 0; i < 125; ++i) {
        wtc_rc_note(b, 0);  /* copy */
        wtc_rc_note(b, 1);  /* release */
    }
    return NULL;
}

static void* reuser(void* arg) {
    (void)arg;
    for (int i = 0; i < 5; ++i) {
        wtc_rc_note(a, 0);
        wtc_rc_note(a, 1);
    }
    return NULL;
}

static void* release_c(void* arg) {
    (void)arg;
    wtc_rc_note(c, 1);
    return NULL;
}

static void* remake_d(void* arg) {
    (void)arg;
    wtc_rc_note(d, 6);  /* made */
    wtc_rc_note(d, 0);
    return NULL;
}

int main(void) {
    pthread_t t[3], r;
    for (int i = 0; i < 3; ++i) pthread_create(&t[i], NULL, worker, NULL);
    worker(NULL);
    for (int i = 0; i < 500; ++i) {
        wtc_rc_note(a, 0);
        wtc_rc_note(a, 1);
    }
    for (int i = 0; i < 3; ++i) pthread_join(t[i], NULL);
    wtc_rc_note(a, 5);  /* destroyed */
    pthread_create(&r, NULL, reuser, NULL);
    pthread_join(r, NULL);
    for (uintptr_t i = 0; i < 200000; ++i) {
        void* p = (void*)(0x10000000u + 16u * i);   /* distinct addresses, never dereferenced */
        wtc_rc_note(p, 1);
        wtc_rc_note(p, 5);
    }
    wtc_rc_note(c, 6);
    pthread_t rc, rd;
    pthread_create(&rc, NULL, release_c, NULL);
    pthread_join(rc, NULL);
    wtc_rc_note(d, 6);
    for (int i = 0; i < 3; ++i) wtc_rc_note(d, 0);
    pthread_create(&rd, NULL, remake_d, NULL);
    pthread_join(rd, NULL);
    puts("done");
    return 0;
}
