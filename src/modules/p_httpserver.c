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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <inttypes.h>

#include "../lib/log.h"
#include "../lib/instance_config.h"
#include "../lib/instances.h"
#include "../lib/threads.h"
#include "../lib/message_broker.h"
#include "../lib/array.h"
#include "../lib/map.h"
#include "../lib/http/http_session.h"
#include "../lib/http/http_server.h"
#include "../lib/net_transport/net_transport_config.h"
#include "../lib/stats/stats_instance.h"
#include "../lib/mqtt/mqtt_topic.h"
#include "../lib/messages/msg_msg.h"
#include "../lib/ip/ip_defines.h"
#include "../lib/util/gnu.h"
#include "../lib/message_holder/message_holder.h"
#include "../lib/message_holder/message_holder_struct.h"
//#include "../ip_util.h"

#define RRR_HTTPSERVER_DEFAULT_PORT_PLAIN			80
#define RRR_HTTPSERVER_DEFAULT_PORT_TLS				443
#define RRR_HTTPSERVER_REQUEST_TOPIC_PREFIX			"httpserver/request/"
#define RRR_HTTPSERVER_RAW_TOPIC_PREFIX				"httpserver/raw/"
#define RRR_HTTPSERVER_RAW_RESPONSE_TIMEOUT_MS		1500

struct httpserver_data {
	struct rrr_instance_runtime_data *thread_data;
	struct rrr_net_transport_config net_transport_config;

	rrr_setting_uint port_plain;
	rrr_setting_uint port_tls;

	struct rrr_map http_fields_accept;

	int do_http_fields_accept_any;
	int do_allow_empty_messages;
	int do_get_raw_response_from_senders;
	int do_receive_raw_data;
	int do_receive_full_request;

	pthread_mutex_t oustanding_responses_lock;
};

static void httpserver_data_cleanup(void *arg) {
	struct httpserver_data *data = arg;
	rrr_net_transport_config_cleanup(&data->net_transport_config);
	rrr_map_clear(&data->http_fields_accept);
}

static int httpserver_data_init (
		struct httpserver_data *data,
		struct rrr_instance_runtime_data *thread_data
) {
	memset(data, '\0', sizeof(*data));

	data->thread_data = thread_data;

	return 0;
}

static int httpserver_parse_config (
		struct httpserver_data *data,
		struct rrr_instance_config_data *config
) {
	int ret = 0;

	if (rrr_net_transport_config_parse (
			&data->net_transport_config,
			config,
			"http_server",
			1,
			RRR_NET_TRANSPORT_PLAIN
	) != 0) {
		ret = 1;
		goto out;
	}

	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_UNSIGNED("http_server_port_tls", port_tls, RRR_HTTPSERVER_DEFAULT_PORT_TLS);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_UNSIGNED("http_server_port_plain", port_plain, RRR_HTTPSERVER_DEFAULT_PORT_PLAIN);

	RRR_INSTANCE_CONFIG_IF_EXISTS_THEN("http_server_port_tls",
			if (data->net_transport_config.transport_type != RRR_NET_TRANSPORT_TLS &&
				data->net_transport_config.transport_type != RRR_NET_TRANSPORT_BOTH
			) {
				RRR_MSG_0("Setting http_server_port_tls is set for httpserver instance %s but TLS transport is not configured.\n",
						config->name);
				ret = 1;
				goto out;
			}
	);

	RRR_INSTANCE_CONFIG_IF_EXISTS_THEN("http_server_port_plain",
			if (data->net_transport_config.transport_type != RRR_NET_TRANSPORT_PLAIN &&
				data->net_transport_config.transport_type != RRR_NET_TRANSPORT_BOTH
			) {
				RRR_MSG_0("Setting http_server_port_plain is set for httpserver instance %s but plain transport is not configured.\n",
						config->name);
				ret = 1;
				goto out;
			}
	);

	if ((ret = rrr_instance_config_parse_comma_separated_associative_to_map(&data->http_fields_accept, config, "http_server_fields_accept", "->")) != 0) {
		RRR_MSG_0("Could not parse setting http_server_fields_accept for instance %s\n",
				config->name);
		goto out;
	}

	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("http_server_fields_accept_any", do_http_fields_accept_any, 0);

	if (RRR_MAP_COUNT(&data->http_fields_accept) > 0 && data->do_http_fields_accept_any != 0) {
		RRR_MSG_0("Setting http_server_fields_accept in instance %s was set while http_server_fields_accept_any was 'yes', this is an invalid configuration.\n",
				config->name);
		ret = 1;
		goto out;
	}

	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("http_server_allow_empty_messages", do_allow_empty_messages, 0);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("http_server_get_raw_response_from_senders", do_get_raw_response_from_senders, 0);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("http_server_receive_raw_data", do_receive_raw_data, 0);
	RRR_INSTANCE_CONFIG_PARSE_OPTIONAL_YESNO("http_server_receive_full_request", do_receive_full_request, 0);

	out:
	return ret;
}

