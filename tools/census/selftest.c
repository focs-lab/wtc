/* A program whose census is known: mutex a is taken by the main thread
 * only, 1000 times; mutex b by the main thread and three others, 250 times
 * each. The report must say one single-thread mutex of two and 50% of the
 * acquisitions on it. */
#include <pthread.h>
#include <stdio.h>

static pthread_mutex_t a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t b = PTHREAD_MUTEX_INITIALIZER;
static int counter_a, counter_b;   /* one per mutex, so there is no race */

static void* worker(void* arg) {
    (void)arg;
    for (int i = 0; i < 250; ++i) {
        pthread_mutex_lock(&b);
        ++counter_b;
        pthread_mutex_unlock(&b);
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
    printf("counters %d %d\n", counter_a, counter_b);
    return counter_a == 1000 && counter_b == 1000 ? 0 : 1;
}
