// Minimal single-threaded stand-ins for the Zephyr APIs used by wired.c.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef long atomic_t;
static inline long atomic_get(atomic_t *a) { return *a; }
static inline long atomic_set(atomic_t *a, long v) { long o = *a; *a = v; return o; }
static inline long atomic_clear(atomic_t *a) { return atomic_set(a, 0); }
static inline bool atomic_cas(atomic_t *a, long o, long n) { if (*a != o) return false; *a = n; return true; }
#define BIT(n) (1u << (n))
#define printk printf
#define SYS_FOREVER_US (-1)
typedef struct { long ms; } k_timeout_t;
#define K_MSEC(x) ((k_timeout_t){(x)})
#define K_FOREVER ((k_timeout_t){-1})
#define K_NO_WAIT ((k_timeout_t){0})
extern int64_t sim_now_ms;
static inline int64_t k_uptime_get(void) { return sim_now_ms; }
static inline unsigned int irq_lock(void) { return 0; }
static inline void irq_unlock(unsigned int k) { (void)k; }
// message queue
struct k_msgq { size_t size, max, used, head; uint8_t *buf; };
#define K_MSGQ_DEFINE(n, sz, cnt, al) static uint8_t n##_buf[(sz) * (cnt)]; struct k_msgq n = {(sz), (cnt), 0, 0, n##_buf}
static inline int k_msgq_put(struct k_msgq *q, const void *d, k_timeout_t t) { (void)t; if (q->used == q->max) return -1; memcpy(q->buf + ((q->head + q->used) % q->max) * q->size, d, q->size); q->used++; return 0; }
static inline int k_msgq_get(struct k_msgq *q, void *d, k_timeout_t t) { (void)t; if (!q->used) return -1; memcpy(d, q->buf + q->head * q->size, q->size); q->head = (q->head + 1) % q->max; q->used--; return 0; }
// work items: record calls only
struct k_work { void (*fn)(struct k_work *); int submitted; };
struct k_work_delayable { struct k_work work; long delay_ms; int scheduled; };
#define K_WORK_DEFINE(n, f) struct k_work n = {f, 0}
#define K_WORK_DELAYABLE_DEFINE(n, f) struct k_work_delayable n = {{f, 0}, 0, 0}
static inline int k_work_submit(struct k_work *w) { w->submitted++; return 0; }
static inline int k_work_reschedule(struct k_work_delayable *w, k_timeout_t t) { w->delay_ms = t.ms; w->scheduled++; return 0; }
static inline int k_work_cancel_delayable(struct k_work_delayable *w) { w->scheduled = 0; return 0; }
struct k_sem { int count; };
#define K_SEM_DEFINE(n, i, m) struct k_sem n = {(i)}
static inline void k_sem_give(struct k_sem *s) { s->count = 1; }
static inline int k_sem_take(struct k_sem *s, k_timeout_t t) { (void)t; if (!s->count) return -1; s->count = 0; return 0; }
#define K_THREAD_DEFINE(n, st, fn, a, b, c, p, o, d) void *const n##_entry = (void *)(fn)
// devicetree / device
struct device { const char *name; };
#define DT_CHOSEN(x) 0
#define DT_PATH(x) 0
#define DT_NODE_HAS_STATUS(n, s) 1
#define BUILD_ASSERT(c, m)
extern struct device sim_uart;
#define DEVICE_DT_GET(n) (&sim_uart)
extern int sim_device_inits;
static inline bool device_is_ready(const struct device *d) { (void)d; return true; }
static inline int device_init(const struct device *d) { (void)d; sim_device_inits++; return 0; }
// logging
#define LOG_MODULE_REGISTER(n, l)
#define LOG_INF(...) (printf("  [inf] " __VA_ARGS__), printf("\n"))
#define LOG_WRN(...) (printf("  [wrn] " __VA_ARGS__), printf("\n"))
#define LOG_ERR(...) (printf("  [err] " __VA_ARGS__), printf("\n"))