static int httpserver_start_listening (struct httpserver_data *data, struct rrr_http_server *http_server) {
	int ret = 0;

	if (data->net_transport_config.transport_type == RRR_NET_TRANSPORT_PLAIN ||
		data->net_transport_config.transport_type == RRR_NET_TRANSPORT_BOTH
	) {
		if ((ret = rrr_http_server_start_plain(http_server, data->port_plain)) != 0) {
			RRR_MSG_0("Could not start listening in plain mode on port %u in httpserver instance %s\n",
					data->port_plain, INSTANCE_D_NAME(data->thread_data));
			ret = 1;
			goto out;
		}
	}

	if (data->net_transport_config.transport_type == RRR_NET_TRANSPORT_TLS ||
		data->net_transport_config.transport_type == RRR_NET_TRANSPORT_BOTH
	) {
		if ((ret = rrr_http_server_start_tls (
				http_server,
				data->port_tls,
				&data->net_transport_config,
				0
		)) != 0) {
			RRR_MSG_0("Could not start listening in TLS mode on port %u in httpserver instance %s\n",
					data->port_tls, INSTANCE_D_NAME(data->thread_data));
			ret = 1;
			goto out;
		}
	}

	out:
	return ret;
}

struct httpserver_worker_process_field_callback {
	struct rrr_array *array;
	struct httpserver_data *parent_data;
};

static int httpserver_worker_process_field_callback (
		const struct rrr_http_field *field,
		void *arg
) {
	struct httpserver_worker_process_field_callback *callback_data = arg;

	int ret = RRR_HTTP_OK;

	struct rrr_type_value *value_tmp = NULL;
	int do_add_field = 0;
	const char *name_to_use = field->name;

	if (callback_data->parent_data->do_http_fields_accept_any) {
		do_add_field = 1;
	}
	else if (RRR_MAP_COUNT(&callback_data->parent_data->http_fields_accept) > 0) {
		RRR_MAP_ITERATE_BEGIN(&callback_data->parent_data->http_fields_accept);
			if (strcmp(node_tag, field->name) == 0) {
				do_add_field = 1;
				if (node->value != NULL && node->value_size > 0 && *(node->value) != '\0') {
					// Do name translation
					name_to_use = node->value;
					RRR_LL_ITERATE_LAST();
				}
			}
		RRR_MAP_ITERATE_END();
	}

	if (do_add_field != 1) {
		goto out;
	}

	if (field->content_type != NULL && strcmp(field->content_type, RRR_MESSAGE_MIME_TYPE) == 0) {
		if (rrr_type_value_allocate_and_import_raw (
				&value_tmp,
				&rrr_type_definition_msg,
				field->value,
				field->value + field->value_size,
				strlen(name_to_use),
				name_to_use,
				field->value_size,
				1 // <-- We only support one message per field
		) != 0) {
			RRR_MSG_0("Failed to import RRR message from HTTP field\n");
			ret = 1;
			goto out;
		}

		RRR_LL_APPEND(callback_data->array, value_tmp);
		value_tmp = NULL;
	}
	else if (field->value != NULL && field->value_size > 0) {
		ret = rrr_array_push_value_str_with_tag_with_size (
				callback_data->array,
				name_to_use,
				field->value,
				field->value_size
		);
	}
	else {
		ret = rrr_array_push_value_u64_with_tag (
				callback_data->array,
				name_to_use,
				0
		);
	}

	if (ret != 0) {
		RRR_MSG_0("Error while pushing field to array in __rrr_http_server_worker_process_field_callback\n");
		ret = RRR_HTTP_HARD_ERROR;
		goto out;
	}

	out:
	if (value_tmp != NULL) {
		rrr_type_value_destroy(value_tmp);
	}
	return ret;
}

