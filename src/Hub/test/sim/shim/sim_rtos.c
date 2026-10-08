// Minimal FreeRTOS queue/task emulation on pthreads for the host simulation.
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/queue.h"
#include "freertos/task.h"

struct sim_queue {
	pthread_mutex_t m;
	pthread_cond_t c;
	unsigned len, size, head, count;
	unsigned char *buf;
};

QueueHandle_t xQueueCreate(unsigned len, unsigned item_size)
{
	struct sim_queue *q = calloc(1, sizeof(*q));
	pthread_mutex_init(&q->m, NULL);
	pthread_condattr_t ca;
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&q->c, &ca);
	q->len = len;
	q->size = item_size;
	q->buf = calloc(len, item_size);
	return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
	(void)ticks;
	pthread_mutex_lock(&q->m);
	if (q->count == q->len) {
		pthread_mutex_unlock(&q->m);
		return pdFALSE;
	}
	memcpy(q->buf + ((q->head + q->count) % q->len) * q->size, item, q->size);
	q->count++;
	pthread_cond_signal(&q->c);
	pthread_mutex_unlock(&q->m);
	return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t ticks)
{
	struct timespec dl;
	clock_gettime(CLOCK_MONOTONIC, &dl);
	dl.tv_sec += ticks / 1000;
	dl.tv_nsec += (long)(ticks % 1000) * 1000000L;
	if (dl.tv_nsec >= 1000000000L) {
		dl.tv_sec++;
		dl.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&q->m);
	while (q->count == 0 && ticks != 0) {
		if (pthread_cond_timedwait(&q->c, &q->m, &dl) == ETIMEDOUT)
			break;
	}
	if (q->count == 0) {
		pthread_mutex_unlock(&q->m);
		return pdFALSE;
	}
	memcpy(item, q->buf + q->head * q->size, q->size);
	q->head = (q->head + 1) % q->len;
	q->count--;
	pthread_mutex_unlock(&q->m);
	return pdTRUE;
}

struct start {
	TaskFunction_t fn;
	void *arg;
};

static void *trampoline(void *p)
{
	struct start s = *(struct start *)p;
	free(p);
	s.fn(s.arg);
	return NULL;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
				   unsigned prio, TaskHandle_t *handle, int core)
{
	(void)name; (void)stack; (void)prio; (void)handle; (void)core;
	pthread_t t;
	struct start *s = malloc(sizeof(*s));
	s->fn = fn;
	s->arg = arg;
	pthread_create(&t, NULL, trampoline, s);
	pthread_detach(t);
	return pdTRUE;
}

void vTaskDelay(TickType_t ticks)
{
	struct timespec ts = {ticks / 1000, (long)(ticks % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}
