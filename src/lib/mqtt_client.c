/*

Read Route Record

Copyright (C) 2019 Atle Solbakken atle@goliathdns.no

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

#include <inttypes.h>
#include <stdlib.h>

#include "../global.h"
#include "mqtt_client.h"
#include "mqtt_common.h"
#include "mqtt_subscription.h"

struct set_connection_settings_callback_data {
	uint16_t keep_alive;
	const struct rrr_mqtt_p_protocol_version *protocol_version;
	struct rrr_mqtt_session *session;
};

static int __rrr_mqtt_client_connect_set_connection_settings(struct rrr_mqtt_conn *connection, void *arg) {
	int ret = RRR_MQTT_CONN_OK;

	struct set_connection_settings_callback_data *callback_data = arg;

	RRR_MQTT_COMMON_CALL_CONN_AND_CHECK_RETURN_GENERAL(
			rrr_mqtt_conn_iterator_ctx_set_data_from_connect (
					connection,
					callback_data->keep_alive,
					callback_data->protocol_version,
					callback_data->session
			),
			goto out,
			" while setting new keep-alive on connection in __rrr_mqtt_client_connect_set_connection_settings"
	);

	out:
	return ret;
}

int rrr_mqtt_client_connection_check_alive (
		int *alive,
		int *send_allowed,
		struct rrr_mqtt_client_data *data,
		struct rrr_mqtt_conn *connection
) {
	return rrr_mqtt_conn_check_alive(alive, send_allowed, &data->mqtt_data.connections, connection);
}

int rrr_mqtt_client_publish (
		struct rrr_mqtt_client_data *data,
		struct rrr_mqtt_conn *connection,
		struct rrr_mqtt_p_publish *publish
) {
	int ret = 0;

	RRR_MQTT_COMMON_CALL_SESSION_AND_CHECK_RETURN_GENERAL(
			data->mqtt_data.sessions->methods->send_packet (
					data->mqtt_data.sessions,
					&connection->session,
					(struct rrr_mqtt_p *) publish
			),
			goto out,
			" while sending PUBLISH packet in rrr_mqtt_client_publish\n"
	);

	out:
	return ret;
}


int rrr_mqtt_client_subscribe (
		struct rrr_mqtt_client_data *data,
		struct rrr_mqtt_conn *connection,
		const struct rrr_mqtt_subscription_collection *subscriptions
) {
	int ret = 0;

	if ((ret = rrr_mqtt_subscription_collection_count(subscriptions)) == 0) {
		VL_DEBUG_MSG_1("No subscriptions in rrr_mqtt_client_subscribe\n");
		goto out;
	}
	else if (ret < 0) {
		VL_BUG("Unknown return value %i from rrr_mqtt_subscription_collection_count in rrr_mqtt_client_subscribe\n", ret);
	}
	ret = 0;

	if (data->protocol_version == NULL) {
		VL_MSG_ERR("Protocol version not set in rrr_mqtt_client_send_subscriptions\n");
		ret = 1;
		goto out;
	}

	struct rrr_mqtt_p_subscribe *subscribe = (struct rrr_mqtt_p_subscribe *) rrr_mqtt_p_allocate(
			RRR_MQTT_P_TYPE_SUBSCRIBE,
			data->protocol_version
	);
	if (subscribe == NULL) {
		VL_MSG_ERR("Could not allocate SUBSCRIBE message in rrr_mqtt_client_send_subscriptions\n");
		ret = 1;
		goto out;
	}

	RRR_MQTT_P_LOCK(subscribe);

	if (rrr_mqtt_subscription_collection_append_unique_copy_from_collection(subscribe->subscriptions, subscriptions, 0) != 0) {
		VL_MSG_ERR("Could not add subscriptions to SUBSCRIBE message in rrr_mqtt_client_send_subscriptions\n");
		goto out_unlock;
	}

	RRR_MQTT_P_UNLOCK(subscribe);

	RRR_MQTT_COMMON_CALL_SESSION_AND_CHECK_RETURN_GENERAL(
			data->mqtt_data.sessions->methods->send_packet (
					data->mqtt_data.sessions,
					&connection->session,
					(struct rrr_mqtt_p *) subscribe
			),
			goto out_decref,
			" while sending SUBSCRIBE packet in rrr_mqtt_client_send_subscriptions\n"
	);

	goto out_decref;
	out_unlock:
		RRR_MQTT_P_UNLOCK(subscribe);
	out_decref:
		RRR_MQTT_P_DECREF(subscribe);
	out:
		return (ret != 0);
}

struct rrr_mqtt_client_property_override {
	struct rrr_mqtt_property *property;
};

int rrr_mqtt_client_connect (
		struct rrr_mqtt_conn **connection,
		struct rrr_mqtt_client_data *data,
		const char *server,
		uint16_t port,
		uint8_t version,
		uint16_t keep_alive,
		uint8_t clean_start,
		const struct rrr_mqtt_property_collection *connect_properties
) {
	int ret = 0;

	struct rrr_mqtt_data *mqtt_data = &data->mqtt_data;

	struct rrr_mqtt_p_connect *connect = NULL;
	struct rrr_mqtt_session *session = NULL;

	// Sleep a bit in case server runs in the same RRR program
	usleep(500000); // 500ms

	if (rrr_mqtt_conn_collection_connect(connection, &data->mqtt_data.connections, port, server) != 0) {
		VL_MSG_ERR("Could not connect to mqtt server '%s'\n", server);
		ret = 1;
		goto out_nolock;
	}

	if (*connection == NULL) {
		VL_MSG_ERR("Could not connect to mqtt server '%s'\n", server);
		return 1;
	}

	const struct rrr_mqtt_p_protocol_version *protocol_version = rrr_mqtt_p_get_protocol_version(version);
	if (protocol_version == NULL) {
		VL_BUG("Invalid protocol version %u in rrr_mqtt_client_connect\n", version);
	}

	connect = (struct rrr_mqtt_p_connect *) rrr_mqtt_p_allocate(RRR_MQTT_P_TYPE_CONNECT, protocol_version);
	RRR_MQTT_P_LOCK(connect);

	connect->client_identifier = malloc(strlen(data->mqtt_data.client_name) + 1);
	if (connect->client_identifier == NULL) {
		VL_MSG_ERR("Could not allocate memory in rrr_mqtt_client_connect\n");
		ret = 1;
		goto out;
	}
	strcpy(connect->client_identifier, data->mqtt_data.client_name);

	connect->keep_alive = keep_alive;
	connect->connect_flags |= (clean_start != 0)<<1;
	// Will QoS
	// connect->connect_flags |= 2 << 3;

	if (rrr_mqtt_property_collection_add_from_collection(&connect->properties, connect_properties) != 0) {
		VL_MSG_ERR("Could not add properties to CONNECT packet in rrr_mqtt_client_connect\n");
		ret = 1;
		goto out;
	}

	if (version >= 5) {
		struct rrr_mqtt_property *session_expiry = rrr_mqtt_property_collection_get_property (
				&connect->properties,
				RRR_MQTT_PROPERTY_SESSION_EXPIRY_INTERVAL,
				0
		);

		if (session_expiry == NULL) {
			// Default for version 3.1 is that sessions do not expire,
			// only use clean session to control this
			data->session_properties.session_expiry = 0xffffffff;

			if (rrr_mqtt_property_collection_add_uint32 (
					&connect->properties,
					RRR_MQTT_PROPERTY_SESSION_EXPIRY_INTERVAL,
					data->session_properties.session_expiry
			) != 0) {
				VL_MSG_ERR("Could not set session expiry for CONNECT packet in rrr_mqtt_client_connect\n");
				ret = 1;
				goto out;
			}
		}
	}

	data->protocol_version = protocol_version;
	data->session_properties = rrr_mqtt_common_default_session_properties;

	struct rrr_mqtt_common_parse_properties_data_connect callback_data = {
			&connect->properties,
			RRR_MQTT_P_5_REASON_OK,
			&data->session_properties
	};

	// After adding properties to the CONNECT packet, read out all values and
	// update the session properties. This will fail if non-CONNECT properties
	// has been used.
	uint8_t reason_v5 = 0;
	RRR_MQTT_COMMON_HANDLE_PROPERTIES (
			&connect->properties,
			connect,
			rrr_mqtt_common_handler_connect_handle_properties_callback,
			goto out
	);

	int session_present = 0;
	if ((ret = mqtt_data->sessions->methods->get_session (
			&session,
			mqtt_data->sessions,
			connect->client_identifier,
			&session_present,
			0 // Create if non-existent client ID
	)) != RRR_MQTT_SESSION_OK || session == NULL) {
		ret = RRR_MQTT_CONN_INTERNAL_ERROR;
		VL_MSG_ERR("Internal error getting session in rrr_mqtt_client_connect\n");
		goto out;
	}

	if ((ret = mqtt_data->sessions->methods->init_session (
			mqtt_data->sessions,
			&session,
			callback_data.session_properties,
			mqtt_data->retry_interval_usec,
			RRR_MQTT_CLIENT_MAX_IN_FLIGHT,
			RRR_MQTT_CLIENT_COMPLETE_PUBLISH_GRACE_TIME,
			RRR_MQTT_P_CONNECT_GET_FLAG_CLEAN_START(connect),
			1, // Local delivery (check received PUBLISH against subscriptions and deliver locally)
			&session_present
	)) != RRR_MQTT_SESSION_OK) {
		if ((ret & RRR_MQTT_SESSION_DELETED) != 0) {
			VL_MSG_ERR("New session was deleted in rrr_mqtt_client_connect\n");
		}
		else {
			VL_MSG_ERR("Error while initializing session in rrr_mqtt_client_connect, return was %i\n", ret);
		}
		ret = 1;
		goto out;
	}

	if (rrr_mqtt_conn_with_iterator_ctx_do (
			&data->mqtt_data.connections,
			*connection,
			(struct rrr_mqtt_p *) connect,
			rrr_mqtt_conn_iterator_ctx_send_packet
	) != 0) {
		VL_MSG_ERR("Could not send CONNECT packet in rrr_mqtt_client_connect");
		ret = 1;
		goto out;
	}

	{
		struct set_connection_settings_callback_data callback_data = {
			connect->keep_alive,
			connect->protocol_version,
			session
		};

		if (rrr_mqtt_conn_with_iterator_ctx_do_custom (
				&data->mqtt_data.connections,
				*connection,
				__rrr_mqtt_client_connect_set_connection_settings,
				&callback_data
		) != 0) {
			VL_MSG_ERR("Could not set protocol version and keep alive from CONNECT packet in rrr_mqtt_client_connect");
			ret = 1;
			goto out;
		}
	}

	out:
		RRR_MQTT_P_UNLOCK(connect);
	out_nolock:
		RRR_MQTT_P_DECREF_IF_NOT_NULL(connect);
		return ret;
}

static int __rrr_mqtt_client_handle_connack (RRR_MQTT_TYPE_HANDLER_DEFINITION) {
	int ret = RRR_MQTT_CONN_OK;

	struct rrr_mqtt_client_data *client_data = (struct rrr_mqtt_client_data *) mqtt_data;
	(void)(connection);

	struct rrr_mqtt_p_connack *connack = (struct rrr_mqtt_p_connack *) packet;

	RRR_MQTT_P_LOCK(packet);

	if (connack->reason_v5 != RRR_MQTT_P_5_REASON_OK) {
		VL_MSG_ERR("CONNACK: Connection failed with reason '%s'\n", connack->reason->description);
		ret = RRR_MQTT_CONN_SOFT_ERROR | RRR_MQTT_CONN_DESTROY_CONNECTION;
		goto out;
	}

	rrr_mqtt_conn_iterator_ctx_update_state (connection, packet, RRR_MQTT_CONN_UPDATE_STATE_DIRECTION_IN);

	if (connack->session_present == 0) {
		RRR_MQTT_COMMON_CALL_SESSION_CHECK_RETURN_TO_CONN_ERRORS_GENERAL(
				mqtt_data->sessions->methods->clean_session(mqtt_data->sessions, &connection->session),
				goto out,
				" while cleaning session in __rrr_mqtt_client_handle_connack"
		);
	}

	uint8_t reason_v5 = 0;
	struct rrr_mqtt_common_parse_properties_data_connect callback_data = {
			&connack->properties,
			RRR_MQTT_P_5_REASON_OK,
			&client_data->session_properties
	};

	RRR_MQTT_COMMON_HANDLE_PROPERTIES (
			&connack->properties,
			connack,
			rrr_mqtt_common_handler_connack_handle_properties_callback,
			goto out
	);

	RRR_MQTT_COMMON_CALL_SESSION_CHECK_RETURN_TO_CONN_ERRORS_GENERAL(
			mqtt_data->sessions->methods->reset_properties(
					mqtt_data->sessions,
					&connection->session,
					&client_data->session_properties
			),
			goto out,
			" while resetting properties in __rrr_mqtt_client_handle_connack"
	);

	if (client_data->session_properties.server_keep_alive > 0) {
		if (client_data->session_properties.server_keep_alive > 0xffff) {
			VL_BUG("Session server keep alive was >0xffff in __rrr_mqtt_client_handle_connack\n");
		}
		RRR_MQTT_COMMON_CALL_CONN_AND_CHECK_RETURN_GENERAL(
				rrr_mqtt_conn_iterator_ctx_set_data_from_connect (
						connection,
						client_data->session_properties.server_keep_alive,
						connack->protocol_version,
						connection->session
				),
				goto out,
				" while setting new keep-alive on connection"
		);
	}

	VL_DEBUG_MSG_1("Received CONNACK, now connected\n");

	out:
		RRR_MQTT_P_UNLOCK(packet);
		return ret;
}

static int __rrr_mqtt_client_handle_suback (RRR_MQTT_TYPE_HANDLER_DEFINITION) {
	int ret = RRR_MQTT_CONN_OK;

	struct rrr_mqtt_client_data *client_data = (struct rrr_mqtt_client_data *) mqtt_data;
	(void)(connection);

	unsigned int match_count = 0;
	RRR_MQTT_COMMON_CALL_SESSION_CHECK_RETURN_TO_CONN_ERRORS_GENERAL(
		mqtt_data->sessions->methods->receive_packet (
					mqtt_data->sessions,
					&connection->session,
					packet,
					&match_count
			),
			goto out,
			" while handling SUBACK packet"
	);

	if (match_count == 0) {
		VL_MSG_ERR("Received SUBACK but did not find corresponding SUBSCRIBE packet, possible duplicate\n");
		goto out;
	}

	if (client_data->suback_handler != NULL) {
		if (client_data->suback_handler(client_data, packet, client_data->suback_handler_arg) != 0) {
			VL_MSG_ERR("Error from custom suback handler in __rrr_mqtt_client_handle_suback\n");
			ret = RRR_MQTT_CONN_SOFT_ERROR;
		}
	}

	out:
	return ret;
}

int __rrr_mqtt_client_handle_pingresp (RRR_MQTT_TYPE_HANDLER_DEFINITION) {
	int ret = RRR_MQTT_CONN_OK;

	unsigned int match_count = 0;
	RRR_MQTT_COMMON_CALL_SESSION_CHECK_RETURN_TO_CONN_ERRORS_GENERAL(
		mqtt_data->sessions->methods->receive_packet (
					mqtt_data->sessions,
					&connection->session,
					packet,
					&match_count
			),
			goto out,
			" while handling PINGRESP packet"
	);

	if (match_count == 0) {
		VL_DEBUG_MSG_1("Received PINGRESP with no matching PINGREQ\n");
	}

	out:
	return ret;
}

static const struct rrr_mqtt_type_handler_properties handler_properties[] = {
	{NULL},
	{NULL},
	{__rrr_mqtt_client_handle_connack},
	{rrr_mqtt_common_handle_publish},
	{rrr_mqtt_common_handle_puback_pubcomp},
	{rrr_mqtt_common_handle_pubrec},
	{rrr_mqtt_common_handle_pubrel},
	{rrr_mqtt_common_handle_puback_pubcomp},
	{NULL},
	{__rrr_mqtt_client_handle_suback},
	{NULL},
	{NULL},
	{NULL},
	{__rrr_mqtt_client_handle_pingresp},
	{rrr_mqtt_common_handle_disconnect},
	{NULL}
};

static int __rrr_mqtt_client_event_handler (
		struct rrr_mqtt_conn *connection,
		int event,
		void *static_arg,
		void *arg
) {
	struct rrr_mqtt_client_data *data = static_arg;

	(void)(connection);
	(void)(data);
	(void)(arg);

	int ret = RRR_MQTT_CONN_OK;

	switch (event) {
		case RRR_MQTT_CONN_EVENT_DISCONNECT:
			break;
		default:
			break;
	};

	return ret;
}

void rrr_mqtt_client_destroy (struct rrr_mqtt_client_data *client) {
	rrr_mqtt_common_data_destroy(&client->mqtt_data);
	rrr_mqtt_session_properties_destroy(&client->session_properties);
	free(client);
}

int rrr_mqtt_client_new (
		struct rrr_mqtt_client_data **client,
		const struct rrr_mqtt_common_init_data *init_data,
		int (*session_initializer)(struct rrr_mqtt_session_collection **sessions, void *arg),
		void *session_initializer_arg,
		int (*suback_handler)(struct rrr_mqtt_client_data *data, struct rrr_mqtt_p *packet, void *private_arg),
		void *suback_handler_arg
) {
	int ret = 0;

	struct rrr_mqtt_client_data *result = malloc(sizeof(*result));

	if (result == NULL) {
		VL_MSG_ERR("Could not allocate memory in rrr_mqtt_client_new\n");
		ret = 1;
		goto out;
	}

	memset (result, '\0', sizeof(*result));

	ret = rrr_mqtt_common_data_init (
			&result->mqtt_data,
			handler_properties,
			init_data,
			session_initializer,
			session_initializer_arg,
			__rrr_mqtt_client_event_handler,
			result
	);

	if (ret != 0) {
		VL_MSG_ERR("Could not initialize MQTT common data in rrr_mqtt_client_new\n");
		ret = 1;
		goto out_free;
	}

	result->last_pingreq_time = time_get_64();
	result->suback_handler = suback_handler;
	result->suback_handler_arg = suback_handler_arg;

	*client = result;

	goto out;
	out_free:
		free(result);
	out:
		return ret;
}

struct exceeded_keep_alive_callback_data {
	struct rrr_mqtt_client_data *data;
};

static int __rrr_mqtt_client_exceeded_keep_alive_callback (struct rrr_mqtt_conn *connection, void *arg) {
	int ret = RRR_MQTT_CONN_OK;

	struct exceeded_keep_alive_callback_data *callback_data = arg;
	struct rrr_mqtt_client_data *data = callback_data->data;

	struct rrr_mqtt_p_pingreq *pingreq = NULL;

	if (connection->protocol_version == NULL) {
		// CONNECT/CONNACK not yet done
		goto out;
	}

	if (connection->keep_alive * 1000 * 1000 + data->last_pingreq_time > time_get_64()) {
		goto out;
	}

	pingreq = (struct rrr_mqtt_p_pingreq *) rrr_mqtt_p_allocate(RRR_MQTT_P_TYPE_PINGREQ, connection->protocol_version);

	RRR_MQTT_COMMON_CALL_SESSION_CHECK_RETURN_TO_CONN_ERRORS_GENERAL(
			data->mqtt_data.sessions->methods->send_packet(
					data->mqtt_data.sessions,
					&connection->session,
					(struct rrr_mqtt_p *) pingreq
			),
			goto out,
			" while sending PINGREQ in __rrr_mqtt_client_exceeded_keep_alive_callback"
	);

	data->last_pingreq_time = time_get_64();

	out:
		RRR_MQTT_P_DECREF_IF_NOT_NULL(pingreq);
		return ret;
}

int rrr_mqtt_client_synchronized_tick (struct rrr_mqtt_client_data *data) {
	int ret = 0;

	struct exceeded_keep_alive_callback_data callback_data = {
			data
	};

	if ((ret = rrr_mqtt_common_read_parse_handle (&data->mqtt_data, __rrr_mqtt_client_exceeded_keep_alive_callback, &callback_data)) != 0) {
		goto out;
	}

	if ((ret = data->mqtt_data.sessions->methods->maintain (
			data->mqtt_data.sessions
	)) != 0) {
		goto out;
	}

	out:
	return ret;
}

int rrr_mqtt_client_iterate_and_clear_local_delivery (
		struct rrr_mqtt_client_data *data,
		int (*callback)(struct rrr_mqtt_p_publish *publish, void *arg),
		void *callback_arg
) {
	return rrr_mqtt_common_iterate_and_clear_local_delivery(
			&data->mqtt_data,
			callback,
			callback_arg
	) & 1; // Clear all errors but internal error
}