struct httpserver_write_message_callback_data {
	struct rrr_array *array;
	const char *topic;
};

// NOTE : Worker thread CTX in httpserver_write_message_callback
static int httpserver_write_message_callback (
		struct rrr_msg_msg_holder *new_entry,
		void *arg
) {
	struct httpserver_write_message_callback_data *callback_data = arg;

	int ret = RRR_MESSAGE_BROKER_OK;

	struct rrr_msg_msg *new_message = NULL;

	if (RRR_LL_COUNT(callback_data->array) > 0) {
		ret = rrr_array_new_message_from_collection (
				&new_message,
				callback_data->array,
				rrr_time_get_64(),
				callback_data->topic,
				strlen(callback_data->topic)
		);
	}
	else {
		if ((ret = rrr_msg_msg_new_empty (
				&new_message,
				MSG_TYPE_MSG,
				MSG_CLASS_DATA,
				rrr_time_get_64(),
				strlen(callback_data->topic),
				0
		)) == 0) { // Note : Check for OK
			memcpy(MSG_TOPIC_PTR(new_message), callback_data->topic, new_message->topic_length);
		}
	}

	if (ret != 0) {
		RRR_MSG_0("Could not create message in httpserver_write_message_callback\n");
		ret = RRR_MESSAGE_BROKER_ERR;
		goto out;
	}

	new_entry->message = new_message;
	new_entry->data_length = MSG_TOTAL_SIZE(new_message);
	new_message = NULL;

	out:
	rrr_msg_msg_holder_unlock(new_entry);
	return ret;
}

struct httpserver_callback_data {
	struct httpserver_data *httpserver_data;
};

static int httpserver_generate_unique_topic (
		char **result,
		const char *prefix,
		rrr_http_unique_id unique_id
) {
	if (rrr_asprintf(result, "%s%" PRIu64, prefix, unique_id) <= 0) {
		RRR_MSG_0("Could not create topic in httpserver_generate_unique_topic\n");
		return 1;
	}
	return 0;
}

static int httpserver_receive_callback_get_fields (
		struct rrr_array *target_array,
		struct httpserver_data *data,
		const struct rrr_http_part *part
) {
	int ret = RRR_HTTP_OK;

	struct httpserver_worker_process_field_callback field_callback_data = {
			target_array,
			data
	};

	if ((ret = rrr_http_part_fields_iterate_const (
			part,
			httpserver_worker_process_field_callback,
			&field_callback_data
	)) != RRR_HTTP_OK) {
		goto out;
	}

	out:
	return ret;
}

struct receive_get_response_callback_data {
	char **response;
	size_t response_size;
	const char *topic_filter;
};

