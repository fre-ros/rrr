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


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <limits.h>
#include <inttypes.h>

#include "../modules.h"
#include "../lib/messages.h"
#include "../lib/threads.h"
#include "../lib/buffer.h"
#include "../lib/messages.h"

struct averager_data {
	struct fifo_buffer input_buffer;
	struct fifo_buffer output_buffer;

	// Set this to 1 when others may read from our buffer
	int average_is_ready;
	pthread_mutex_t average_ready_lock;
};

// Should not be smaller than module max
#define VL_AVERAGER_MAX_SENDERS VL_MODULE_MAX_SENDERS

// In seconds, keep x seconds of readings in the buffer
#define VL_AVERAGER_TIMESPAN 15

// Create an average/max/min-reading every x seconds
#define VL_AVERAGER_INTERVAL 10

// Poll of our output buffer from other modules
int averager_poll_delete (
	struct module_thread_data *thread_data,
	int (*callback)(struct fifo_callback_args *caller_data, char *data, unsigned long int size),
	struct fifo_callback_args *caller_data
) {
	struct averager_data *data = thread_data->private_data;

	pthread_mutex_lock(&data->average_ready_lock);
	if (data->average_is_ready == 1) {
		data->average_is_ready = 0;
		pthread_mutex_unlock(&data->average_ready_lock);
		return fifo_read_clear_forward(&data->output_buffer, NULL, callback, caller_data);
	}
	pthread_mutex_unlock(&data->average_ready_lock);

	return 0;
}

// Poll of our output buffer from other modules
int averager_poll (
	struct module_thread_data *thread_data,
	int (*callback)(struct fifo_callback_args *caller_data, char *data, unsigned long int size),
	struct fifo_callback_args *caller_data
) {
	struct averager_data *data = thread_data->private_data;

	return fifo_search(&data->output_buffer, callback, caller_data);
}

// Messages when from polling sender comes in here
int poll_callback(struct fifo_callback_args *poll_data, char *data, unsigned long int size) {
	struct module_thread_data *thread_data = poll_data->source;
	struct vl_message *message = (struct vl_message *) data;

	struct averager_data *averager_data = poll_data->private_data;

	// TODO : If we get an info message, the average measurements may get lost due to them having lower timestamps

	// We route info messages directly to output and store point measurements in input buffer
	if (MSG_IS_MSG_POINT(message)) {
		fifo_buffer_write_ordered(&averager_data->input_buffer, message->timestamp_from, data, size);

		printf ("Averager: %s size %lu measurement %" PRIu64 "\n", message->data, size, message->data_numeric);
	}
	else if (MSG_IS_MSG_INFO(message)) {
		fifo_buffer_write_ordered(&averager_data->output_buffer, message->timestamp_from, data, size);

		printf ("Averager: size %lu information '%s'\n", size, message->data);
	}
	else {
		fprintf (stderr, "Averager: Unknown message type from sender. Discarding.\n");
		free(message);
	}

	return 0;
}

void averager_maintain_buffer(struct averager_data *data) {
	uint64_t timespan_useconds = VL_AVERAGER_TIMESPAN * 1000000;
	uint64_t time_now = time_get_64();

	fifo_clear_order_lt(&data->input_buffer, time_now - timespan_useconds);
}

struct averager_calculation {
	struct averager_data *data;
	unsigned long int max;
	unsigned long int min;
	unsigned long int sum;
	unsigned long int entries;

	uint64_t timestamp_from;
	uint64_t timestamp_to;
	uint64_t timestamp_max;
	uint64_t timestamp_min;
};

int averager_callback(struct fifo_callback_args *poll_data, char *data, unsigned long int size) {
	struct averager_calculation *calculation = poll_data->private_data;
	struct vl_message *message = (struct vl_message *) data;

	if (!MSG_IS_MSG_POINT(message)) {
		printf ("Averager: Ignoring a message which is not point measurement\n");
		return FIFO_SEARCH_KEEP;
	}

	calculation->entries++;
	calculation->sum += message->data_numeric;
	if (message->data_numeric >= calculation->max) {
		calculation->max = message->data_numeric;
		calculation->timestamp_max = message->timestamp_from;
	}
	if (message->data_numeric < calculation->min) {
		calculation->min = message->data_numeric;
		calculation->timestamp_min = message->timestamp_from;
	}
	if (message->timestamp_from < calculation->timestamp_from) {
		calculation->timestamp_from = message->timestamp_from;
	}
	if (message->timestamp_to > calculation->timestamp_to) {
		calculation->timestamp_to = message->timestamp_to;
	}

	return FIFO_SEARCH_KEEP;
}

void averager_spawn_message (
	struct averager_data *data,
	int class,
	uint64_t time_from,
	uint64_t time_to,
	uint64_t measurement
) {
	struct vl_message *message = malloc(sizeof(*message));

	char buf[64];
	sprintf(buf, "%" PRIu64, measurement);

	if (init_message (
			MSG_TYPE_MSG,
			class,
			time_from,
			time_to,
			measurement,
			buf,
			strlen(buf),
			message
	) != 0) {
		free(message);
		fprintf (stderr, "Bug: Could not initialize message\n");
		exit (EXIT_FAILURE);
	}

	fifo_buffer_write_ordered(&data->output_buffer, time_to, (char*) message, sizeof(*message));
}

