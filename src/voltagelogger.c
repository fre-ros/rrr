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

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

#include "modules.h"
#include "cmdlineparser/cmdline.h"

int main_loop() {
	return 0;
}

static volatile int main_running = 1;

void signal_interrupt (int s) {
    main_running = 0;
}

int main (int argc, const char *argv[]) {
	struct cmd_data cmd;

	int ret = EXIT_SUCCESS;

	if (cmd_parse(&cmd, argc, argv, CMD_CONFIG_NOCOMMAND) != 0) {
		fprintf (stderr, "Error while parsing command line\n");
		ret = EXIT_FAILURE;
		goto out;
	}

	const char *src_module_string = cmd_get_value(&cmd, "src_module");
	const char *p_module_string = cmd_get_value(&cmd, "p_module");
	const char *dst_module_string = cmd_get_value(&cmd, "dst_module");

	if (src_module_string == NULL) {
		src_module_string = "dummy";
	}
	if (p_module_string == NULL) {
		p_module_string = "raw";
	}
	if (dst_module_string == NULL) {
		dst_module_string = "stdout";
	}

	printf ("Using source module '%s' for input\n", src_module_string);
	printf ("Using processor module '%s' for processing\n", p_module_string);
	printf ("Using destination module '%s' for output\n", dst_module_string);

	struct module_dynamic_data *source_module = load_module(src_module_string);
	if (source_module == NULL) {
		fprintf (stderr, "Module %s could not be loaded\n", src_module_string);
		ret = EXIT_FAILURE;
		goto out;
	}

	struct module_dynamic_data *processor_module = load_module(p_module_string);
	if (processor_module == NULL) {
		fprintf (stderr, "Module %s could not be loaded\n", p_module_string);
		ret = EXIT_FAILURE;
		goto out_unload_source;
	}

	struct module_dynamic_data *destination_module = load_module(dst_module_string);
	if (destination_module == NULL) {
		fprintf (stderr, "Module %s could not be loaded\n", dst_module_string);
		ret = EXIT_FAILURE;
		goto out_unload_processor;
	}

	if (source_module->type != VL_MODULE_TYPE_SOURCE) {
		fprintf (stderr, "Module %s could not be used as source module\n", src_module_string);
		ret = EXIT_FAILURE;
	}
	if (processor_module->type != VL_MODULE_TYPE_PROCESSOR) {
		fprintf (stderr, "Module %s could not be used as processor module\n", p_module_string);
		ret = EXIT_FAILURE;
	}
	if (destination_module->type != VL_MODULE_TYPE_DESTINATION) {
		fprintf (stderr, "Module %s could not be used as destination module\n", dst_module_string);
		ret = EXIT_FAILURE;
	}
	if (ret != EXIT_SUCCESS) {
		goto out_unload_all;
	}

	module_threads_init();

	struct module_thread_init_data source_init = {source_module, NULL};
	struct module_thread_data *source_thread = module_start_thread(&source_init);
	if (source_thread == NULL) {
		fprintf (stderr, "Error while starting source thread\n");
		ret = EXIT_FAILURE;
		goto out_stop_threads;
	}

	struct module_thread_init_data processor_init = {processor_module, source_thread};
	struct module_thread_data *processor_thread = module_start_thread(&processor_init);
	if (processor_thread == NULL) {
		fprintf (stderr, "Error while starting processor thread\n");
		ret = EXIT_FAILURE;
		goto out_stop_threads;
	}

	struct module_thread_init_data destination_init = {destination_module, processor_thread};
	struct module_thread_data *destination_thread = module_start_thread(&destination_init);
	if (destination_thread == NULL) {
		fprintf (stderr, "Error while starting output thread\n");
		ret = EXIT_FAILURE;
		goto out_stop_threads;
	}

	// TODO : join threads

	struct sigaction action;

	action.sa_handler = signal_interrupt;
	sigemptyset (&action.sa_mask);
	action.sa_flags = 0;

	sigaction (SIGINT, &action, NULL);

	while (main_running) {
		usleep (20000000);
		break;
	}

	out_stop_threads:
	module_threads_stop();

	module_free_thread(source_thread);
	module_free_thread(processor_thread);
	module_free_thread(destination_thread);

	out_unload_all:
	unload_module(destination_module);

	out_unload_processor:
	unload_module(processor_module);

	out_unload_source:
	unload_module(source_module);

	module_threads_destroy();

	out:
	return ret;
}