static int httpserver_receive_get_response_callback (
		RRR_MODULE_POLL_CALLBACK_SIGNATURE
) {
	struct receive_get_response_callback_data *callback_data = arg;
	struct rrr_msg_msg *msg = entry->message;

	int ret = RRR_FIFO_SEARCH_KEEP;

	char *response = NULL;

	if (MSG_TOPIC_LENGTH(msg) > 0) {
		if ((ret = rrr_mqtt_topic_match_str_with_end (
				callback_data->topic_filter,
				MSG_TOPIC_PTR(msg),
				MSG_TOPIC_PTR(msg) + MSG_TOPIC_LENGTH(msg)
		)) != 0) {
			if (ret == RRR_MQTT_TOKEN_MISMATCH) {
				ret = RRR_FIFO_SEARCH_KEEP;
				goto out;
			}
			RRR_MSG_0("Error while matching topic in httpserver_receive_get_response_callback\n");
			goto out;
		}
	}

	// Even if data is 0 length, allocate a byte to make pointer non-zero so that caller
	// can see that response has been received
	if ((response = malloc(MSG_DATA_LENGTH(msg) + 1)) == NULL) {
		RRR_MSG_0("Could not allocate response memory in httpserver_receive_get_response_callback\n");
		ret = RRR_FIFO_GLOBAL_ERR;
		goto out;
	}

	memcpy(response, MSG_DATA_PTR(msg), MSG_DATA_LENGTH(msg));

	*(callback_data->response) = response;
	callback_data->response_size = MSG_DATA_LENGTH(msg);

	response = NULL;

	ret = RRR_FIFO_SEARCH_GIVE|RRR_FIFO_SEARCH_FREE;

	out:
	RRR_FREE_IF_NOT_NULL(response);
	rrr_msg_msg_holder_unlock(entry);
	return ret;
}

static int httpserver_receive_get_raw_response (
		struct httpserver_data *data,
		struct rrr_http_part *response_part,
		uint64_t unique_id
) {
	int ret = 0;

	char *topic = NULL;
	char *response = NULL;

	if ((ret = httpserver_generate_unique_topic (
			&topic,
			RRR_HTTPSERVER_RAW_TOPIC_PREFIX,
			unique_id
	)) != 0) {
		goto out;
	}

	// Message has already been generated by receive raw callback. We await
	// a response here.

	struct receive_get_response_callback_data callback_data = {
			&response,
			0,
			topic
	};

	// We may spend some time in this loop. This is not a problem as each request
	// is processed by a dedicated thread.
	uint64_t timeout = rrr_time_get_64() + RRR_HTTPSERVER_RAW_RESPONSE_TIMEOUT_MS * 1000;
	while (rrr_time_get_64() < timeout) {
		if ((ret = rrr_poll_do_poll_search (
				data->thread_data,
				&data->thread_data->poll,
				httpserver_receive_get_response_callback,
				&callback_data,
				2
		)) != 0) {
			RRR_MSG_0("Error from poll in httpserver_receive_callback\n");
			goto out;
		}

		if (*(callback_data.response) != NULL) {
			break;
		}
	}

	if (*(callback_data.response) == NULL) {
		RRR_DBG_1("Timeout while waiting for response from senders in httpserver instance %s\n",
				INSTANCE_D_NAME(data->thread_data));
		ret = RRR_HTTP_SOFT_ERROR;
		goto out;
	}

	RRR_DBG_3("httpserver instance %s got a response from senders with filter %s size %lu\n",
			INSTANCE_D_NAME(data->thread_data), callback_data.topic_filter, callback_data.response_size);

	// Will set our pointer to NULL
	rrr_http_part_set_allocated_raw_response (
			response_part,
			callback_data.response,
			callback_data.response_size
	);

	out:
	RRR_FREE_IF_NOT_NULL(topic);
	RRR_FREE_IF_NOT_NULL(response);
	return ret;
}

