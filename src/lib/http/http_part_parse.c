/*

Read Route Record

Copyright (C) 2019-2021 Atle Solbakken atle@goliathdns.no

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

#include "../log.h"
#include "../allocator.h"
#include "http_part.h"
#include "http_part_parse.h"
#include "http_common.h"
#include "http_util.h"

static int __rrr_http_part_parse_response_code (
		struct rrr_http_part *result,
		rrr_biglength *parsed_bytes,
		const char * const buf,
		const rrr_biglength start_pos,
		const char * const end
) {
	int ret = RRR_HTTP_PARSE_OK;

	const char *start = buf + start_pos;

	*parsed_bytes = 0;

	const char *crlf = rrr_http_util_find_crlf(start, end);
	if (crlf == NULL) {
		ret = RRR_HTTP_PARSE_INCOMPLETE;
		goto out;
	}

	if (crlf == start) {
		RRR_MSG_0("No response string found in HTTP response, only CRLF found\n");
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	if (crlf - start < (ssize_t) strlen("HTTP/1.x 200")) {
		ret = RRR_HTTP_PARSE_INCOMPLETE;
		goto out;
	}

	const char *start_orig = start;

	rrr_length tmp_len = 0;
	if (rrr_http_util_strcasestr(&start, &tmp_len, start, crlf, "HTTP/1.1") != 0 || start != start_orig) {
		start = start_orig;
		if (rrr_http_util_strcasestr(&start, &tmp_len, start, crlf, "HTTP/1.0") != 0 || start != start_orig) {
			RRR_MSG_0("Could not understand HTTP response header/version in __rrr_http_parse_response_code\n");
			ret = RRR_HTTP_PARSE_SOFT_ERR;
			goto out;
		}
		result->parsed_version = RRR_HTTP_VERSION_10;
	}
	else {
		result->parsed_version = RRR_HTTP_VERSION_11;
	}

	start += tmp_len;
	start += rrr_http_util_count_whsp(start, end);

	unsigned long long int response_code = 0;
	if (rrr_http_util_strtoull_raw(&response_code, &tmp_len, start, crlf, 10) != 0 || response_code > 999) {
		RRR_MSG_0("Could not understand HTTP response code in __rrr_http_parse_response_code\n");
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}
	result->response_code = (unsigned int) response_code;

	start += tmp_len;
	start += rrr_http_util_count_whsp(start, end);

	if (start < crlf) {
		// ssize_t response_str_len = crlf - start;
		// Response phrase ignored
	}
	else if (start > crlf) {
		RRR_BUG("pos went beyond CRLF in __rrr_http_parse_response_code\n");
	}

	// Must be set when everything is complete
	result->parsed_application_type = RRR_HTTP_APPLICATION_HTTP1;

	*parsed_bytes = rrr_biglength_from_ptr_sub_bug_const(crlf + 2, buf + start_pos);

	out:
	return ret;
}

static int __rrr_http_part_parse_request (
		struct rrr_http_part *result,
		rrr_biglength *parsed_bytes,
		const char *buf,
		rrr_biglength start_pos,
		const char *end
) {
	int ret = RRR_HTTP_PARSE_OK;

	const char *start = buf + start_pos;

	*parsed_bytes = 0;

	const char *crlf = NULL;
	const char *space = NULL;

	if ((crlf = rrr_http_util_find_crlf(start, end)) == NULL) {
		ret = RRR_HTTP_PARSE_INCOMPLETE;
		goto out;
	}

	if (crlf == start) {
		RRR_MSG_0("No request method string found in HTTP request, only CRLF found\n");
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	if ((space = rrr_http_util_find_whsp(start, end)) == NULL) {
		RRR_MSG_0("Whitespace missing after request method in HTTP request\n");
		rrr_http_util_print_where_message(start, end);
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	rrr_length request_method_length;
	if ( rrr_length_from_ptr_sub_err(&request_method_length, space, start) != 0 ||
	     request_method_length > 10 ||
	     request_method_length == 0
	) {
		RRR_MSG_0("Invalid request method in HTTP request\n");
		rrr_http_util_print_where_message(start, end);
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	if (rrr_nullsafe_str_new_or_replace_raw(&result->request_method_str_nullsafe, start, request_method_length) != 0) {
		RRR_MSG_0("Could not allocate string for request method in __rrr_http_parse_request \n");
		ret = RRR_HTTP_PARSE_HARD_ERR;
		goto out;
	}

	start += space - start;
	start += rrr_http_util_count_whsp(start, end);

	if ((space = rrr_http_util_find_whsp(start, end)) == NULL) {
		RRR_MSG_0("Whitespace missing after request URI in HTTP request\n");
		rrr_http_util_print_where_message(start, end);
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	rrr_length request_uri_length;
	if (rrr_length_from_ptr_sub_err (&request_uri_length, space, start) != 0) {
		RRR_MSG_0("Length overflow in HTTP request URI\n");
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	if (rrr_nullsafe_str_new_or_replace_raw(&result->request_uri_nullsafe, start, request_uri_length) != 0) {
		RRR_MSG_0("Could not allocate string for uri in __rrr_http_parse_request \n");
		rrr_http_util_print_where_message(start, end);
		ret = RRR_HTTP_PARSE_HARD_ERR;
		goto out;
	}

	start += space - start;
	start += rrr_http_util_count_whsp(start, end);

	const char *start_orig = start;

	rrr_length protocol_length = 0;
	if ((ret = rrr_http_util_strcasestr(&start, &protocol_length, start_orig, crlf, "HTTP/1.1")) != 0 || start != start_orig) {
		start = start_orig;
		if ((ret = rrr_http_util_strcasestr(&start, &protocol_length, start_orig, crlf, "HTTP/1.0")) != 0 || start != start_orig) {
			RRR_MSG_0("Invalid or missing protocol version in HTTP request\n");
			ret = RRR_HTTP_PARSE_SOFT_ERR;
			goto out;
		}
		result->parsed_version = RRR_HTTP_VERSION_10;
	}
	else {
		result->parsed_version = RRR_HTTP_VERSION_11;
	}

	if (start_orig + protocol_length != crlf) {
		RRR_MSG_0("Extra data after protocol version in HTTP request\n");
		rrr_http_util_print_where_message(start_orig + protocol_length, end);
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	// Must be set when everything is complete
	result->parsed_application_type = RRR_HTTP_APPLICATION_HTTP1;

	*parsed_bytes = rrr_biglength_from_ptr_sub_bug_const(crlf + 2, buf + start_pos);

	out:
	return ret;
}

static int __rrr_http_part_parse_chunk_header (
		struct rrr_http_chunk **result_chunk,
		rrr_biglength *parsed_bytes,
		const char *buf,
		rrr_biglength start_pos,
		const char *end
) {
	int ret = RRR_HTTP_PARSE_OK;

	*parsed_bytes = 0;
	*result_chunk = NULL;

	// TODO : Implement chunk header fields
/*
	char buf_dbg[32];
	memcpy(buf_dbg, buf + start_pos - 16, 16);
	buf_dbg[16] = '\0';

	printf ("Looking for chunk header between %s\n", buf_dbg);
	memcpy(buf_dbg, buf + start_pos, 16);
	buf_dbg[16] = '\0';
	printf ("and %s\n", buf_dbg);
*/
	const char *start = buf + start_pos;
	const char *pos = start;

	if (pos >= end) {
		ret = RRR_HTTP_PARSE_INCOMPLETE;
		goto out;
	}

	const char *crlf = rrr_http_util_find_crlf(pos, end);

	if (pos >= end) {
		ret = RRR_HTTP_PARSE_INCOMPLETE;
		goto out;
	}

	// Allow extra \r\n at beginning
	if (crlf == pos) {
		pos += 2;
		crlf = rrr_http_util_find_crlf(pos, end);
//		printf ("Parsed extra CRLF before chunk header\n");
	}

	if (crlf == NULL) {
		ret = RRR_HTTP_PARSE_INCOMPLETE;
		goto out;
	}

	unsigned long long chunk_length = 0;

	rrr_length parsed_bytes_tmp = 0;
	if ((ret = rrr_http_util_strtoull_raw(&chunk_length, &parsed_bytes_tmp, pos, crlf, 16)) != 0) {
		RRR_MSG_0("Error while parsing chunk length, invalid value\n");
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	if (pos + parsed_bytes_tmp == end) {
		// Chunk header incomplete
		ret = RRR_HTTP_PARSE_INCOMPLETE;
		goto out;
	}
	else if (ret != 0 || (size_t) crlf - (size_t) pos != parsed_bytes_tmp) {
		RRR_MSG_0("Error while parsing chunk length, invalid value\n");
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	pos += parsed_bytes_tmp;
	pos += 2; // Plus CRLF after chunk header

	if (pos + 1 >= end) {
		ret = RRR_HTTP_PARSE_INCOMPLETE;
		goto out;
	}

	struct rrr_http_chunk *new_chunk = NULL;
	rrr_biglength chunk_start = rrr_biglength_from_ptr_sub_bug_const (pos, buf);

//		printf ("First character in chunk: %i\n", *(buf + chunk_start));

	if ((new_chunk = rrr_http_part_chunk_new(chunk_start, chunk_length)) == NULL) {
		ret = RRR_HTTP_PARSE_HARD_ERR;
		goto out;
	}

	*parsed_bytes = rrr_biglength_from_ptr_sub_bug_const (pos, start);
	*result_chunk = new_chunk;

	out:
	return ret;
}

static int __rrr_http_part_header_fields_parse (
		struct rrr_http_header_field_collection *target,
		rrr_biglength *parsed_bytes,
		const char *buf,
		rrr_biglength start_pos,
		const char *end
) {
	int ret = RRR_HTTP_PARSE_OK;

	const char *pos = buf + start_pos;

	*parsed_bytes = 0;

	rrr_biglength parsed_bytes_total = 0;

//	static int run_count_loop = 0;
	while (1) {
//		printf ("Run count loop: %i\n", ++run_count_loop);
		const char *crlf = rrr_http_util_find_crlf(pos, end);
		if (crlf == NULL) {
			// Header incomplete, not enough data
			ret = RRR_HTTP_PARSE_INCOMPLETE;
			goto out;
		}
		else if (crlf == pos) {
			// Header complete
			// pos += 2; -- Enable if needed
			parsed_bytes_total += 2;
			break;
		}

		rrr_length parsed_bytes_tmp = 0;
		if ((ret = rrr_http_header_field_parse_name_and_value(target, &parsed_bytes_tmp, pos, end)) != 0) {
			goto out;
		}

		pos += parsed_bytes_tmp;
		parsed_bytes_total += parsed_bytes_tmp;

		if (pos == crlf) {
			pos += 2;
			parsed_bytes_total += 2;
		}
	}

	out:
	*parsed_bytes = parsed_bytes_total;
	return ret;
}

static int __rrr_http_part_parse_chunk (
		struct rrr_http_chunks *chunks,
		rrr_biglength *parsed_bytes,
		const char *buf,
		rrr_biglength start_pos,
		const char *end
) {
	int ret = 0;

	*parsed_bytes = 0;

	const struct rrr_http_chunk *last_chunk = RRR_LL_LAST(chunks);

	rrr_biglength parsed_bytes_total = 0;
	rrr_biglength parsed_bytes_previous_chunk = 0;

	if (last_chunk != NULL) {
		if (buf + last_chunk->start + last_chunk->length > end) {
			// Need to read more
			ret = RRR_HTTP_PARSE_INCOMPLETE;
			goto out;
		}

		// Chunk is done. Don't add to to total just yet in case
		// parsing of the next chunk header turns out incomplete
		// and we need to parse it again.
		parsed_bytes_previous_chunk = last_chunk->length;
	}

	struct rrr_http_chunk *new_chunk = NULL;

	if ((ret = __rrr_http_part_parse_chunk_header (
			&new_chunk,
			&parsed_bytes_total,
			buf,
			start_pos + parsed_bytes_previous_chunk,
			end
	)) == 0 && new_chunk != NULL) { // != NULL check due to false positive warning about use of NULL from scan-build
		RRR_DBG_3("Found new HTTP chunk start %" PRIrrrbl " length %" PRIrrrbl "\n", new_chunk->start, new_chunk->length);
		RRR_LL_APPEND(chunks, new_chunk);

		// All of the bytes in the previous chunk (if any) have been read
		parsed_bytes_total += parsed_bytes_previous_chunk;

		if (new_chunk == NULL) {
			RRR_BUG("Bug last_chunk was not set but return from __rrr_http_part_parse_chunk_header was OK in rrr_http_part_parse\n");
		}
		if (new_chunk->length == 0) {
			// Last last_chunk
			ret = RRR_HTTP_PARSE_OK;
		}
		else {
			ret = RRR_HTTP_PARSE_INCOMPLETE;
		}
		goto out;
	}
	else if (ret == RRR_HTTP_PARSE_INCOMPLETE) {
		goto out;
	}
	else {
		RRR_MSG_0("Error while parsing last_chunk header in rrr_http_part_parse\n");
		ret = RRR_HTTP_PARSE_HARD_ERR;
		goto out;
	}

	out:
	*parsed_bytes = parsed_bytes_total;
	return ret;
}

static int __rrr_http_part_request_method_and_format_to_enum (
		enum rrr_http_method *method,
		enum rrr_http_body_format *body_format,
		const struct rrr_http_part *part
) {
	int ret = 0;

	*method = 0;
	*body_format = RRR_HTTP_BODY_FORMAT_RAW;

	if (rrr_nullsafe_str_cmpto(part->request_method_str_nullsafe, "GET") == 0) {
		*method = RRR_HTTP_METHOD_GET;
	}
	else if (rrr_nullsafe_str_cmpto(part->request_method_str_nullsafe, "OPTIONS") == 0) {
		*method = RRR_HTTP_METHOD_OPTIONS;
	}
	else if (rrr_nullsafe_str_cmpto(part->request_method_str_nullsafe, "POST") == 0) {
		*method = RRR_HTTP_METHOD_POST;
	}
	else if (rrr_nullsafe_str_cmpto(part->request_method_str_nullsafe, "PUT") == 0) {
		*method = RRR_HTTP_METHOD_PUT;
	}
	else if (rrr_nullsafe_str_cmpto(part->request_method_str_nullsafe, "PATCH") == 0) {
		*method = RRR_HTTP_METHOD_PATCH;
	}
	else if (rrr_nullsafe_str_cmpto(part->request_method_str_nullsafe, "HEAD") == 0) {
		*method = RRR_HTTP_METHOD_HEAD;
	}
	else if (rrr_nullsafe_str_cmpto(part->request_method_str_nullsafe, "DELETE") == 0) {
		*method = RRR_HTTP_METHOD_DELETE;
	}
	else {
		RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(value,part->request_method_str_nullsafe);
		RRR_MSG_0("Unknown request method '%s' in HTTP request (not GET/OPTIONS/POST/PUT/PATCH/HEAD/DELETE)\n", value);
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	const struct rrr_http_header_field *content_type = rrr_http_part_header_field_get(part, "content-type");
	if (content_type != NULL && rrr_nullsafe_str_len(content_type->value)) {
		if (rrr_nullsafe_str_cmpto_case(content_type->value, "multipart/form-data") == 0) {
			*body_format = RRR_HTTP_BODY_FORMAT_MULTIPART_FORM_DATA;
		}
		else if (rrr_nullsafe_str_cmpto_case(content_type->value, "application/x-www-form-urlencoded") == 0) {
			*body_format = RRR_HTTP_BODY_FORMAT_URLENCODED;
		}
#ifdef RRR_WITH_JSONC
		else if (rrr_nullsafe_str_cmpto_case(content_type->value, "application/json") == 0) {
			*body_format = RRR_HTTP_BODY_FORMAT_JSON;
		}
#endif
	}

	out:
	return ret;
}

static int __rrr_http_part_parse_chunked (
		struct rrr_http_part *part,
		rrr_biglength *target_size,
		rrr_biglength *parsed_bytes,
		const char *data_ptr,
		rrr_biglength start_pos,
		const char *end,
		enum rrr_http_parse_type parse_type
) {
	int ret = 0;

	*target_size = 0;
	*parsed_bytes = 0;

	rrr_biglength parsed_bytes_tmp = 0;

	if (parse_type == RRR_HTTP_PARSE_MULTIPART) {
		RRR_MSG_0("Chunked transfer encoding found in HTTP multipart body, this is not allowed\n");
		ret = RRR_HTTP_SOFT_ERROR;
		goto out;
	}

	ret = __rrr_http_part_parse_chunk (
			&part->chunks,
			&parsed_bytes_tmp,
			data_ptr,
			start_pos,
			end
	);

	if (ret == RRR_HTTP_PARSE_OK) {
		if (RRR_LL_LAST(&part->chunks)->length != 0) {
			RRR_BUG("BUG: __rrr_http_part_parse_chunk return OK but last chunk length was not 0 in __rrr_http_part_parse_chunked\n");
		}

		// Part length is position of last chunk plus CRLF minus header and response code
		part->data_length = RRR_LL_LAST(&part->chunks)->start + 2 - part->header_length - part->headroom_length;

		// Target size is total length from start of session to last chunk plus CRLF
		*target_size = RRR_LL_LAST(&part->chunks)->start + 2;
	}

	*parsed_bytes = parsed_bytes_tmp;

	out:
	return ret;
}

int rrr_http_part_parse (
		struct rrr_http_part *part,
		rrr_biglength *target_size,
		rrr_biglength *parsed_bytes,
		const char *data_ptr,
		rrr_biglength start_pos,
		const char *end,
		enum rrr_http_parse_type parse_type
) {
	int ret = RRR_HTTP_PARSE_INCOMPLETE;

//	static int run_count = 0;
//	printf ("Run count: %i pos %i\n", ++run_count, start_pos);

	*target_size = 0;
	*parsed_bytes = 0;

	rrr_biglength parsed_bytes_tmp = 0;
	rrr_biglength parsed_bytes_total = 0;

	if (part->is_chunked == 1) {
		// This is merely a shortcut to skip already checked conditions
		goto out_parse_chunked;
	}

	if (part->parsed_application_type == 0 && parse_type != RRR_HTTP_PARSE_MULTIPART) {
		if (parse_type == RRR_HTTP_PARSE_REQUEST) {
			ret = __rrr_http_part_parse_request (
					part,
					&parsed_bytes_tmp,
					data_ptr,
					start_pos + parsed_bytes_total,
					end
			);
		}
		else if (parse_type == RRR_HTTP_PARSE_RESPONSE) {
			ret = __rrr_http_part_parse_response_code (
					part,
					&parsed_bytes_tmp,
					data_ptr,
					start_pos + parsed_bytes_total,
					end
			);
		}
		else {
			RRR_BUG("BUG: Unknown parse type %i to rrr_http_part_parse\n", parse_type);
		}

		parsed_bytes_total += parsed_bytes_tmp;

		if (ret == RRR_HTTP_PARSE_INCOMPLETE && end - data_ptr > RRR_HTTP_PARSE_HEADROOM_LIMIT_KB * 1024) {
			RRR_MSG_0("HTTP1 request or response line not found in the first %llu kB, triggering soft error.\n",
				(long long unsigned) RRR_HTTP_PARSE_HEADER_LIMIT_KB
			);
			ret = RRR_HTTP_SOFT_ERROR;
			goto out;
		}
		else if (ret != RRR_HTTP_PARSE_OK) {
			if (part->parsed_application_type != 0) {
				RRR_BUG("BUG: Application type was set prior to complete response/request parsing in rrr_http_part_parse\n");
			}
			goto out;
		}
		else if (part->parsed_application_type == 0) {
			RRR_BUG("BUG: Application type not set after complete response/request parsing in rrr_http_part_parse\n");
		}

		part->headroom_length = parsed_bytes_tmp;
	}

	if (part->header_complete) {
		goto out;
	}

	if (start_pos + parsed_bytes_total - part->headroom_length> RRR_HTTP_PARSE_HEADER_LIMIT_KB * 1024) {
		RRR_MSG_0("Received too long HTTP header (fixed limit) (%llu>%llu)\n",
			(unsigned long long) start_pos + parsed_bytes_total - part->headroom_length,
			(unsigned long long) RRR_HTTP_PARSE_HEADER_LIMIT_KB * 1024
		);
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	{
		ret = __rrr_http_part_header_fields_parse (
				&part->headers,
				&parsed_bytes_tmp,
				data_ptr,
				start_pos + parsed_bytes_total,
				end
		);

		parsed_bytes_total += parsed_bytes_tmp;

		// Make sure the maths are done correctly. Header may be partially parsed in a previous round,
		// we need to figure out the header length using the current parsing position
		part->header_length += parsed_bytes_tmp;

		if (ret != RRR_HTTP_PARSE_OK) {
			// Incomplete or error
			goto out;
		}

		part->header_complete = 1;
	}

	if (parse_type == RRR_HTTP_PARSE_REQUEST) {
		{
			RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(method_str, part->request_method_str_nullsafe);
			RRR_DBG_3("HTTP request header parse complete, request method is '%s'\n", method_str);
		}

		if (part->request_method != 0) {
			RRR_BUG("BUG: Numeric request method was non zero in rrr_http_part_parse\n");
		}

		if ((ret = __rrr_http_part_request_method_and_format_to_enum (
				&part->request_method,
				&part->body_format,
				part
		)) != 0) {
			goto out;
		}
	}
	else if (part->response_code > 0) {
		RRR_DBG_3("HTTP completed parsing of a header, response code %u\n", part->response_code);
	}
	else {
		RRR_DBG_3("HTTP completed parsing of a header\n");
	}

	const struct rrr_http_header_field *connection = rrr_http_part_header_field_get(part, "connection");

	if (connection != NULL) {
		if (rrr_nullsafe_str_cmpto_case(connection->value, "close") == 0) {
			RRR_DBG_3("HTTP 'Connection: close' header found\n");
			part->parsed_connection = RRR_HTTP_CONNECTION_CLOSE;
		}
		else if (rrr_nullsafe_str_cmpto_case(connection->value, "keep-alive") == 0) {
			RRR_DBG_3("HTTP 'Connection: keep-alive' header found\n");
			part->parsed_connection = RRR_HTTP_CONNECTION_KEEPALIVE;
		}
		else if (rrr_nullsafe_str_cmpto_case(connection->value, "upgrade") == 0) {
			RRR_DBG_3("HTTP 'Connection: upgrade' header found, implies keep-alive\n");
			part->parsed_connection = RRR_HTTP_CONNECTION_KEEPALIVE;
		}
		else {
			RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(tmp, connection->value);
			RRR_DBG_3("HTTP unknown value '%s' for 'Connection' header ignored\n", tmp);
		}
	}

	if (part->parsed_connection == 0) {
		if (part->parsed_version == RRR_HTTP_VERSION_10) {
			RRR_DBG_3("HTTP 'Connection: close' implied by protocol version HTTP/1.0\n");
			part->parsed_connection = RRR_HTTP_CONNECTION_CLOSE;
		}
		else {
			RRR_DBG_3("HTTP 'Connection: keep-alive' implied by protocol version HTTP/1.1\n");
			part->parsed_connection = RRR_HTTP_CONNECTION_KEEPALIVE;
		}
	}

	const struct rrr_http_header_field *content_length = rrr_http_part_header_field_get(part, "content-length");
	const struct rrr_http_header_field *transfer_encoding = rrr_http_part_header_field_get(part, "transfer-encoding");

	if (content_length != NULL) {
		part->data_length = content_length->value_unsigned;
		*target_size = part->headroom_length + part->header_length + content_length->value_unsigned;

		RRR_DBG_3("HTTP 'Content-Length' found in response: %llu (plus response %" PRIrrrbl " and header %" PRIrrrbl ") target size is %" PRIrrrbl "\n",
				content_length->value_unsigned, part->headroom_length, part->header_length, *target_size);

		ret = RRR_HTTP_PARSE_OK;

		goto out;
	}
	else if (transfer_encoding != NULL && rrr_nullsafe_str_cmpto_case(transfer_encoding->value, "chunked") == 0) {
		RRR_DBG_3("HTTP 'Transfer-Encoding: chunked' found in response\n");
		goto out_parse_chunked;
	}
	else if (	parse_type == RRR_HTTP_PARSE_REQUEST ||
				part->response_code == RRR_HTTP_RESPONSE_CODE_OK_NO_CONTENT ||
				part->response_code == RRR_HTTP_RESPONSE_CODE_SWITCHING_PROTOCOLS
	) {
		goto out_no_content;
	}

	// Unknown size, parse until connection closes
	part->data_length_unknown = 1;
	*target_size = 0;
	ret = RRR_HTTP_PARSE_INCOMPLETE;

	goto out;
	out_no_content:
		part->data_length = 0;
		*target_size = part->headroom_length + part->header_length;
		ret = RRR_HTTP_PARSE_OK;
		goto out;

	out_parse_chunked:
		part->is_chunked = 1;
		ret = __rrr_http_part_parse_chunked (
				part,
				target_size,
				&parsed_bytes_tmp,
				data_ptr,
				start_pos + parsed_bytes_total,
				end,
				parse_type
		);
		parsed_bytes_total += parsed_bytes_tmp;
		goto out;

	out:
		*parsed_bytes = parsed_bytes_total;
		return ret;
}

// Set all required request data without parsing
int rrr_http_part_parse_request_data_set (
		struct rrr_http_part *part,
		rrr_biglength data_length,
		enum rrr_http_application_type application_type,
		enum rrr_http_version version,
		const struct rrr_nullsafe_str *request_method,
		const struct rrr_nullsafe_str *uri
) {
	if ((rrr_nullsafe_str_dup(&part->request_uri_nullsafe, uri)) != 0) {
		return 1;
	}
	if ((rrr_nullsafe_str_dup(&part->request_method_str_nullsafe, request_method)) != 0) {
		return 1;
	}
	if (__rrr_http_part_request_method_and_format_to_enum (
		&part->request_method,
		&part->body_format,
		part
	) != 0) {
		return 1;
	}

	part->parsed_application_type = application_type;
	part->parsed_version = version;
	part->data_length = data_length;
	part->header_complete = 1;
	part->parse_complete = 1;

	return 0;
}

// Set all required response data without parsing
int rrr_http_part_parse_response_data_set (
		struct rrr_http_part *part,
		rrr_biglength data_length
) {
	part->data_length = data_length;
	part->header_complete = 1;
	part->parse_complete = 1;

	return 0;
}
