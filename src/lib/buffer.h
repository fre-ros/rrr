/*

Voltage Logger

Copyright (C) 2018 Atle Solbakken atle@goliathdns.no

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifndef VL_BUFFER_H
#define VL_BUFFER_H

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include "../global.h"

#define FIFO_SPIN_DELAY 50 // milliseconds

#define FIFO_OK 0
#define FIFO_GLOBAL_ERR -1
#define FIFO_CALLBACK_ERR 1

struct fifo_callback_args {
	void *source;
	void *private_data;
	unsigned int flags;
};

struct fifo_buffer_entry {
	char *data;
	unsigned long int size;
	uint64_t order;
	struct fifo_buffer_entry *next;
};

struct fifo_buffer {
	struct fifo_buffer_entry *gptr_first;
	struct fifo_buffer_entry *gptr_last;
	pthread_mutex_t mutex;
	pthread_mutex_t write_mutex;
	int readers;
	int writers;
	int writer_waiting;
	int invalid;
};

// TODO : These locking methods are unfair, fix if it matters
static inline void fifo_write_lock(struct fifo_buffer *buffer) {
	int ok = 0;
	while (ok != 2) {
		if (ok == 0) {
			VL_DEBUG_MSG_4("Buffer %p write lock wait for write mutex\n", buffer);
			pthread_mutex_lock(&buffer->write_mutex);
			VL_DEBUG_MSG_4("Buffer %p write lock wait for writer waiting %i\n", buffer, buffer->writer_waiting);
			if (buffer->writer_waiting == 0) {
				buffer->writer_waiting = 1;
				ok = 1;
			}
			VL_DEBUG_MSG_4("Buffer %p write lock unlock write mutex\n", buffer);
			pthread_mutex_unlock(&buffer->write_mutex);
		}

		if (ok == 1) {
			pthread_mutex_lock(&buffer->mutex);
			VL_DEBUG_MSG_4("Buffer %p write lock wait for %i readers %i writers\n", buffer, buffer->readers, buffer->writers);
			if (buffer->readers == 0 && buffer->writers == 0) {
				VL_DEBUG_MSG_4("Buffer %p write lock obtained\n", buffer);
				buffer->writers = 1;
				ok = 2;
			}
			else {
				usleep(FIFO_SPIN_DELAY*1000);
			}
			pthread_mutex_unlock(&buffer->mutex);
		}
		else {
			usleep(FIFO_SPIN_DELAY*1000);
		}
	}

	VL_DEBUG_MSG_4("Buffer %p write lock wait for write mutex end\n", buffer);
	pthread_mutex_lock(&buffer->write_mutex);
	buffer->writer_waiting = 0;
	VL_DEBUG_MSG_4("Buffer %p write lock unlock write mutex end\n", buffer);
	pthread_mutex_unlock(&buffer->write_mutex);
}

static inline void fifo_write_unlock(struct fifo_buffer *buffer) {
	VL_DEBUG_MSG_4("Buffer %p write unlock\n", buffer);
	buffer->writers = 0;
}

static inline void fifo_read_lock(struct fifo_buffer *buffer) {
	int ok = 0;
	while (!ok) {
		VL_DEBUG_MSG_4("Buffer %p read lock wait for mutex\n", buffer);
		pthread_mutex_lock(&buffer->mutex);
		if (buffer->writers == 0 && buffer->writer_waiting == 0) {
			VL_DEBUG_MSG_4("Buffer %p read lock pass 1\n", buffer);
			buffer->readers++;
			ok = 1;
		}
		else {
			usleep(FIFO_SPIN_DELAY*1000);
		}
		pthread_mutex_unlock(&buffer->mutex);
	}
}

static inline void fifo_read_unlock(struct fifo_buffer *buffer) {
	VL_DEBUG_MSG_4("Buffer %p read unlock wait for mutex\n", buffer);
	pthread_mutex_lock(&buffer->mutex);
	VL_DEBUG_MSG_4("Buffer %p read unlock\n", buffer);
	buffer->readers--;
	pthread_mutex_unlock(&buffer->mutex);
}

/*
 * With fifo_read_clear_forward, the callback function MUST
 * handle ALL entries as we cannot add elements back in this
 * case, the callback function may simply write them back
 * using one of the write functions as no locks are active
 * when the callback function is run.
 *
 * For fifo_search the lock is active when the callback function
 * is run, and the callback MUST NOT attempt to write to the same buffer
 * as this causes a deadlock.
 *
 * Callbacks of fifo_search may return these values to control when
 * to stop or when to delete entries (values can be ORed except for
 * the error value). Functions return 0 on success and 1 on error. If
 * the callback of fifo_search returns FIFO_SEARCH_ERR, the search
 * is stopped and fifo_search returns 1.
 *
 * To count elements, a counter may be placed in a custom struct pointed
 * to by the fifo_callback_data struct, and the callback has to do the
 * counting.
 */

#define FIFO_SEARCH_ERR		-1
#define FIFO_SEARCH_KEEP	0
#define FIFO_SEARCH_STOP	(1 << 1)
#define FIFO_SEARCH_GIVE	(1 << 2)
#define FIFO_SEARCH_FREE	(1 << 3)

int fifo_search (
	struct fifo_buffer *buffer,
	int (*callback)(struct fifo_callback_args *callback_data, char *data, unsigned long int size),
	struct fifo_callback_args *callback_data
);
int fifo_clear_order_lt (
		struct fifo_buffer *buffer,
		uint64_t order_min
);
int fifo_read_clear_forward (
		struct fifo_buffer *buffer,
		struct fifo_buffer_entry *last_element,
		int (*callback)(struct fifo_callback_args *callback_data, char *data, unsigned long int size),
		struct fifo_callback_args *callback_data
);

//void fifo_read(struct fifo_buffer *buffer, void (*callback)(char *data, unsigned long int size)); Not needed, dupes fifo_search
void fifo_buffer_write(struct fifo_buffer *buffer, char *data, unsigned long int size);
void fifo_buffer_write_ordered(struct fifo_buffer *buffer, uint64_t order, char *data, unsigned long int size);

void fifo_buffer_invalidate(struct fifo_buffer *buffer);
// void fifo_buffer_destroy(struct fifo_buffer *buffer); Not thread safe
void fifo_buffer_init(struct fifo_buffer *buffer);

#endif
