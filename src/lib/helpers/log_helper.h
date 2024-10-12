/*

Read Route Record

Copyright (C) 2024 Atle Solbakken atle@goliathdns.no

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

#ifndef RRR_LOG_HELPER_H
#define RRR_LOG_HELPER_H

#include <stdint.h>

#include "../read_constants.h"

#define RRR_LOG_HELPER_OK          RRR_READ_OK
#define RRR_LOG_HELPER_SOFT_ERROR  RRR_READ_SOFT_ERROR
#define RRR_LOG_HELPER_HARD_ERROR  RRR_READ_HARD_ERROR

struct rrr_array;

int rrr_log_helper_extract_log_fields_from_array (
		char **log_file,
		int *log_line,
		uint8_t *log_level,
		char **log_prefix,
		char **log_message,
		struct rrr_array *array
);

#endif /* RRR_LOG_HELPER_H */