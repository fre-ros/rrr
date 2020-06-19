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

#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "net_transport.h"
#include "mqtt_common.h"
#include "mqtt_connection.h"
#include "mqtt_transport.h"
#include "mqtt_session.h"
#include "mqtt_acl.h"

const struct rrr_mqtt_session_properties rrr_mqtt_common_default_session_properties = {
		.session_expiry							= 0,
		.receive_maximum						= 0,
		.maximum_qos							= 0,
		.retain_available						= 1,
		.maximum_packet_size					= 0,
		.assigned_client_identifier				= NULL,
		.reason_string							= NULL,
		.wildcard_subscriptions_available		= 1,
		.subscription_identifiers_availbable	= 1,
		.shared_subscriptions_available			= 1,
		.server_keep_alive						= 30,
		.response_information					= NULL,
		.server_reference						= NULL,

		.topic_alias_maximum					= 0,
		.request_response_information			= 0,
		.request_problem_information			= 0,
		.user_properties						= {0},

		.auth_method							= NULL,
		.auth_data								= NULL
};

void rrr_mqtt_common_data_destroy (struct rrr_mqtt_data *data) {
	if (data == NULL) {
		return;
	}

	if (data->transport != NULL) {
		rrr_mqtt_transport_destroy(data->transport);
		data->transport = NULL;
	}

	if (data->sessions != NULL) {
		data->sessions->methods->destroy(data->sessions);
		data->sessions = NULL;
	}

	RRR_FREE_IF_NOT_NULL(data->client_name);
	data->handler_properties = NULL;
}

/* Call this when a thread has been cancelled to make sure mutexes are unlocked etc. */
void rrr_mqtt_common_data_notify_pthread_cancel (struct rrr_mqtt_data *data) {
	// Nothing to do at the moment
	(void)(data);
}

struct clear_sesion_from_connections_callback_data {
	const struct rrr_mqtt_session *session_to_remove;
	int disregard_transport_handle;
};

static int __rrr_mqtt_broker_clear_session_from_connections_callback (
		struct rrr_net_transport_handle *handle,
		void *arg
) {
	RRR_MQTT_DEFINE_CONN_FROM_HANDLE_AND_CHECK;

	struct clear_sesion_from_connections_callback_data *callback_data = arg;

	int ret = RRR_MQTT_OK;

	if (handle->handle == callback_data->disregard_transport_handle) {
		goto out_nolock;
	}

	if (connection->session == callback_data->session_to_remove) {
		connection->session = NULL;
	}

	out_nolock:
	return ret;
}

/* If an old connection still holds the session while being destroyed after
 * disconnect timer has expired, the session will be destroyed and this new
 * connection will also become disconnected. To avoid this, clear the session from
 * all other connections upon CONNECT. */
int rrr_mqtt_common_clear_session_from_connections (
		struct rrr_mqtt_data *data,
		const struct rrr_mqtt_session *session_to_remove,
		int disregard_transport_handle
) {
	int ret = 0;

	struct clear_sesion_from_connections_callback_data callback_data = {
			session_to_remove,
			disregard_transport_handle
	};

	ret = rrr_mqtt_transport_iterate (
			data->transport,
			RRR_NET_TRANSPORT_SOCKET_MODE_CONNECTION,
			__rrr_mqtt_broker_clear_session_from_connections_callback ,
			&callback_data
	);

	return ret;
}

/*
 * We are called in here from the connection framework on packet events as it
 * is unaware of sessions. We assess here whether something needs to be updated
 * in the sessions or not. The downstream session storage engine
 * is also called as it might have stuff to maintain. Packets which come in
 * and are handled by the broker or client, are NOT passed to the
 * session framework through this function. The packet handlers notify the sessions
 * directly. This goes for PUBLISH, SUBSCRIBE and UNSUBSCRIBE.
 */
static int __rrr_mqtt_common_connection_event_handler (
		struct rrr_mqtt_conn *connection,
		int event,
		void *static_arg,
		void *arg
) {
	struct rrr_mqtt_data *data = static_arg;

	int ret = 0;
	int ret_tmp = 0;

	// session is NULL for instance after parsing CONNECT packet
	if (connection->session == NULL) {
		goto out;
	}

	// Call downstream event handler (broker/client), must be called first in
	// case session-stuff fails due to client counters
	ret_tmp = data->event_handler(connection, event, data->event_handler_static_arg, arg);
	if (ret_tmp != 0) {
		if ((ret_tmp & RRR_MQTT_SOFT_ERROR) != 0) {
			ret |= RRR_MQTT_SOFT_ERROR;
		}
		if ((ret_tmp & RRR_MQTT_SOFT_ERROR) != 0) {
			ret |= RRR_MQTT_SOFT_ERROR;
		}

		if (ret_tmp != 0) {
			RRR_MSG_0("Internal error while calling downstream event handler in __rrr_mqtt_common_connection_event_handler with event %i return was %i\n",
					event, ret_tmp);
			ret |= RRR_MQTT_INTERNAL_ERROR;
			goto out;
		}
	}

	switch (event) {
		case RRR_MQTT_CONN_EVENT_DISCONNECT:
			ret_tmp = MQTT_COMMON_CALL_SESSION_NOTIFY_DISCONNECT(data, connection->session, connection->disconnect_reason_v5_);
			break;
		case RRR_MQTT_CONN_EVENT_PACKET_PARSED:
			ret_tmp = MQTT_COMMON_CALL_SESSION_HEARTBEAT(data, connection->session);
			break;
		default:
			RRR_BUG("Unknown event %i in __rrr_mqtt_common_connection_event_handler\n", event);
	}
	if (ret_tmp != 0) {
		if ((ret_tmp & RRR_MQTT_SESSION_DELETED) != 0) {
			// It is normal to return DELETED from disconnect event
			if (event != RRR_MQTT_CONN_EVENT_DISCONNECT) {
				RRR_MSG_0("Session was deleted while calling session storage engine in __rrr_mqtt_common_connection_event_handler with event %i\n", event);
			}
			ret |= RRR_MQTT_SOFT_ERROR;
		}
		if ((ret_tmp & RRR_MQTT_SESSION_ERROR) != 0) {
			RRR_MSG_0("Session error while calling session storage engine in __rrr_mqtt_common_connection_event_handler with event %i\n", event);
			ret |= RRR_MQTT_SOFT_ERROR;
		}

		ret_tmp = ret_tmp & ~(RRR_MQTT_SESSION_ERROR|RRR_MQTT_SESSION_DELETED);

		if (ret_tmp != 0) {
			RRR_MSG_0("Internal error while calling session storage engine in __rrr_mqtt_common_connection_event_handler with event %i return was %i\n",
					event, ret_tmp);
			ret |= RRR_MQTT_INTERNAL_ERROR;
			goto out;
		}
	}

	out:
	return ret;
}

