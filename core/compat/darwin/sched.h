/* <sched.h> plus the Linux CPU-affinity API. Workers in Schwung modules pin
 * themselves off the SPI core (core 3); on a desktop the kernel scheduler is
 * the right owner of that decision, so affinity calls succeed as no-ops. */
#pragma once
#include_next <sched.h>
#include <sys/types.h>
#include <string.h>
#ifndef CPU_SETSIZE
#define CPU_SETSIZE 64
typedef struct { unsigned long long bits; } cpu_set_t;
#define CPU_ZERO(s)       memset((s), 0, sizeof(cpu_set_t))
#define CPU_SET(c, s)     ((s)->bits |= (1ULL << (c)))
#define CPU_CLR(c, s)     ((s)->bits &= ~(1ULL << (c)))
#define CPU_ISSET(c, s)   (((s)->bits >> (c)) & 1ULL)
#define CPU_COUNT(s)      __builtin_popcountll((s)->bits)
static inline int sched_setaffinity(pid_t p, size_t n, const cpu_set_t *s) { (void)p; (void)n; (void)s; return 0; }
static inline int sched_getaffinity(pid_t p, size_t n, cpu_set_t *s) { (void)p; (void)n; if (s) s->bits = ~0ULL; return 0; }
static inline int sched_getcpu(void) { return 0; }
#endif

/* sched_setscheduler/sched_getscheduler do not exist on macOS (threads are
 * scheduled through pthread_setschedparam / QoS). Schwung workers use them to
 * drop back to SCHED_OTHER off the SPI core; on a desktop that is already the
 * default, so succeed without doing anything. */
#ifndef SCHED_OTHER
#define SCHED_OTHER 1
#endif
#ifndef SCHED_FIFO
#define SCHED_FIFO 4
#endif
#ifndef SCHED_RR
#define SCHED_RR 2
#endif
#ifndef SW_HAVE_SCHED_SETSCHEDULER
#define SW_HAVE_SCHED_SETSCHEDULER
static inline int sw_sched_setscheduler(pid_t p, int pol, const struct sched_param *sp) { (void)p; (void)pol; (void)sp; return 0; }
static inline int sw_sched_getscheduler(pid_t p) { (void)p; return SCHED_OTHER; }
#define sched_setscheduler sw_sched_setscheduler
#define sched_getscheduler sw_sched_getscheduler
#endif

