/* Unnamed POSIX semaphores (sem_init) are not implemented on macOS: sem_init
 * returns ENOSYS. Several modules and the chain host's bus worker depend on
 * them, so map the unnamed API onto libdispatch semaphores. Named semaphores
 * (sem_open) are left to the system. */
#pragma once
#include_next <semaphore.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <time.h>
typedef struct { dispatch_semaphore_t s; } sw_sem_t;
static inline int sw_sem_init(sw_sem_t *x, int pshared, unsigned v) {
    (void)pshared; x->s = dispatch_semaphore_create((long)v); return x->s ? 0 : -1; }
static inline int sw_sem_destroy(sw_sem_t *x) { if (x->s) { dispatch_release(x->s); x->s = NULL; } return 0; }
static inline int sw_sem_post(sw_sem_t *x) { dispatch_semaphore_signal(x->s); return 0; }
static inline int sw_sem_wait(sw_sem_t *x) { dispatch_semaphore_wait(x->s, DISPATCH_TIME_FOREVER); return 0; }
static inline int sw_sem_trywait(sw_sem_t *x) {
    if (dispatch_semaphore_wait(x->s, DISPATCH_TIME_NOW) == 0) return 0; errno = EAGAIN; return -1; }
static inline int sw_sem_timedwait(sw_sem_t *x, const struct timespec *abs) {
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    long long ns = (long long)(abs->tv_sec - now.tv_sec) * 1000000000LL + (abs->tv_nsec - now.tv_nsec);
    if (ns < 0) ns = 0;
    if (dispatch_semaphore_wait(x->s, dispatch_time(DISPATCH_TIME_NOW, ns)) == 0) return 0;
    errno = ETIMEDOUT; return -1; }
#define sem_t            sw_sem_t
#define sem_init         sw_sem_init
#define sem_destroy      sw_sem_destroy
#define sem_post         sw_sem_post
#define sem_wait         sw_sem_wait
#define sem_trywait      sw_sem_trywait
#define sem_timedwait    sw_sem_timedwait