int rrr_mqtt_common_data_init (
		struct rrr_mqtt_data *data,
		const struct rrr_mqtt_type_handler_properties *handler_properties,
		const struct rrr_mqtt_common_init_data *init_data,
		int (*session_initializer)(struct rrr_mqtt_session_collection **sessions, void *arg),
		void *session_initializer_arg,
		int (*event_handler)(struct rrr_mqtt_conn *connection, int event, void *static_arg, void *arg),
		void *event_handler_static_arg,
		int (*acl_handler)(struct rrr_mqtt_conn *connection, struct rrr_mqtt_p *packet, void *arg),
		void *acl_handler_arg
) {
	int ret = 0;

	memset (data, '\0', sizeof(*data));

	data->client_name = malloc(strlen(init_data->client_name) + 1);
	if (data->client_name == NULL) {
		RRR_MSG_0("Could not allocate memory in rrr_mqtt_data_init\n");
		ret = 1;
		goto out;
	}

	strcpy(data->client_name, init_data->client_name);
	data->event_handler = event_handler;
	data->event_handler_static_arg = event_handler_static_arg;
	data->retry_interval_usec = init_data->retry_interval_usec;
	data->close_wait_time_usec = init_data->close_wait_time_usec;
	data->handler_properties = handler_properties;
	data->acl_handler = acl_handler;
	data->acl_handler_arg = acl_handler_arg;

	if (rrr_mqtt_transport_new (
			&data->transport,
			init_data->max_socket_connections,
			init_data->close_wait_time_usec,
			__rrr_mqtt_common_connection_event_handler,
			data
	) != 0) {
		RRR_MSG_0("Could not initialize connection collection in rrr_mqtt_data_new\n");
		ret = 1;
		goto out;
	}

	if (session_initializer (&data->sessions, session_initializer_arg) != 0) {
		RRR_MSG_0("Could not initialize session data in rrr_mqtt_data_new\n");
		ret = 1;
		goto out_destroy_connections;
	}

	goto out;

	out_destroy_connections:
		rrr_mqtt_transport_destroy(data->transport);
		data->transport = NULL;

	out:
		return ret;
}

#define HANDLE_PROPERTY_CHECK_DUP()																				\
	do {unsigned int dup_count = 0;																				\
	if (	RRR_MQTT_PROPERTY_GET_ID(property) != RRR_MQTT_PROPERTY_USER_PROPERTY &&							\
			RRR_MQTT_PROPERTY_GET_ID(property) != RRR_MQTT_PROPERTY_SUBSCRIPTION_ID &&							\
			(dup_count = rrr_mqtt_property_collection_count_duplicates(callback_data->source, property)) != 0	\
	) {																											\
		RRR_MSG_0("Property '%s' was specified more than once (%u times) in packet\n",							\
				RRR_MQTT_PROPERTY_GET_NAME(property), dup_count + 1);											\
		goto out_reason_protocol_error;																			\
	}} while (0)

