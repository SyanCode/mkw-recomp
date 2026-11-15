// What p2p.c and ov_p2p.c link against, replaced by pthreads and the console, for tests/c/t_p2p.c and t_ov_p2p.c.
#pragma once
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <time.h>

#include "platform/platform.h"

static pthread_mutex_t big = PTHREAD_MUTEX_INITIALIZER;   // one lock for every PlatMutex: p2p.c never nests two
void plat_mutex_lock(PlatMutex *m) { (void)m; pthread_mutex_lock(&big); }
void plat_mutex_unlock(PlatMutex *m) { (void)m; pthread_mutex_unlock(&big); }
uint64_t plat_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
int plat_random(void *out, size_t n) { return getrandom(out, n, 0) == (ssize_t)n; }
struct PlatThread { pthread_t t; };
PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    (void)name;
    PlatThread *t = malloc(sizeof *t);
    typedef void *(*Fn)(void *);
    if (pthread_create(&t->t, NULL, (Fn)(void (*)(void))fn, arg)) return free(t), NULL;
    return t;
}
void plat_thread_join(PlatThread *t) { pthread_join(t->t, NULL); free(t); }
void rt_log(const char *fmt, ...) {
    va_list a;
    va_start(a, fmt);
    printf("  [log] ");
    vprintf(fmt, a);
    printf("\n");
    va_end(a);
}

