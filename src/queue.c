/*
 * V4L2 Codec decoding example application
 * Kamil Debski <k.debski@samsung.com>
 *
 * Queue handling
 *
 * Copyright 2012 Samsung Electronics Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "common.h"
#include "queue.h"

#include <pthread.h>
#include <stdlib.h>

#define DBG_TAG "  queue"

int queue_init(struct queue *q, int size)
{
	q->q = malloc(size * sizeof(int));
	if (!q->q) {
		err("Failed to init queue (malloc failed)");
		return -1;
	}

	q->size = size;
	q->head = 0;
	q->tail = 0;
	q->n = 0;

	pthread_mutex_init(&q->mutex, NULL);
	pthread_cond_init(&q->cond, NULL);

	return 0;
}

int queue_add(struct queue *q, int e)
{
	pthread_mutex_lock(&q->mutex);

	if (q->n >= q->size) {
		pthread_mutex_unlock(&q->mutex);
		return -1;
	}
	q->q[q->head] = e;
	q->head++;
	q->head %= q->size;
	q->n++;

	pthread_cond_signal(&q->cond);

	pthread_mutex_unlock(&q->mutex);

	return 0;
}

int queue_remove(struct queue *q, int nonblock)
{
	int x;

	pthread_mutex_lock(&q->mutex);

	if (q->n == 0) {
		if (nonblock) {
			x = -EINVAL;
			goto unlock;
		}
		dbg("queue_remove wait");
		pthread_cond_wait(&q->cond, &q->mutex);
	}

	x = q->q[q->tail];
	q->tail++;
	q->tail %= q->size;
	q->n--;

unlock:
	pthread_mutex_unlock(&q->mutex);

	return x;
}

int queue_empty(struct queue *q)
{
	int x;

	pthread_mutex_lock(&q->mutex);
	x = (q->n == 0);
	pthread_mutex_unlock(&q->mutex);

	return x;
}

int queue_num(struct queue *q)
{
	int x;

	pthread_mutex_lock(&q->mutex);
	x = q->n;
	pthread_mutex_unlock(&q->mutex);

	return x;
}

void queue_flush(struct queue *q)
{
	pthread_cond_signal(&q->cond);
}

void queue_free(struct queue *q)
{
	free(q->q);
	pthread_cond_destroy(&q->cond);
	pthread_mutex_destroy(&q->mutex);
}