#define HANDLE_PROPERTY_SWITCH_BEGIN()										\
	int ret = RRR_MQTT_OK;												\
	HANDLE_PROPERTY_CHECK_DUP();											\
	uint32_t tmp_u32 = 0; (void)(tmp_u32);									\
	do { switch (RRR_MQTT_PROPERTY_GET_ID(property)) {						\
		case 0:																\
			RRR_BUG("Property id was 0 in HANDLE_PROPERTY_SWITCH_BEGIN\n");	\
			break

#define HANDLE_PROPERTY_U32_UNCHECKED(target,id) 							\
		case id:															\
			(target) = rrr_mqtt_property_get_uint32(property);				\
			break

#define HANDLE_PROPERTY_U32_NON_ZERO(target,id,error_msg)					\
		case id:															\
			tmp_u32 = rrr_mqtt_property_get_uint32(property);				\
			if (tmp_u32 == 0) {												\
				RRR_MSG_0(error_msg "\n");									\
				goto out_reason_protocol_error;								\
			}																\
			(target) = tmp_u32;												\
			break

#define HANDLE_PROPERTY_U32_QOS(target,id,error_msg)						\
		case id:															\
			tmp_u32 = rrr_mqtt_property_get_uint32(property);				\
			if (tmp_u32 > 2) {												\
				RRR_MSG_0(error_msg "\n");									\
				goto out_reason_protocol_error;								\
			}																\
			(target) = tmp_u32;												\
			break

#define HANDLE_PROPERTY_U32_ON_OFF_TO_U8(target,id,error_msg)				\
		case id:															\
			tmp_u32 = rrr_mqtt_property_get_uint32(property);				\
			if (tmp_u32 > 1) {												\
				RRR_MSG_0(error_msg "\n");									\
				goto out_reason_protocol_error;								\
			}																\
			(target) = tmp_u32;												\
			break

#define HANDLE_PROPERTY_U32_TO_U8(target,id)								\
		case id:															\
			tmp_u32 = rrr_mqtt_property_get_uint32(property);				\
			if (tmp_u32 > 0xff) {											\
				RRR_BUG("U8 property overflow in HANDLE_PROPERTY_U32_TO_U8\n");\
			}																\
			(target) = tmp_u32;												\
			break

#define HANDLE_PROPERTY_U32_TO_U16(target,id)								\
		case id:															\
			tmp_u32 = rrr_mqtt_property_get_uint32(property);				\
			if (tmp_u32 > 0xffff) {											\
				RRR_BUG("U16 property overflow in HANDLE_PROPERTY_U32_TO_U8\n");\
			}																\
			(target) = tmp_u32;												\
			break

#define HANDLE_PROPERTY_TO_COLLECTION(target,id)													\
		case id:																					\
			ret = rrr_mqtt_property_collection_add_cloned((target), property);						\
			if (ret != 0) {																			\
				RRR_MSG_0("Error while cloning property in HANDLE_PROPERTY_TO_COLLECTION\n");		\
				goto out_internal_error;															\
			}																						\
			break

#define HANDLE_PROPERTY_TO_COLLECTION_NON_ZERO(target,id,error_msg)									\
		case id:																					\
			tmp_u32 = rrr_mqtt_property_get_uint32(property);										\
			if (tmp_u32 == 0) {																		\
				RRR_MSG_0(error_msg "\n");															\
				goto out_reason_protocol_error;														\
			}																						\
			ret = rrr_mqtt_property_collection_add_cloned((target), property);						\
			if (ret != 0) {																			\
				RRR_MSG_0("Error while cloning property in HANDLE_PROPERTY_TO_COLLECTION\n");		\
				goto out_internal_error;															\
			}																						\
			break

#define HANDLE_PROPERTY_CLONE(target,id)															\
		case id:																					\
			if (rrr_mqtt_property_clone((target), property) != 0) {									\
				RRR_MSG_0("Could not clone property HANDLE_PROPERTY_USER_PROPERTY\n");				\
				goto out_internal_error;															\
			}																						\
			break;

#define HANDLE_PROPERTY_COPY_POINTER_DANGEROUS(target,id)											\
		case id:																					\
			(target) = property;																	\
			break;

// We do not return error as we want to parse the rest of the source_properties to check
// for more errors. Caller checks for non-zero reason.
#define HANDLE_PROPERTY_SWITCH_END_AND_RETURN() 													\
		default:																					\
			RRR_MSG_0("Unknown property '%s'\n", RRR_MQTT_PROPERTY_GET_NAME(property));			\
			goto out_reason_protocol_error;															\
	};																								\
	goto out;																						\
	out_internal_error:																				\
		ret = RRR_MQTT_INTERNAL_ERROR;															\
		return ret;																					\
	out_reason_protocol_error:																		\
		ret = RRR_MQTT_SOFT_ERROR;																\
		callback_data->reason_v5 = RRR_MQTT_P_5_REASON_PROTOCOL_ERROR;								\
	out:																							\
		return ret;																					\
	} while (0)

int rrr_mqtt_common_handler_connect_handle_properties_callback (
		const struct rrr_mqtt_property *property,
		void *arg
) {
	struct rrr_mqtt_common_parse_properties_data_connect *callback_data = arg;
	struct rrr_mqtt_session_properties *session_properties = callback_data->session_properties;

	HANDLE_PROPERTY_SWITCH_BEGIN();
		HANDLE_PROPERTY_U32_UNCHECKED (
				session_properties->session_expiry,
				RRR_MQTT_PROPERTY_SESSION_EXPIRY_INTERVAL
		);
		HANDLE_PROPERTY_U32_NON_ZERO (
				session_properties->receive_maximum,
				RRR_MQTT_PROPERTY_RECEIVE_MAXIMUM,
				"Receive maximum was 0 in CONNECT packet"
		);
		HANDLE_PROPERTY_U32_NON_ZERO (
				session_properties->maximum_packet_size,
				RRR_MQTT_PROPERTY_MAXIMUM_PACKET_SIZE,
				"Maximum packet size was 0 in CONNECT packet"
		);
		HANDLE_PROPERTY_U32_UNCHECKED (
				session_properties->topic_alias_maximum,
				RRR_MQTT_PROPERTY_TOPIC_ALIAS_MAXIMUM
		);
		HANDLE_PROPERTY_U32_ON_OFF_TO_U8 (
				session_properties->request_response_information,
				RRR_MQTT_PROPERTY_REQUEST_RESPONSE_INFO,
				"Request response information field in CONNECT packet was not 0 or 1"
		);
		HANDLE_PROPERTY_U32_ON_OFF_TO_U8 (
				session_properties->request_problem_information,
				RRR_MQTT_PROPERTY_REQUEST_PROBLEM_INFO,
				"Request problem information field in CONNECT packet was not 0 or 1"
		);
		HANDLE_PROPERTY_TO_COLLECTION (
				&session_properties->user_properties,
				RRR_MQTT_PROPERTY_USER_PROPERTY
		);
		HANDLE_PROPERTY_CLONE (
				&session_properties->auth_method,
				RRR_MQTT_PROPERTY_AUTH_METHOD
		);
		HANDLE_PROPERTY_CLONE (
				&session_properties->auth_data,
				RRR_MQTT_PROPERTY_AUTH_DATA
		);
	HANDLE_PROPERTY_SWITCH_END_AND_RETURN();
}

