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

#ifndef VL_MODULES_H
#define VL_MODULES_H

#include <stdlib.h>
#include <string.h>
#include <error.h>

#include "lib/threads.h"

#define VL_MODULE_TYPE_SOURCE 1
#define VL_MODULE_TYPE_DESTINATION 2
#define VL_MODULE_TYPE_PROCESSOR 3

//#define VL_MODULE_NO_DL_CLOSE

// TODO : Create processor modules

struct module_dynamic_data;
struct reading;

struct vl_thread_start_data;
struct module_thread_data;

struct module_operations {
	void *(*thread_entry)(struct vl_thread_start_data *);

	/* Used by source modules */
	int (*poll)(struct module_thread_data *data, void (*callback)(void *caller_data, char *data, unsigned long int size), struct module_thread_data *caller_data);

	/* Used by output modules */
	int (*print)(struct module_thread_data *data);

};

struct module_dynamic_data {
	const char *name;
	unsigned int type;
	struct module_operations operations;
	void *dl_ptr;
	void *private_data;
	void (*unload)(struct module_dynamic_data *data);
};

struct module_thread_data {
	struct vl_thread *thread;
	struct module_thread_data *sender;
	struct module_dynamic_data *module;
	void *private_data;
};

struct module_thread_init_data {
	struct module_dynamic_data *module;
	struct module_thread_data *sender;
};

void module_threads_init();
void module_threads_stop();
void module_threads_destroy();
void module_free_thread(struct module_thread_data *module);
struct module_thread_data *module_start_thread(struct module_thread_init_data *init_data);
struct module_dynamic_data *load_module(const char *name);
void unload_module(struct module_dynamic_data *data);

#endif