void averager_calculate_average(struct averager_data *data) {
	struct averager_calculation calculation = {data, 0, ULONG_MAX, 0, 0, UINT64_MAX, 0, 0, 0};
	struct fifo_callback_args poll_data = {NULL, &calculation};
	fifo_search(&data->input_buffer, averager_callback, &poll_data);

	if (calculation.entries == 0) {
		printf ("Averager: No entries, not averaging\n");

		// There might be some info messages to pick up
		pthread_mutex_lock(&data->average_ready_lock);
		data->average_is_ready = 1;
		pthread_mutex_unlock(&data->average_ready_lock);

		return;
	}

	unsigned long int average = calculation.sum/calculation.entries;
	printf ("Average: %lu, Max: %lu, Min: %lu, Entries: %lu\n", average, calculation.max, calculation.min, calculation.entries);


	pthread_mutex_lock(&data->average_ready_lock);

	// Use the maximum timestamp for "to" for all three to make sure they can be written on block device
	// without newer timestamps getting written before older ones.
	averager_spawn_message(data, MSG_CLASS_AVG, calculation.timestamp_from, calculation.timestamp_to, average);
	averager_spawn_message(data, MSG_CLASS_MAX, calculation.timestamp_max, calculation.timestamp_to+1, calculation.max);
	averager_spawn_message(data, MSG_CLASS_MIN, calculation.timestamp_min, calculation.timestamp_to+2, calculation.min);

	data->average_is_ready = 1;

	pthread_mutex_unlock(&data->average_ready_lock);
}

struct averager_data *data_init(struct module_thread_data *module_thread_data) {
	// Use special memory region provided in module_thread_data which we don't have to free
	struct averager_data *data = (struct averager_data *) module_thread_data->private_memory;
	if (sizeof(*data) > VL_MODULE_PRIVATE_MEMORY_SIZE) {
		fprintf (stderr, "averager: Module thread private memory area too small\n");
		exit(EXIT_FAILURE);
	}
	memset(data, '\0', sizeof(*data));
	fifo_buffer_init(&data->input_buffer);
	fifo_buffer_init(&data->output_buffer);
	pthread_mutex_init(&data->average_ready_lock, NULL);
	return data;
}

void data_cleanup(void *arg) {
	// Make sure all readers have left and invalidate buffer
	struct averager_data *data = (struct averager_data *) arg;
	fifo_buffer_invalidate(&data->input_buffer);
	fifo_buffer_invalidate(&data->output_buffer);
	// Don't destroy mutex, threads might still try to use it
	//fifo_buffer_destroy(&data->buffer);
}


static void *thread_entry_averager(struct vl_thread_start_data *start_data) {
	struct module_thread_data *thread_data = start_data->private_arg;
	thread_data->thread = start_data->thread;
	unsigned long int senders_count = thread_data->senders_count;
	struct averager_data *data = data_init(thread_data);
	thread_data->private_data = data;

	printf ("Averager  thread data is %p\n", thread_data);

	pthread_cleanup_push(data_cleanup, data);
	pthread_cleanup_push(thread_set_stopping, start_data->thread);

	thread_set_state(start_data->thread, VL_THREAD_STATE_INITIALIZED);
	thread_signal_wait(thread_data->thread, VL_THREAD_SIGNAL_START);
	thread_set_state(start_data->thread, VL_THREAD_STATE_RUNNING);

	if (senders_count > VL_AVERAGER_MAX_SENDERS) {
		fprintf (stderr, "Too many senders for averager module, max is %i\n", VL_AVERAGER_MAX_SENDERS);
		goto out_message;
	}

	int (*poll[VL_AVERAGER_MAX_SENDERS])(
			struct module_thread_data *data,
			int (*callback)(
					struct fifo_callback_args *caller_data,
					char *data,
					unsigned long int size
			),
			struct fifo_callback_args *caller_data
	);

	for (int i = 0; i < senders_count; i++) {
		printf ("Averager: found sender %p\n", thread_data->senders[i]);
		poll[i] = thread_data->senders[i]->module->operations.poll_delete;

		if (poll[i] == NULL) {
			fprintf (stderr, "Averager cannot use this sender, lacking poll delete function.\n");
			goto out_message;
		}
	}

	printf ("Averager started thread %p\n", thread_data);
	if (senders_count == 0) {
		fprintf (stderr, "Error: Sender was not set for averager processor module\n");
		goto out_message;
	}

	uint64_t previous_average_time = time_get_64();
	uint64_t average_interval_useconds = VL_AVERAGER_INTERVAL * 1000000;

	while (thread_check_encourage_stop(thread_data->thread) != 1) {
		update_watchdog_time(thread_data->thread);

		averager_maintain_buffer(data);

		int err = 0;

		for (int i = 0; i < senders_count; i++) {
			struct fifo_callback_args poll_data = {thread_data->senders[i], data};
			int res = poll[i](thread_data->senders[i], poll_callback, &poll_data);
			if (!(res >= 0)) {
				fprintf (stderr, "Averager module received error from poll function\n");
				err = 1;
				break;
			}
		}

		if (err != 0) {
			break;
		}

		uint64_t current_time = time_get_64();
		if (previous_average_time + average_interval_useconds < current_time) {
			averager_calculate_average(data);
			previous_average_time = current_time;
		}

		usleep (1000000); // 1000 ms
	}

	out_message:

	printf ("Thread averager %p exiting\n", thread_data->thread);

	out:

	pthread_cleanup_pop(1);
	pthread_cleanup_pop(1);
	pthread_exit(0);
}

static struct module_operations module_operations = {
		thread_entry_averager,
		averager_poll,
		NULL,
		averager_poll_delete
};

static const char *module_name = "averager";

__attribute__((constructor)) void load() {
}

void init(struct module_dynamic_data *data) {
	data->private_data = NULL;
	data->name = module_name;
	data->type = VL_MODULE_TYPE_PROCESSOR;
	data->operations = module_operations;
	data->dl_ptr = NULL;
}

void unload(struct module_dynamic_data *data) {
	printf ("Destroy averager module\n");
}