int rrr_mqtt_common_handler_connack_handle_properties_callback (
		const struct rrr_mqtt_property *property,
		void *arg
) {
	struct rrr_mqtt_common_parse_properties_data_connect *callback_data = arg;
	struct rrr_mqtt_session_properties *session_properties = callback_data->session_properties;

	HANDLE_PROPERTY_SWITCH_BEGIN();
		HANDLE_PROPERTY_U32_UNCHECKED (
				session_properties->session_expiry,
				RRR_MQTT_PROPERTY_SESSION_EXPIRY_INTERVAL
		);
		HANDLE_PROPERTY_U32_NON_ZERO (
				session_properties->receive_maximum,
				RRR_MQTT_PROPERTY_RECEIVE_MAXIMUM,
				"Receive maximum was 0 in CONNACK packet"
		);
		HANDLE_PROPERTY_U32_QOS (
				session_properties->maximum_qos,
				RRR_MQTT_PROPERTY_MAXIMUM_QOS,
				"QOS was not 0, 1 or 2 in CONNACK packet"
		);
		HANDLE_PROPERTY_U32_ON_OFF_TO_U8 (
				session_properties->retain_available,
				RRR_MQTT_PROPERTY_RETAIN_AVAILABLE,
				"Retain available field in CONNECT packet was not 0 or 1"
		);
		HANDLE_PROPERTY_U32_NON_ZERO (
				session_properties->maximum_packet_size,
				RRR_MQTT_PROPERTY_MAXIMUM_PACKET_SIZE,
				"Maximum packet size was 0 in CONNECT packet"
		);
		HANDLE_PROPERTY_CLONE (
				&session_properties->assigned_client_identifier,
				RRR_MQTT_PROPERTY_ASSIGNED_CLIENT_ID
		);
		HANDLE_PROPERTY_CLONE (
				&session_properties->reason_string,
				RRR_MQTT_PROPERTY_REASON_STRING
		);
		HANDLE_PROPERTY_TO_COLLECTION (
				&session_properties->user_properties,
				RRR_MQTT_PROPERTY_USER_PROPERTY
		);
		HANDLE_PROPERTY_U32_ON_OFF_TO_U8 (
				session_properties->wildcard_subscriptions_available,
				RRR_MQTT_PROPERTY_WILDCARD_SUB_AVAILBABLE,
				"Wildcard subscriptions available field in CONNECT packet was not 0 or 1"
		);
		HANDLE_PROPERTY_U32_ON_OFF_TO_U8 (
				session_properties->subscription_identifiers_availbable,
				RRR_MQTT_PROPERTY_SUBSCRIPTION_ID_AVAILABLE,
				"Subscription identifiers available field in CONNECT packet was not 0 or 1"
		);
		HANDLE_PROPERTY_U32_ON_OFF_TO_U8 (
				session_properties->shared_subscriptions_available,
				RRR_MQTT_PROPERTY_SHARED_SUB_AVAILABLE,
				"Shared subscriptions available field in CONNECT packet was not 0 or 1"
		);
		HANDLE_PROPERTY_U32_UNCHECKED (
				session_properties->server_keep_alive,
				RRR_MQTT_PROPERTY_SERVER_KEEP_ALIVE
		);
		HANDLE_PROPERTY_CLONE (
				&session_properties->response_information,
				RRR_MQTT_PROPERTY_RESPONSE_INFO
		);
		HANDLE_PROPERTY_CLONE (
				&session_properties->server_reference,
				RRR_MQTT_PROPERTY_SERVER_REFERENCE
		);
		HANDLE_PROPERTY_CLONE (
				&session_properties->auth_method,
				RRR_MQTT_PROPERTY_AUTH_METHOD
		);
		HANDLE_PROPERTY_CLONE (
				&session_properties->auth_data,
				RRR_MQTT_PROPERTY_AUTH_DATA
		);
	HANDLE_PROPERTY_SWITCH_END_AND_RETURN();
}

int rrr_mqtt_common_handler_publish_handle_properties_callback (
		const struct rrr_mqtt_property *property,
		void *arg
) {
	struct rrr_mqtt_common_parse_properties_data_publish *callback_data = arg;
	struct rrr_mqtt_p_publish *publish = callback_data->publish;

	HANDLE_PROPERTY_SWITCH_BEGIN();
		HANDLE_PROPERTY_U32_ON_OFF_TO_U8 (
				publish->payload_format_indicator,
				RRR_MQTT_PROPERTY_PAYLOAD_FORMAT_INDICATOR,
				"Payload format indicator field in PUBLISH packet was not 0 or 1"
		);
		HANDLE_PROPERTY_U32_UNCHECKED (
				publish->message_expiry_interval,
				RRR_MQTT_PROPERTY_MESSAGE_EXPIRY_INTERVAL
		);
		HANDLE_PROPERTY_U32_TO_U16 (
				publish->topic_alias,
				RRR_MQTT_PROPERTY_TOPIC_ALIAS
		);
		HANDLE_PROPERTY_COPY_POINTER_DANGEROUS (
				publish->response_topic,
				RRR_MQTT_PROPERTY_RESPONSE_TOPIC
		);
		HANDLE_PROPERTY_COPY_POINTER_DANGEROUS (
				publish->correlation_data,
				RRR_MQTT_PROPERTY_CORRELATION_DATA
		);
		HANDLE_PROPERTY_TO_COLLECTION (
				&publish->user_properties,
				RRR_MQTT_PROPERTY_USER_PROPERTY
		);
		HANDLE_PROPERTY_TO_COLLECTION_NON_ZERO (
				&publish->subscription_ids,
				RRR_MQTT_PROPERTY_SUBSCRIPTION_ID,
				"Subscription id was zero in PUBLISH properties"
		);
		HANDLE_PROPERTY_COPY_POINTER_DANGEROUS (
				publish->content_type,
				RRR_MQTT_PROPERTY_CONTENT_TYPE
		);
	HANDLE_PROPERTY_SWITCH_END_AND_RETURN();
}

