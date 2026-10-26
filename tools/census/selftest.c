/* A program whose census is known: mutex a is taken by the main thread
 * only, 1000 times; mutex b by the main thread and three others, 250 times
 * each. Then mutex c is initialised and taken 10 times by the main thread,
 * and initialised again at the same address, without being destroyed, and
 * taken 10 times by a second thread: the second initialisation starts an
 * entry of its own, so both are single-thread. The report must say three
 * single-thread mutexes of four and 1020 of 2020 acquisitions on them. */
#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t b = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t c;
static int counter_a, counter_b, counter_c;   /* one per mutex, so there is no race */

static void* worker(void* arg) {
    (void)arg;
    for (int i = 0; i < 250; ++i) {
        pthread_mutex_lock(&b);
        ++counter_b;
        pthread_mutex_unlock(&b);
    }
    return NULL;
}

static void* take_c(void* arg) {
    (void)arg;
    for (int i = 0; i < 10; ++i) {
        pthread_mutex_lock(&c);
        ++counter_c;
        pthread_mutex_unlock(&c);
    }
    return NULL;
}

int main(void) {
    pthread_t t[3];
    for (int i = 0; i < 3; ++i) pthread_create(&t[i], NULL, worker, NULL);
    worker(NULL);
    for (int i = 0; i < 1000; ++i) {
        pthread_mutex_lock(&a);
        ++counter_a;
        pthread_mutex_unlock(&a);
    }
    for (int i = 0; i < 3; ++i) pthread_join(t[i], NULL);
    pthread_mutex_init(&c, NULL);
    take_c(NULL);
    pthread_mutex_init(&c, NULL);   /* reused without pthread_mutex_destroy */
    pthread_t r;
    pthread_create(&r, NULL, take_c, NULL);
    pthread_join(r, NULL);
    printf("counters %d %d %d\n", counter_a, counter_b, counter_c);
    return counter_a == 1000 && counter_b == 1000 && counter_c == 20 ? 0 : 1;
}
