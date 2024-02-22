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

#include "../log.h"
#include "../allocator.h"

#include "http_service.h"
#include "http_util.h"

#include "../util/linked_list.h"

static void __rrr_http_service_destroy (
		struct rrr_http_service *service
) {
	rrr_http_util_uri_clear(&service->uri);
	rrr_free(service);
}

int rrr_http_service_collection_push (
		struct rrr_http_service_collection *collection,
		const struct rrr_http_uri *uri,
		const struct rrr_http_uri_flags *uri_flags
) {
	int ret = 0;

	struct rrr_http_service *service;

	if ((service = rrr_allocate_zero(sizeof(*service))) == NULL) {
		RRR_MSG_0("Failed to allocate memory in %s\n", __func__);
		ret = 1;
		goto out;
	}

	service->uri = *uri;
	service->uri_flags = *uri_flags;

	RRR_LL_APPEND(collection, service);

	out:
	return ret;
}

void rrr_http_service_collection_clear (
		struct rrr_http_service_collection *collection
) {
	RRR_LL_DESTROY(collection, struct rrr_http_service, __rrr_http_service_destroy(node));
}