int rrr_mqtt_common_handle_properties (
		const struct rrr_mqtt_property_collection *source,
		int (*callback)(const struct rrr_mqtt_property *property, void *arg),
		struct rrr_mqtt_common_parse_properties_data *callback_data,
		uint8_t *reason_v5
) {
	int ret = RRR_MQTT_OK;

	*reason_v5 = RRR_MQTT_P_5_REASON_OK;

	if ((ret = rrr_mqtt_property_collection_iterate (
		source,
		callback,
		callback_data
	)) != 0 || callback_data->reason_v5 != RRR_MQTT_P_5_REASON_OK) {
		if ((ret & RRR_MQTT_SOFT_ERROR) != 0) {
			RRR_MSG_0("Soft error while iterating properties\n");
			ret = ret & ~(RRR_MQTT_SOFT_ERROR);
		}
		if (ret != 0) {
			ret = RRR_MQTT_INTERNAL_ERROR;
			RRR_MSG_0("Internal error while iterating properties, return was %i\n", ret);
			goto out;
		}

		if (callback_data->reason_v5 == RRR_MQTT_P_5_REASON_OK) {
			RRR_BUG("Callback return error in __rrr_mqtt_p_handle_propertie returned but no reason was set\n");
		}

		ret = RRR_MQTT_SOFT_ERROR;
		*reason_v5 = callback_data->reason_v5;
	}

	out:
	return ret;
}

int rrr_mqtt_common_handle_publish (RRR_MQTT_TYPE_HANDLER_DEFINITION) {
	RRR_MQTT_DEFINE_CONN_FROM_HANDLE_AND_CHECK;

	int ret = RRR_MQTT_OK;

	struct rrr_mqtt_p_publish *publish = (struct rrr_mqtt_p_publish *) packet;
	struct rrr_mqtt_p *ack = NULL;
	uint8_t reason_v5 = 0;

	// If we send an ACK without giving the PUBLISH to session framework first, this
	// must be set to 1
	int allow_missing_originating_packet = 0;

	if (publish->reason_v5 != RRR_MQTT_P_5_REASON_OK) {
		allow_missing_originating_packet = 1;

		if (publish->qos == 0) {
			RRR_MSG_0("Closing connection due to malformed PUBLISH packet with QoS 0\n");
			ret = RRR_MQTT_SOFT_ERROR;
			goto out;
		}

		RRR_MSG_0("Sending ACK for malformed PUBLISH packet with QoS %u, reason was %u\n",
				publish->qos, publish->reason_v5);

		reason_v5 = publish->reason_v5;
		goto out_generate_ack;
	}

	int acl_result = mqtt_data->acl_handler(connection, packet, mqtt_data->acl_handler_arg);
	switch (acl_result) {
		case RRR_MQTT_ACL_RESULT_ALLOW:
			RRR_MSG_2 ("PUBLISH topic '%s' ALLOWED\n", publish->topic);
			reason_v5 = RRR_MQTT_P_5_REASON_OK;
			break;
		case RRR_MQTT_ACL_RESULT_DISCONNECT:
			RRR_MSG_2 ("PUBLISH topic '%s' DENIED AND DISCONNECTING\n", publish->topic);
			ret = RRR_MQTT_SOFT_ERROR;
			goto out;
		case RRR_MQTT_ACL_RESULT_DENY:
			RRR_MSG_2 ("PUBLISH topic '%s' DENIED\n", publish->topic);
			reason_v5 = RRR_MQTT_P_5_REASON_NOT_AUTHORIZED;
			break;
		default:
			RRR_MSG_0("Warning: Error while checking ACL in rrr_mqtt_common_handle_publish, dropping packet and closing connection\n");
			ret = RRR_MQTT_SOFT_ERROR;
			goto out;
	};
	if (reason_v5 != RRR_MQTT_P_5_REASON_OK) {
		allow_missing_originating_packet = 1;
		goto out_generate_ack;
	}

	struct rrr_mqtt_common_parse_properties_data_publish callback_data = {
			&publish->properties,
			0,
			publish
	};

	RRR_MQTT_COMMON_HANDLE_PROPERTIES (
			&publish->properties,
			publish,
			rrr_mqtt_common_handler_publish_handle_properties_callback,
			goto out_generate_ack
	);

//	RRR_MQTT_P_UNLOCK(packet);
	RRR_MQTT_P_INCREF(packet);
	unsigned int dummy;
	int ret_from_receive_publish = mqtt_data->sessions->methods->receive_packet(
			mqtt_data->sessions,
			&connection->session,
			(struct rrr_mqtt_p *) publish,
			&dummy
	);
	RRR_MQTT_P_DECREF(packet);
//	RRR_MQTT_P_LOCK(packet);

	RRR_MQTT_COMMON_CALL_SESSION_CHECK_RETURN_TO_CONN_ERRORS_GENERAL(
			ret_from_receive_publish,
			goto out,
			" in session receive publish function in rrr_mqtt_common_handle_publish"
	);

	out_generate_ack:
	if (publish->qos == 1) {
		struct rrr_mqtt_p_puback *puback = (struct rrr_mqtt_p_puback *) rrr_mqtt_p_allocate (
						RRR_MQTT_P_TYPE_PUBACK, publish->protocol_version
		);
		ack = (struct rrr_mqtt_p *) puback;
		if (puback == NULL) {
			RRR_MSG_0("Could not allocate PUBACK in __rrr_mqtt_broker_handle_publish\n");
			ret = RRR_MQTT_INTERNAL_ERROR;
			goto out;
		}

		RRR_MQTT_P_LOCK(puback);
		puback->reason_v5 = reason_v5;
		puback->packet_identifier = publish->packet_identifier;
		RRR_MQTT_P_UNLOCK(puback);
	}
	else if (publish->qos == 2) {
		struct rrr_mqtt_p_pubrec *pubrec = (struct rrr_mqtt_p_pubrec *) rrr_mqtt_p_allocate (
						RRR_MQTT_P_TYPE_PUBREC, publish->protocol_version
		);
		ack = (struct rrr_mqtt_p *) pubrec;
		if (pubrec == NULL) {
			RRR_MSG_0("Could not allocate PUBREC in __rrr_mqtt_broker_handle_publish\n");
			ret = RRR_MQTT_INTERNAL_ERROR;
			goto out;
		}

		RRR_MQTT_P_LOCK(pubrec);
		pubrec->reason_v5 = reason_v5;
		pubrec->packet_identifier = publish->packet_identifier;
		RRR_MQTT_P_UNLOCK(pubrec);
	}
	else if (publish->qos != 0) {
		RRR_BUG("Invalid QoS (%u) in rrr_mqtt_common_handle_publish\n", publish->qos);
	}

	if (ack != NULL) {
		// NOTE : Connection subsystem will notify session system when ACK is successfully
		//        sent. We also need to unlock the packet because the original publish needs
		//        to be locked when the ACK notification is processed.
		RRR_MQTT_P_UNLOCK(packet);

		RRR_MQTT_COMMON_CALL_SESSION_CHECK_RETURN_TO_CONN_ERRORS_GENERAL(
			mqtt_data->sessions->methods->send_packet(
					mqtt_data->sessions,
					&connection->session,
					ack,
					allow_missing_originating_packet
			),
			goto out_relock,
			" in session send packet function in rrr_mqtt_common_handle_publish"
		);

		RRR_MQTT_P_LOCK(packet);
	}

	goto out;
	out_relock:
		RRR_MQTT_P_LOCK(packet);
	out:
		RRR_MQTT_P_DECREF_IF_NOT_NULL(ack);
		return ret;
}