static int httpserver_receive_callback_full_request (
		struct rrr_array *target_array,
		const struct rrr_http_part *part,
		const char *data_ptr
) {
	int ret = 0;

	const char *body_ptr = RRR_HTTP_PART_BODY_PTR(data_ptr,part);
	size_t body_len = RRR_HTTP_PART_BODY_LENGTH(part);

	// http_method, http_endpoint, http_body, http_content_transfer_encoding, http_content_type

	const struct rrr_http_header_field *content_type = rrr_http_part_header_field_get(part, "content-type");
	const struct rrr_http_header_field *content_transfer_encoding = rrr_http_part_header_field_get(part, "content-transfer-encoding");

	ret |= rrr_array_push_value_str_with_tag(target_array, "http_method", part->request_method_str);
	ret |= rrr_array_push_value_str_with_tag(target_array, "http_endpoint", part->request_uri);

	if (content_type != NULL && *(content_type->value) != '\0') {
		ret |= rrr_array_push_value_str_with_tag (
				target_array,
				"http_content_type",
				content_type->value
		);
	}

	if (content_transfer_encoding != NULL && *(content_transfer_encoding->value) != '\0') {
		ret |= rrr_array_push_value_str_with_tag (
				target_array,
				"http_content_transfer_encoding",
				content_transfer_encoding->value
		);
	}

	if (body_len > 0){
		ret |= rrr_array_push_value_blob_with_tag_with_size (
				target_array, "http_body", body_ptr, body_len
		);
	}

	if (ret != 0) {
		RRR_MSG_0("Failed to add full request fields in httpserver_receive_callback_full_request\n");
		goto out;
	}

	out:
	return ret;
}

// NOTE : Worker thread CTX in httpserver_receive_callback
static int httpserver_receive_callback (
		RRR_HTTP_SESSION_RECEIVE_CALLBACK_ARGS
) {
	struct httpserver_callback_data *receive_callback_data = arg;
	struct httpserver_data *data = receive_callback_data->httpserver_data;

	(void)(overshoot_bytes);

	int ret = 0;

	char *request_topic = NULL;
	struct rrr_array array_tmp = {0};

	if ((ret = httpserver_generate_unique_topic (
			&request_topic,
			RRR_HTTPSERVER_REQUEST_TOPIC_PREFIX,
			unique_id
	)) != 0) {
		goto out;
	}

	struct httpserver_write_message_callback_data write_callback_data = {
			&array_tmp,
			request_topic
	};

	if (data->do_receive_full_request) {
		if ((RRR_HTTP_PART_BODY_LENGTH(request_part) == 0 && !data->do_allow_empty_messages)) {
			RRR_DBG_3("Zero length body from HTTP client, not creating RRR full request message\n");
		}
		else if ((ret = httpserver_receive_callback_full_request (
				&array_tmp,
				request_part,
				data_ptr
		)) != 0) {
			goto out;
		}
	}

	if (request_part->request_method == RRR_HTTP_METHOD_OPTIONS) {
		// Don't receive fields, let server framework send default reply
		RRR_DBG_3("Not processing fields from OPTIONS request\n");
	}
	else if ((ret = httpserver_receive_callback_get_fields (
			&array_tmp,
			data,
			request_part
	)) != 0) {
		goto out;
	}

	if (RRR_LL_COUNT(&array_tmp) == 0 && data->do_allow_empty_messages == 0) {
		RRR_DBG_3("No array values set after processing request from HTTP client, not creating RRR array message\n");
	}
	else {
		if ((ret = rrr_message_broker_write_entry (
				INSTANCE_D_BROKER(data->thread_data),
				INSTANCE_D_HANDLE(data->thread_data),
				sockaddr,
				socklen,
				RRR_IP_TCP,
				httpserver_write_message_callback,
				&write_callback_data
		)) != 0) {
			RRR_MSG_0("Error while saving message in httpserver_receive_callback\n");
			ret = RRR_HTTP_HARD_ERROR;
			goto out;
		}
	}

	if (data->do_get_raw_response_from_senders) {
		if ((ret = httpserver_receive_get_raw_response (
				data,
				response_part,
				unique_id
		)) != 0) {
			if (ret == RRR_HTTP_SOFT_ERROR) {
				response_part->response_code = RRR_HTTP_RESPONSE_CODE_GATEWAY_TIMEOUT;
				ret = RRR_HTTP_OK;
			}
			goto out;
		}
	}

	out:
	rrr_array_clear(&array_tmp);
	RRR_FREE_IF_NOT_NULL(request_topic);
	return ret;
}

struct receive_raw_broker_callback_data {
	struct httpserver_data *parent_data;
	const char *data;
	ssize_t data_size;
	rrr_http_unique_id unique_id;
};

