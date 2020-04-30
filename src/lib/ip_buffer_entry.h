/*

Read Route Record

Copyright (C) 2018-2020 Atle Solbakken atle@goliathdns.no

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

#ifndef RRR_IP_BUFFER_ENTRY_H
#define RRR_IP_BUFFER_ENTRY_H

#include <sys/socket.h>
#include <stdint.h>
#include <pthread.h>

#include "rrr_socket.h"
#include "linked_list.h"

struct rrr_ip_buffer_entry {
	RRR_LL_NODE(struct rrr_ip_buffer_entry);
	pthread_mutex_t lock;
	ssize_t data_length;
	struct rrr_sockaddr addr;
	socklen_t addr_len;
	int protocol;
	uint64_t send_time;
	void *message;
};

struct rrr_ip_buffer_entry_collection {
	RRR_LL_HEAD(struct rrr_ip_buffer_entry);
};

extern pthread_mutex_t rrr_ip_buffer_master_lock;

static inline void rrr_ip_buffer_entry_lock (struct rrr_ip_buffer_entry *entry) {
	pthread_mutex_lock(&rrr_ip_buffer_master_lock);
	pthread_mutex_lock(&entry->lock);
	pthread_mutex_unlock(&rrr_ip_buffer_master_lock);
}

static inline void rrr_ip_buffer_entry_unlock (struct rrr_ip_buffer_entry *entry) {
	pthread_mutex_lock(&rrr_ip_buffer_master_lock);
	pthread_mutex_unlock(&entry->lock);
	pthread_mutex_unlock(&rrr_ip_buffer_master_lock);
}

void rrr_ip_buffer_entry_destroy (
		struct rrr_ip_buffer_entry *entry
);
void rrr_ip_buffer_entry_destroy_while_locked (
		struct rrr_ip_buffer_entry *entry
);
void rrr_ip_buffer_entry_destroy_void (
		void *entry
);
void rrr_ip_buffer_entry_collection_clear (
		struct rrr_ip_buffer_entry_collection *collection
);
void rrr_ip_buffer_entry_collection_clear_void (
		void *arg
);
int rrr_ip_buffer_entry_new (
		struct rrr_ip_buffer_entry **result,
		ssize_t data_length,
		const struct sockaddr *addr,
		socklen_t addr_len,
		int protocol,
		void *message
);
int rrr_ip_buffer_entry_new_with_empty_message (
		struct rrr_ip_buffer_entry **result,
		ssize_t message_data_length,
		const struct sockaddr *addr,
		socklen_t addr_len,
		int protocol
);
int rrr_ip_buffer_entry_clone (
		struct rrr_ip_buffer_entry **result,
		const struct rrr_ip_buffer_entry *source
);
void rrr_ip_buffer_entry_set_unlocked (
		struct rrr_ip_buffer_entry *target,
		void *message,
		ssize_t message_data_length,
		const struct sockaddr *addr,
		socklen_t addr_len,
		int protocol
);

#endif /* RRR_IP_BUFFER_ENTRY_H */