static int __rrr_mqtt_common_handle_general_ack (
		unsigned int *match_count,
		uint8_t *reason_v5,
		RRR_MQTT_TYPE_HANDLER_DEFINITION
) {
	RRR_MQTT_DEFINE_CONN_FROM_HANDLE_AND_CHECK;

	int ret = RRR_MQTT_OK;

	*reason_v5 = RRR_MQTT_P_5_REASON_OK;

	int ret_from_session = mqtt_data->sessions->methods->receive_packet (
			mqtt_data->sessions,
			&connection->session,
			packet,
			match_count
	);

	// It is possible to receive PUBREC and PUBACK with unknown packet IDs (remains from
	// older QoS handshake which only remote knows about). If the ID used happens to be
	// available, we can continue. If not, it is a session error.

	if (*match_count != 1) {
		*reason_v5 = RRR_MQTT_P_5_REASON_PACKET_IDENTIFIER_NOT_FOUND;
	}

	RRR_MQTT_COMMON_CALL_SESSION_CHECK_RETURN_TO_CONN_ERRORS_GENERAL(
			ret_from_session,
			goto out,
			" while handling packet"
	);

	out:
	return ret;
}

int rrr_mqtt_common_handle_puback_pubcomp (RRR_MQTT_TYPE_HANDLER_DEFINITION) {
	RRR_MQTT_DEFINE_CONN_FROM_HANDLE_AND_CHECK;

	int ret = RRR_MQTT_OK;

	unsigned int match_count = 0;
	uint8_t reason_v5 = 0;
	ret = __rrr_mqtt_common_handle_general_ack (
			&match_count,
			&reason_v5,
			mqtt_data,
			handle,
			packet
	);

	if (ret != 0) {
		if (ret == RRR_MQTT_INTERNAL_ERROR) {
			goto out;
		}
		if (reason_v5 == RRR_MQTT_P_5_REASON_OK) {
			RRR_DBG_1("Setting disconnect reason to 0x80 in rrr_mqtt_common_handle_puback_pubcomp\n");
			reason_v5 = RRR_MQTT_P_5_REASON_UNSPECIFIED_ERROR;
		}
		RRR_MSG_0("Error while handling received %s packet, reason: %u\n",
				RRR_MQTT_P_GET_TYPE_NAME(packet), reason_v5);
		ret = RRR_MQTT_SOFT_ERROR;
		goto out;
	}

	if (match_count != 1) {
		RRR_DBG_1("No match for ACK of type %s id %u, possibly old packet\n",
				RRR_MQTT_P_GET_TYPE_NAME(packet), RRR_MQTT_P_GET_IDENTIFIER(packet));
	}

	out:
	return ret;
}

// See explanation of operation in mqtt_session.h
static int __rrr_mqtt_common_handle_pubrec_pubrel (
		RRR_MQTT_TYPE_HANDLER_DEFINITION,
		struct rrr_mqtt_conn *connection,
		uint8_t next_ack_type
) {
	int ret = RRR_MQTT_OK;

	struct rrr_mqtt_p *next_ack = NULL;

	uint8_t reason_v5;
	unsigned int match_count = 0;

	ret = __rrr_mqtt_common_handle_general_ack (
			&match_count,
			&reason_v5,
			mqtt_data,
			handle,
			packet
	);

	if (ret != 0) {
		if (ret == RRR_MQTT_INTERNAL_ERROR) {
			goto out;
		}
		if (reason_v5 == RRR_MQTT_P_5_REASON_OK) {
			RRR_DBG_1("Setting disconnect reason to 0x80 in rrr_mqtt_common_handle_pubrec_pubrel\n");
			reason_v5 = RRR_MQTT_P_5_REASON_UNSPECIFIED_ERROR;
		}

		// For version 5, send a response with the error specified. For version 3.1,
		// we must close the connection.
		if (RRR_MQTT_P_IS_V5(packet)) {
			ret = RRR_MQTT_OK;
			goto out_send_ack;
		}
		goto out;
	}

	// TODO : Check if it's OK just to continue a QoS2 handshake which we did not know about
/*	if (match_count != 1) {
		VL_BUG("match_count was not 1 in __rrr_mqtt_broker_handle_pubrec_pubrel, session system should have triggered an error\n");
	}*/

	out_send_ack:
	next_ack = rrr_mqtt_p_allocate (
			next_ack_type,
			packet->protocol_version
	);
	if (next_ack == NULL) {
		RRR_MSG_0("Could not allocate %s in __rrr_mqtt_broker_handle_pubrec_pubrel\n",
				RRR_MQTT_P_GET_TYPE_NAME_RAW(next_ack_type));
		ret = RRR_MQTT_INTERNAL_ERROR;
		goto out;
	}

	RRR_MQTT_P_LOCK(next_ack);

	next_ack->reason_v5 = reason_v5;
	next_ack->packet_identifier = packet->packet_identifier;

	RRR_MQTT_P_UNLOCK(next_ack);

	RRR_MQTT_COMMON_CALL_SESSION_CHECK_RETURN_TO_CONN_ERRORS_GENERAL(
			mqtt_data->sessions->methods->send_packet(mqtt_data->sessions, &connection->session, next_ack, 0),
			goto out,
			" while sending ACK for packet to session in __rrr_mqtt_broker_handle_pubrec_pubrel"
	);

	out:
		RRR_MQTT_P_DECREF_IF_NOT_NULL(next_ack);
	return ret;
}