static int httpserver_receive_raw_broker_callback (
		struct rrr_msg_msg_holder *entry_new,
		void *arg
) {
	struct receive_raw_broker_callback_data *write_callback_data = arg;

	int ret = 0;

	char *topic = NULL;

	if (write_callback_data->parent_data->do_get_raw_response_from_senders) {
		if ((ret = httpserver_generate_unique_topic(
				&topic,
				RRR_HTTPSERVER_RAW_TOPIC_PREFIX,
				write_callback_data->unique_id
		)) != 0) {
			goto out;
		}
	}

	if ((ret = rrr_msg_msg_new_with_data (
			(struct rrr_msg_msg **) &entry_new->message,
			MSG_TYPE_MSG,
			MSG_CLASS_DATA,
			rrr_time_get_64(),
			topic,
			(topic != NULL ? strlen(topic) : 0),
			write_callback_data->data,
			write_callback_data->data_size
	)) != 0) {
		RRR_MSG_0("Could not create message in httpserver_receive_raw_broker_callback\n");
		goto out;
	}

	entry_new->data_length = MSG_TOTAL_SIZE((struct rrr_msg_msg *) entry_new->message);

	RRR_DBG_3("httpserver instance %s created raw httpserver data message with data size %li topic %s\n",
			INSTANCE_D_NAME(write_callback_data->parent_data->thread_data), write_callback_data->data_size, topic);

	out:
	RRR_FREE_IF_NOT_NULL(topic);
	rrr_msg_msg_holder_unlock(entry_new);
	return ret;
}

// NOTE : Worker thread CTX in httpserver_receive_raw_callback
static int httpserver_receive_raw_callback (
		RRR_HTTP_SESSION_RAW_RECEIVE_CALLBACK_ARGS
) {
	struct httpserver_callback_data *receive_callback_data = arg;

	struct receive_raw_broker_callback_data write_callback_data = {
		receive_callback_data->httpserver_data,
		data,
		data_size,
		unique_id
	};

	return rrr_message_broker_write_entry (
			INSTANCE_D_BROKER_ARGS(receive_callback_data->httpserver_data->thread_data),
			NULL,
			0,
			0,
			httpserver_receive_raw_broker_callback,
			&write_callback_data
	);

	return 0;
}

int httpserver_unique_id_generator_callback (
		RRR_HTTP_SESSION_UNIQUE_ID_GENERATOR_CALLBACK_ARGS
) {
	struct httpserver_callback_data *data = arg;

	return rrr_message_broker_get_next_unique_id (
			result,
			INSTANCE_D_BROKER_ARGS(data->httpserver_data->thread_data)
	);
}

// If we receive messages from senders which no worker seem to want, we must delete it
static int httpserver_housekeep_callback (RRR_MODULE_POLL_CALLBACK_SIGNATURE) {
	struct httpserver_callback_data *callback_data = arg;

	int ret = RRR_FIFO_SEARCH_KEEP;

	struct rrr_msg_msg *msg = entry->message;

	uint64_t timeout = msg->timestamp + RRR_HTTPSERVER_RAW_RESPONSE_TIMEOUT_MS * 1000;

	if (rrr_time_get_64() > timeout) {
		RRR_DBG_1("httpserver instance %s deleting message from senders of size %li which has timed out\n",
				INSTANCE_D_NAME(callback_data->httpserver_data->thread_data), MSG_TOTAL_SIZE(msg));
		ret = RRR_FIFO_SEARCH_GIVE|RRR_FIFO_SEARCH_FREE;
	}

	rrr_msg_msg_holder_unlock(entry);
	return ret;
}

