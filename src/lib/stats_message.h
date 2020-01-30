/*

Read Route Record

Copyright (C) 2020 Atle Solbakken atle@goliathdns.no

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

#ifndef RRR_STATS_MESSAGE_H
#define RRR_STATS_MESSAGE_H

#include <stdint.h>
#include <stdio.h>

#include "linked_list.h"
#include "rrr_socket_msg.h"

#define RRR_STATS_MESSAGE_TYPE_TEXT			1
#define RRR_STATS_MESSAGE_TYPE_BASE10_TEXT	2

#define RRR_STATS_MESSAGE_PATH_INSTANCE_NAME	"name"

#define RRR_STATS_MESSAGE_FLAGS_STICKY (1<<0)

#define RRR_STATS_MESSAGE_FLAGS_ALL (RRR_STATS_MESSAGE_FLAGS_STICKY)

#define RRR_STATS_MESSAGE_FLAGS_IS_STICKY(message) ((message->flags & RRR_STATS_MESSAGE_FLAGS_STICKY) != 0)

#define RRR_STATS_MESSAGE_PATH_MAX_LENGTH 512
#define RRR_STATS_MESSAGE_DATA_MAX_SIZE 512

struct rrr_socket_read_session;

struct rrr_stats_message {
	RRR_LL_NODE(struct rrr_stats_message);
	uint8_t type;
	uint32_t flags;
	uint32_t data_size;
	uint64_t timestamp;
	char path[RRR_STATS_MESSAGE_PATH_MAX_LENGTH + 1];
	char data[RRR_STATS_MESSAGE_DATA_MAX_SIZE];
};

// msg_value of rrr_socket_msg-struct is used for timestamp
struct rrr_stats_message_packed {
	RRR_SOCKET_MSG_HEAD;
	uint8_t type;
	uint32_t flags;

	// Data begins after path_size and it's length is calculated
	// from msg_size and path_size
	uint16_t path_size;
	char path_and_data[RRR_STATS_MESSAGE_PATH_MAX_LENGTH + 1 + RRR_STATS_MESSAGE_DATA_MAX_SIZE];
} __attribute((packed));

struct rrr_stats_message_collection {
	RRR_LL_HEAD(struct rrr_stats_message);
};

struct rrr_stats_message_unpack_callback_data {
	int (*callback)(const struct rrr_stats_message *message, void *private_arg);
	void *private_arg;
};

int rrr_stats_message_unpack_callback (
		struct rrr_socket_read_session *read_session,
		void *private_arg
);

void rrr_stats_message_pack_and_flip (
		struct rrr_stats_message_packed *target,
		size_t *total_size,
		const struct rrr_stats_message *source
);

int rrr_stats_message_init (
		struct rrr_stats_message *message,
		uint8_t type,
		uint32_t flags,
		const char *path_postfix,
		const void *data,
		uint32_t data_size
);

int rrr_stats_message_new_empty (
		struct rrr_stats_message **message
);

int rrr_stats_message_new (
		struct rrr_stats_message **message,
		uint8_t type,
		uint32_t flags,
		const char *path_postfix,
		const void *data,
		uint32_t data_size
);

int rrr_stats_message_set_path (
		struct rrr_stats_message *message,
		const char *path
);

int rrr_stats_message_duplicate (
		struct rrr_stats_message **target,
		const struct rrr_stats_message *source
);

int rrr_stats_message_destroy (
		struct rrr_stats_message *message
);

#endif /* RRR_STATS_MESSAGE_H */