int rrr_mqtt_common_handle_pubrec (RRR_MQTT_TYPE_HANDLER_DEFINITION) {
	RRR_MQTT_DEFINE_CONN_FROM_HANDLE_AND_CHECK;

	return __rrr_mqtt_common_handle_pubrec_pubrel (
			mqtt_data,
			handle,
			packet,
			connection,
			RRR_MQTT_P_TYPE_PUBREL
	);
}

int rrr_mqtt_common_handle_pubrel (RRR_MQTT_TYPE_HANDLER_DEFINITION) {
	RRR_MQTT_DEFINE_CONN_FROM_HANDLE_AND_CHECK;

	return __rrr_mqtt_common_handle_pubrec_pubrel (
			mqtt_data,
			handle,
			packet,
			connection,
			RRR_MQTT_P_TYPE_PUBCOMP
	);
}

int rrr_mqtt_common_handle_disconnect (RRR_MQTT_TYPE_HANDLER_DEFINITION) {
	RRR_MQTT_DEFINE_CONN_FROM_HANDLE_AND_CHECK;

	int ret = 0;

	(void)(mqtt_data);

	if ((ret = rrr_mqtt_conn_update_state (
			connection,
			packet,
			RRR_MQTT_CONN_UPDATE_STATE_DIRECTION_IN
	)) != RRR_MQTT_OK) {
		RRR_MSG_0("Could not update connection state in rrr_mqtt_p_handler_disconnect\n");
		goto out;
	}

	out:
	return ret;
}

static int __rrr_mqtt_common_handle_packet_callback (
		struct rrr_net_transport_handle *handle,
		struct rrr_mqtt_p *packet,
		void *arg
) {
	RRR_MQTT_DEFINE_CONN_FROM_HANDLE_AND_CHECK;

	int ret = 0;

	struct rrr_mqtt_data *mqtt_data = arg;

	RRR_MQTT_P_LOCK(packet);

	if (RRR_MQTT_P_GET_TYPE(packet) == RRR_MQTT_P_TYPE_CONNECT) {
		if (!RRR_MQTT_CONN_STATE_RECEIVE_CONNECT_IS_ALLOWED(connection)) {
			RRR_MSG_0("Received a CONNECT packet while not allowed in __rrr_mqtt_common_handle_packets_callback\n");
			ret |= RRR_MQTT_SOFT_ERROR;
			goto out;
		}
	}
	else if (RRR_MQTT_P_GET_TYPE(packet) == RRR_MQTT_P_TYPE_CONNACK) {
		if (!RRR_MQTT_CONN_STATE_RECEIVE_CONNACK_IS_ALLOWED(connection)) {
			RRR_MSG_0("Received a CONNACK packet while not allowed in __rrr_mqtt_common_handle_packets_callback\n");
			ret |= RRR_MQTT_SOFT_ERROR;
			goto out;
		}
	}
	else if (!RRR_MQTT_CONN_STATE_RECEIVE_ANY_IS_ALLOWED(connection)) {
		RRR_MSG_0("Received a %s packet while only CONNECT was allowed in __rrr_mqtt_common_handle_packets_callback\n",
				RRR_MQTT_P_GET_TYPE_NAME(packet));
		ret |= RRR_MQTT_SOFT_ERROR;
		goto out;
	}

	if (mqtt_data->handler_properties[RRR_MQTT_P_GET_TYPE(packet)].handler == NULL) {
		RRR_MSG_0("No handler specified for packet type %i\n", RRR_MQTT_P_GET_TYPE(packet));
		ret |= RRR_MQTT_SOFT_ERROR;
		goto out;
	}

	RRR_DBG_3 ("Handling packet of type %s id %u dup %u\n",
			RRR_MQTT_P_GET_TYPE_NAME(packet), RRR_MQTT_P_GET_IDENTIFIER(packet), packet->dup);

	if ((ret = mqtt_data->handler_properties[RRR_MQTT_P_GET_TYPE(packet)].handler(mqtt_data, handle, packet)) != 0) {
		if (ret == RRR_MQTT_INTERNAL_ERROR) {
			RRR_MSG_0("Error while handing packet in __rrr_mqtt_common_handle_packets_callback\n");
		}
		goto out;
	}

	out:
		RRR_MQTT_P_UNLOCK(packet);
		return ret;
}

static int __rrr_mqtt_common_read_parse_handle (
		struct rrr_net_transport_handle *handle,
		struct rrr_mqtt_data *data
) {
	int ret = RRR_MQTT_OK;

	// TODO : Make this better
	// Do this 60 times as we send 50 packets at a time (10 more)
	for (int i = 0; i < 60; i++) {
		if ((ret = rrr_mqtt_conn_iterator_ctx_read (
				handle,
				RRR_MQTT_SYNCHRONIZED_READ_STEP_MAX_SIZE,
				__rrr_mqtt_common_handle_packet_callback,
				data
		)) != 0) {
			if (ret == RRR_MQTT_INTERNAL_ERROR) {
				RRR_MSG_0("Error while reading data from remote in __rrr_mqtt_common_read_parse_handle\n");
			}
			goto out;
		}
	}

	out:
	return ret;
}