static void *thread_entry_httpserver (struct rrr_thread *thread) {
	struct rrr_instance_runtime_data *thread_data = thread->private_data;
	struct httpserver_data *data = thread_data->private_data = thread_data->private_memory;

	if (httpserver_data_init(data, thread_data) != 0) {
		RRR_MSG_0("Could not initialize thread_data in httpserver instance %s\n", INSTANCE_D_NAME(thread_data));
		pthread_exit(0);
	}

	RRR_DBG_1 ("httpserver thread thread_data is %p\n", thread_data);

	pthread_cleanup_push(httpserver_data_cleanup, data);

	rrr_thread_set_state(thread, RRR_THREAD_STATE_INITIALIZED);
	rrr_thread_signal_wait(thread, RRR_THREAD_SIGNAL_START);
	rrr_thread_set_state(thread, RRR_THREAD_STATE_RUNNING);

	if (httpserver_parse_config(data, INSTANCE_D_CONFIG(thread_data)) != 0) {
		goto out_message;
	}

	rrr_instance_config_check_all_settings_used(thread_data->init_data.instance_config);

	RRR_DBG_1 ("httpserver started thread %p\n", thread_data);

	struct rrr_http_server *http_server = NULL;

	if (rrr_http_server_new(&http_server) != 0) {
		RRR_MSG_0("Could not create HTTP server in httpserver instance %s\n",
				INSTANCE_D_NAME(thread_data));
		goto out_message;
	}

	// TODO : There are occasional (?) reports from valgrind that http_server is
	//        not being freed upon program exit.

	pthread_cleanup_push(rrr_http_server_destroy_void, http_server);

	if (httpserver_start_listening(data, http_server) != 0) {
		goto out_cleanup_httpserver;
	}

	unsigned int accept_count_total = 0;
	uint64_t prev_stats_time = rrr_time_get_64();

	struct httpserver_callback_data callback_data = {
			data
	};

	while (rrr_thread_check_encourage_stop(thread) != 1) {
		rrr_thread_update_watchdog_time(thread);

		int accept_count = 0;

		if (rrr_http_server_tick (
				&accept_count,
				http_server,
				httpserver_unique_id_generator_callback,
				&callback_data,
				(data->do_receive_raw_data ? httpserver_receive_raw_callback : NULL),
				(data->do_receive_raw_data ? &callback_data : NULL),
				httpserver_receive_callback,
				&callback_data
		) != 0) {
			RRR_MSG_0("Failure in main loop in httpserver instance %s\n",
					INSTANCE_D_NAME(thread_data));
			break;
		}

		if (accept_count == 0) {
			rrr_posix_usleep(50000); // 50 ms
		}
		else {
			accept_count_total += accept_count;
		}

		uint64_t time_now = rrr_time_get_64();
		if (time_now > prev_stats_time + 1000000) {
			rrr_stats_instance_update_rate(INSTANCE_D_STATS(thread_data), 1, "accepted", accept_count_total);

			accept_count_total = 0;

			prev_stats_time = time_now;
		}

		if (rrr_poll_do_poll_search (
				data->thread_data,
				&data->thread_data->poll,
				httpserver_housekeep_callback,
				&callback_data,
				0
		) != 0) {
			RRR_MSG_0("Error from poll in httpserver instance %s\n", INSTANCE_D_NAME(thread_data));
			break;
		}
	}

	out_cleanup_httpserver:
	pthread_cleanup_pop(1);

	out_message:
	RRR_DBG_1 ("Thread httpserver %p exiting\n", thread);

	pthread_cleanup_pop(1);
	pthread_exit(0);
}

static int test_config (struct rrr_instance_config_data *config) {
	RRR_DBG_1("Dummy configuration test for instance %s\n", config->name);
	return 0;
}

static struct rrr_module_operations module_operations = {
		NULL,
		thread_entry_httpserver,
		NULL,
		test_config,
		NULL,
		NULL
};

static const char *module_name = "httpserver";

__attribute__((constructor)) void load(void) {
}

void init(struct rrr_instance_module_data *data) {
	data->private_data = NULL;
	data->module_name = module_name;
	data->type = RRR_MODULE_TYPE_FLEXIBLE;
	data->operations = module_operations;
	data->dl_ptr = NULL;
}

void unload(void) {
	RRR_DBG_1 ("Destroy httpserver module\n");
}