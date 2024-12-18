/*

Read Route Record

Copyright (C) 2019-2024 Atle Solbakken atle@goliathdns.no

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

#ifndef RRR_INSTANCES_H
#define RRR_INSTANCES_H

#include "modules.h"
#include "instance_friends.h"
#include "discern_stack.h"
#include "threads.h"
#include "poll_helper.h"
#include "event/event.h"
#include "event/event_collection_struct.h"
#include "util/linked_list.h"

#define RRR_INSTANCE_MISC_OPTIONS_DISABLE_BUFFER           (1<<0)
#define RRR_INSTANCE_MISC_OPTIONS_DISABLE_BACKSTOP         (1<<1)
#define RRR_INSTANCE_MISC_OPTIONS_DUPLICATE                (1<<2)
#define RRR_INSTANCE_MISC_OPTIONS_TOPIC_FILTER_INVERT      (1<<3)
#define RRR_INSTANCE_MISC_OPTIONS_METHODS_DIRECT_DISPATCH  (1<<4)
#define RRR_INSTANCE_MISC_OPTIONS_METHODS_DOUBLE_DELIVERY  (1<<5)

struct rrr_stats_instance;
struct rrr_cmodule;
struct rrr_fork_handler;
struct rrr_event_queue;
struct rrr_stats_engine;
struct rrr_message_broker;
struct rrr_mqtt_topic_token;
struct rrr_instance_config_collection;

struct rrr_instance {
	RRR_LL_NODE(struct rrr_instance);
	// Managed by this struct
	struct rrr_instance_module_data *module_data;
	struct rrr_instance_friend_collection senders;
	struct rrr_instance_friend_collection wait_for;
	struct rrr_discern_stack_collection routes;
	struct rrr_discern_stack_collection methods;
	struct rrr_signal_handler *signal_handler;
	char *topic_filter;
	struct rrr_mqtt_topic_token *topic_first_token;

	// Static members
	unsigned long int senders_count;
	int misc_flags;

	// Shortcuts
	struct rrr_instance_config_data *config;
	struct rrr_thread *thread;
};

#define INSTANCE_I_ROUTES(instance) (&instance->routes)
#define INSTANCE_I_METHODS(instance) (&instance->methods)
#define INSTANCE_I_MISC_FLAGS(instance) instance->misc_flags

#define INSTANCE_M_NAME(instance) instance->module_data->instance_name
#define INSTANCE_M_MODULE_TYPE(instance) instance->module_data->type
#define INSTANCE_M_MODULE_NAME(instance) instance->module_data->module_name

struct rrr_instance_collection {
	RRR_LL_HEAD(struct rrr_instance);
	struct rrr_instance *first_entry;
};

struct rrr_instance_event_functions {
	int (*broker_data_available)(RRR_EVENT_FUNCTION_ARGS);
};

struct rrr_instance_module_data {
	const char *instance_name;
	const char *module_name;
	unsigned int type;
	int want_event_dispatch;
	struct rrr_module_operations operations;
	struct rrr_instance_event_functions event_functions;
	void *dl_ptr;
	void *private_data;
	void (*unload)(void);
	struct rrr_instance_collection *all_instances;
};

#define INSTANCE_D_NAME(thread_data) thread_data->init_data.module->instance_name
#define INSTANCE_D_MODULE_NAME(thread_data) thread_data->init_data.module->module_name
#define INSTANCE_D_MODULE(thread_data) thread_data->init_data.module
#define INSTANCE_D_THREAD(thread_data) thread_data->thread
#define INSTANCE_D_INSTANCE(thread_data) thread_data->init_data.instance
#define INSTANCE_D_FLAGS(thread_data) INSTANCE_D_INSTANCE(thread_data)->misc_flags
#define INSTANCE_D_ROUTES(thread_data) &(INSTANCE_D_INSTANCE(thread_data)->routes)
#define INSTANCE_D_METHODS(thread_data) &(INSTANCE_D_INSTANCE(thread_data)->methods)
#define INSTANCE_D_MISC_FLAGS(thread_data) INSTANCE_D_INSTANCE(thread_data)->misc_flags

struct rrr_instance_runtime_init_data {
	struct cmd_data *cmd_data;

	// Shortcuts to other structures
	struct rrr_instance_config_data *instance_config;
	struct rrr_instance_config_collection *global_config;
	struct rrr_instance_module_data *module;
	struct rrr_instance_friend_collection *senders;
	struct rrr_event_queue *events;
	struct rrr_stats_engine *stats;
	struct rrr_message_broker *message_broker;
	struct rrr_fork_handler *fork_handler;
	const struct rrr_mqtt_topic_token *topic_first_token;
	const char *topic_str;
	struct rrr_instance *instance;
	volatile const int *main_running;
};

struct rrr_instance_runtime_data {
	struct rrr_instance_runtime_init_data init_data;

	struct rrr_message_broker_costumer *message_broker_handle;

	void *private_data;
	void *preload_data;
	char private_memory[RRR_MODULE_PRIVATE_MEMORY_SIZE];
	char preload_memory[RRR_MODULE_PRELOAD_MEMORY_SIZE];

	// Poll statistics for modules which read from others
	struct rrr_poll_helper_counters counters;

	// Shorcut which is set by thread itself after starting
	struct rrr_thread *thread;

	// Not used by all modules but managed by instances framework.
	// * Init and cleanup in intermediate thread entry function
	// * Cleanup in ghost handler
	struct rrr_cmodule *cmodule;
	struct rrr_stats_instance *stats;

	// Events used by instances framework
	struct rrr_event_collection events;
};

#define INSTANCE_D_FORK(thread_data) thread_data->init_data.fork_handler
#define INSTANCE_D_STATS(thread_data) thread_data->stats
#define INSTANCE_D_STATS_ENGINE(thread_data) thread_data->init_data.stats
#define INSTANCE_D_COUNTERS(thread_data) (&thread_data->counters)
#define INSTANCE_D_BROKER(thread_data) thread_data->init_data.message_broker
#define INSTANCE_D_HANDLE(thread_data) thread_data->message_broker_handle
#define INSTANCE_D_BROKER_ARGS(thread_data) INSTANCE_D_HANDLE(thread_data)
#define INSTANCE_D_EVENTS(thread_data) thread_data->init_data.events
#define INSTANCE_D_CONFIG(thread_data) thread_data->init_data.instance_config
#define INSTANCE_D_CMODULE(thread_data) thread_data->cmodule
#define INSTANCE_D_SETTINGS(thread_data) thread_data->init_data.instance_config->settings
#define INSTANCE_D_SETTINGS_USED(thread_data) &thread_data->init_data.instance_config->settings_used
#define INSTANCE_D_TOPIC(thread_data) thread_data->init_data.topic_first_token
#define INSTANCE_D_TOPIC_STR(thread_data) thread_data->init_data.topic_str
#define INSTANCE_D_CANCEL_CHECK_ARGS(thread_data) \
		rrr_thread_signal_encourage_stop_check_and_update_watchdog_timer_void, INSTANCE_D_THREAD(thread_data)
#define INSTANCE_D_INSTANCES(thread_data) thread_data->init_data.module->all_instances
#define INSTANCE_D_MAIN_RUNNING(thread_data) (*thread_data->init_data.main_running)

struct rrr_instance *rrr_instance_find_by_thread (
		struct rrr_instance_collection *instances,
		struct rrr_thread *thread
);
int rrr_instance_check_threads_stopped (
		struct rrr_instance_collection *target
);
int rrr_instance_count_library_users (
		struct rrr_instance_collection *target,
		void *dl_ptr
);
void rrr_instance_unload_all(
		struct rrr_instance_collection *target
);
int rrr_instance_has_sender (
		const struct rrr_instance *instance,
		const struct rrr_instance *sender
);
void rrr_instance_collection_clear (
		struct rrr_instance_collection *target
);
int rrr_instance_collection_new (
		struct rrr_instance_collection **target
);
int rrr_instance_load_and_save (
		struct rrr_instance_collection *instances,
		struct rrr_instance_config_data *instance_config,
		const char **library_paths
);
struct rrr_instance *rrr_instance_find (
		struct rrr_instance_collection *target,
		const char *name
);
int rrr_instance_collection_count (
		struct rrr_instance_collection *collection
);
int rrr_instances_create_and_start_threads (
		struct rrr_thread_collection **thread_collection_target,
		struct rrr_instance_collection *instances,
		struct rrr_instance_config_collection *config,
		struct cmd_data *cmd,
		struct rrr_stats_engine *stats,
		struct rrr_message_broker *message_broker,
		struct rrr_fork_handler *fork_handler,
		volatile const int *main_running
);
int rrr_instance_run (
		struct rrr_instance_collection *instances,
		struct rrr_instance_config_collection *config,
		int instance_index,
		struct cmd_data *cmd,
		struct rrr_event_queue *events,
		struct rrr_stats_engine *stats,
		struct rrr_message_broker *message_broker,
		struct rrr_fork_handler *fork_handler,
		volatile const int *main_running
);
int rrr_instances_create_from_config (
		struct rrr_instance_collection *instances,
		struct rrr_instance_config_collection *config,
		const char **library_paths
);
int rrr_instance_default_set_output_buffer_ratelimit_when_needed (
		unsigned int *delivery_entry_count,
		int *delivery_ratelimit_active,
		struct rrr_instance_runtime_data *thread_data
);

#endif /* RRR_INSTANCES_H */