int rrr_mqtt_common_send_from_sessions_callback (
		struct rrr_mqtt_p *packet,
		void *arg
) {
	// context is FIFO-buffer
	int ret = RRR_FIFO_OK;

	struct rrr_mqtt_send_from_sessions_callback_data *callback_data = arg;

	if (RRR_MQTT_P_TRYLOCK(packet) == 0) {
		RRR_BUG("BUG: Packet %p was not locked in rrr_mqtt_common_send_from_sessions_callback\n", packet);
	}

	if (rrr_mqtt_conn_iterator_ctx_send_packet(callback_data->handle, packet) != 0) {
		RRR_MSG_0("Could not send outbound packet in __rrr_mqtt_common_send_from_sessions_callback\n");
		// Do not delete packet on error, retry with new connection if client reconnects.
		ret = RRR_FIFO_CALLBACK_ERR | RRR_FIFO_SEARCH_STOP;
	}

	return ret;
}

static int __rrr_mqtt_common_send (
		struct rrr_net_transport_handle *handle,
		struct rrr_mqtt_data *data
) {
	int ret = RRR_MQTT_OK;

	RRR_MQTT_DEFINE_CONN_FROM_HANDLE_AND_CHECK;

	if (connection->session == NULL) {
		// No CONNECT yet
		goto out;
	}

	struct rrr_mqtt_send_from_sessions_callback_data callback_data = {
			handle
	};
	RRR_MQTT_COMMON_CALL_SESSION_CHECK_RETURN_TO_CONN_ERRORS_GENERAL(
			data->sessions->methods->iterate_send_queue (
					data->sessions,
					&connection->session,
					rrr_mqtt_common_send_from_sessions_callback,
					&callback_data,
					50	// <-- max count
			),
			goto out,
			"while iterating session send queue"
	);

	out:
		return ret;
}

struct rrr_mqtt_common_read_parse_handle_callback_data {
	struct rrr_mqtt_data *data;
	struct rrr_mqtt_conn_iterator_ctx_housekeeping_callback_data housekeeping_data;
};

static int __rrr_mqtt_common_read_parse_handle_callback (
		struct rrr_net_transport_handle *handle,
		void *arg
) {
	int ret = RRR_MQTT_OK;

	RRR_MQTT_DEFINE_CONN_FROM_HANDLE_AND_CHECK;

	struct rrr_mqtt_common_read_parse_handle_callback_data *callback_data = arg;

	if ((ret = __rrr_mqtt_common_read_parse_handle(handle, callback_data->data)) != 0 && (ret != RRR_MQTT_INCOMPLETE)) {
		if ((ret & RRR_MQTT_INTERNAL_ERROR) == RRR_MQTT_INTERNAL_ERROR) {
			RRR_MSG_0("Internal error in __rrr_mqtt_common_read_parse_handle_callback while reading and parsing\n");
			ret = RRR_MQTT_INTERNAL_ERROR;
			goto out;
		}
		ret = RRR_MQTT_SOFT_ERROR;
		goto housekeeping;
	}

	if ((ret = __rrr_mqtt_common_send(handle, callback_data->data)) != 0 && (ret != RRR_MQTT_INCOMPLETE)) {
		if ((ret & RRR_MQTT_INTERNAL_ERROR) == RRR_MQTT_INTERNAL_ERROR) {
			RRR_MSG_0("Internal error in __rrr_mqtt_common_read_parse_handle_callback while sending\n");
			ret = RRR_MQTT_INTERNAL_ERROR;
			goto out;
		}
		ret = RRR_MQTT_SOFT_ERROR;
		goto housekeeping;
	}

	housekeeping:

	if ((ret = rrr_mqtt_conn_housekeeping(connection, &callback_data->housekeeping_data)) != 0) {
		if ((ret & RRR_MQTT_INTERNAL_ERROR) == RRR_MQTT_INTERNAL_ERROR) {
			RRR_MSG_0("Internal error in __rrr_mqtt_common_read_parse_handle_callback while housekeeping\n");
			ret = RRR_MQTT_INTERNAL_ERROR;
			goto out;
		}
		ret = RRR_MQTT_SOFT_ERROR;
		goto out;
	}

	out:
	return ret;
}

int rrr_mqtt_common_read_parse_handle (
		struct rrr_mqtt_data *data,
		int (*exceeded_keep_alive_callback)(struct rrr_mqtt_conn *connection, void *arg),
		void *callback_arg
) {
	int ret = 0;

	struct rrr_mqtt_common_read_parse_handle_callback_data callback_data = {
			data,
			{ exceeded_keep_alive_callback, callback_arg }
	};

	ret |= rrr_mqtt_transport_iterate (
			data->transport,
			RRR_NET_TRANSPORT_SOCKET_MODE_CONNECTION,
			__rrr_mqtt_common_read_parse_handle_callback,
			&callback_data
	);

	// Only let internal error propagate
	return ret & RRR_MQTT_INTERNAL_ERROR;
}

int rrr_mqtt_common_iterate_and_clear_local_delivery (
		struct rrr_mqtt_data *data,
		int (*callback)(struct rrr_mqtt_p_publish *publish, void *arg),
		void *callback_arg
) {
	int ret = 0;

	RRR_MQTT_COMMON_CALL_SESSION_AND_CHECK_RETURN_GENERAL(
			data->sessions->methods->iterate_and_clear_local_delivery(data->sessions, callback, callback_arg),
			goto out,
			" while iterating local delivery queue in rrr_mqtt_common_iterate_and_clear_local_delivery"
	);

	out:
	return ret & RRR_MQTT_SESSION_INTERNAL_ERROR;
}
