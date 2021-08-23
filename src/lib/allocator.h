/*

Read Route Record

Copyright (C) 2021 Atle Solbakken atle@goliathdns.no

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

#ifndef RRR_ALLOCATOR_H
#define RRR_ALLOCATOR_H

#include <stddef.h>

#include "rrr_types.h"

#define RRR_ALLOCATOR_GROUP_MSG_HOLDER  0
#define RRR_ALLOCATOR_GROUP_MSG         1
#define RRR_ALLOCATOR_GROUP_MAX         1

#define RRR_ALLOCATOR_FREE_IF_NOT_NULL(arg) do{if((arg) != NULL){rrr_free(arg);(arg)=NULL;}}while(0)

struct rrr_mmap_stats;

void *rrr_allocate (rrr_biglength bytes);
void *rrr_allocate_zero (rrr_biglength bytes);
void *rrr_allocate_group (rrr_biglength bytes, size_t group);
void rrr_free (void *ptr);
void *rrr_reallocate (void *ptr_old, rrr_biglength bytes_old, rrr_biglength bytes_new);
void *rrr_reallocate_group (void *ptr_old, rrr_biglength bytes_old, rrr_biglength bytes_new, size_t group);
char *rrr_strdup (const char *str);
int rrr_allocator_init (void);
void rrr_allocator_cleanup (void);
void rrr_allocator_maintenance (struct rrr_mmap_stats *stats);
void rrr_allocator_maintenance_nostats (void);

#endif /* RRR_ALLOCATOR_H */