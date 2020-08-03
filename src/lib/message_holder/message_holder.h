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

#ifndef RRR_MESSAGE_HOLDER_H
#define RRR_MESSAGE_HOLDER_H

#include <sys/socket.h>
#include <stdint.h>
#include <pthread.h>

struct rrr_msg_msg_holder;

//extern pthread_mutex_t rrr_ip_buffer_master_lock;

void rrr_msg_msg_holder_lock (
		struct rrr_msg_msg_holder *entry
);
void rrr_msg_msg_holder_unlock (
		struct rrr_msg_msg_holder *entry
);
void rrr_msg_msg_holder_incref_while_locked (
		struct rrr_msg_msg_holder *entry
);
static inline void rrr_msg_msg_holder_incref (
		struct rrr_msg_msg_holder *entry
) {
	rrr_msg_msg_holder_lock(entry);
	rrr_msg_msg_holder_incref_while_locked (entry);
	rrr_msg_msg_holder_unlock(entry);
}
void rrr_msg_msg_holder_decref (
		struct rrr_msg_msg_holder *entry
);
void rrr_msg_msg_holder_decref_while_locked_and_unlock (
		struct rrr_msg_msg_holder *entry
);
void rrr_msg_msg_holder_decref_void (
		void *entry
);
int rrr_msg_msg_holder_new (
		struct rrr_msg_msg_holder **result,
		ssize_t data_length,
		const struct sockaddr *addr,
		socklen_t addr_len,
		int protocol,
		void *message
);
void rrr_msg_msg_holder_set_unlocked (
		struct rrr_msg_msg_holder *target,
		void *message,
		ssize_t message_data_length,
		const struct sockaddr *addr,
		socklen_t addr_len,
		int protocol
);

#endif /* RRR_MESSAGE_HOLDER_H */
