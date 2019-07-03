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

struct instance_thread_data;
struct rrr_type_definition_collection;

#define MYSQL_CREATE_TABLE_DEFINITION \
	struct instance_thread_data *thread_data, \
	const char *name, \
	const struct rrr_type_definition_collection *definition

struct mysql_module_operations {
	int (*create_table_from_types)(MYSQL_CREATE_TABLE_DEFINITION);
};
