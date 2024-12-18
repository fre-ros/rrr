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

#include <stdlib.h>
#include <string.h>

#include "../log.h"
#include "../allocator.h"
#include "http_header_fields.h"
#include "http_common.h"
#include "http_util.h"
#include "../util/base64.h"
#include "../map.h"

static int __rrr_http_header_field_trim_name_if_required (
		struct rrr_http_header_field *field
) {
	if ((field->definition->flags & RRR_HTTP_HEADER_FIELD_TRIM) != 0) {
		rrr_nullsafe_str_trim(RRR_LL_FIRST(&field->fields)->name);
	}

	if (rrr_nullsafe_str_len(RRR_LL_FIRST(&field->fields)->name) == 0) {
		RRR_MSG_0("HTTP field '%s' was empty after trimming spaces\n",
			field->definition->name_lowercase);
		return RRR_HTTP_PARSE_SOFT_ERR;
	}

	return RRR_HTTP_PARSE_OK;
}

static int __rrr_http_header_parse_single_value_verify (struct rrr_http_header_field *field) {
	if (!rrr_nullsafe_str_isset(field->name)) {
		RRR_BUG("BUG: Name not set for header field in __rrr_http_header_parse_verify_single_value\n");
	}

	RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(name,field->name);

	if (RRR_LL_COUNT(&field->fields) == 0) {
		RRR_MSG_0("No values found for HTTP header field '%s'\n", name);
		return RRR_HTTP_PARSE_SOFT_ERR;
	}

	if (RRR_LL_COUNT(&field->fields) > 1) {
		RRR_MSG_0("Multiple values not allowed for HTTP header field '%s'\n", name);
		return RRR_HTTP_PARSE_SOFT_ERR;
	}

	if (rrr_nullsafe_str_isset(RRR_LL_FIRST(&field->fields)->value)) {
		RRR_MSG_0("name=value pair not valid for HTTP header field '%s'\n", name);
		return RRR_HTTP_PARSE_SOFT_ERR;
	}

	if ((field->definition->flags & RRR_HTTP_HEADER_FIELD_TRIM) != 0) {
		rrr_nullsafe_str_trim(RRR_LL_FIRST(&field->fields)->name);
	}

	return RRR_HTTP_PARSE_OK;
}

static int __rrr_http_header_parse_single_unsigned_value (RRR_HTTP_HEADER_FIELD_PARSER_DEFINITION) {
	int ret = RRR_HTTP_PARSE_OK;

	if ((ret = __rrr_http_header_parse_single_value_verify(field)) != RRR_HTTP_PARSE_OK) {
		goto out;
	}

	__rrr_http_header_field_trim_name_if_required(field);

	struct rrr_http_field *subvalue = RRR_LL_FIRST(&field->fields);

	if ((ret = rrr_http_util_strtoull (
			&field->value_unsigned,
			subvalue->name,
			10
	)) != 0) {
		RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(name,field->name);
		RRR_MSG_0("Could not get value from field '%s'\n", name);
		goto out;
	}

	char test[64];
	snprintf(test, sizeof(test), "%llu", (long long unsigned) field->value_unsigned);
	test[sizeof(test) - 1] = '\0';

	if (strlen(test) != rrr_nullsafe_str_len(subvalue->name)) {
		RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(name,field->name);
		RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(value,subvalue->name);
		RRR_MSG_0("Syntax error in field '%s' requiring an unsigned value, not all bytes were parsed. Value was '%s'.\n", name, value);
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	out:
	return ret;
}

static int __rrr_http_header_parse_single_string_value (RRR_HTTP_HEADER_FIELD_PARSER_DEFINITION) {
	int ret = RRR_HTTP_PARSE_OK;

	if ((ret = __rrr_http_header_parse_single_value_verify(field)) != RRR_HTTP_PARSE_OK) {
		goto out;
	}

	if ((ret = __rrr_http_header_field_trim_name_if_required(field)) != RRR_HTTP_PARSE_OK) {
		goto out;
	}

	struct rrr_http_field *subvalue = RRR_LL_FIRST(&field->fields);

	if (field->value != NULL) {
		RRR_BUG("BUG: value was not NULL in __rrr_http_header_parse_string_value\n");
	}

	if (rrr_nullsafe_str_dup (&field->value, subvalue->name) != 0) {
		RRR_MSG_0("Could not allocate memory in __rrr_http_header_parse_string_value\n");
		ret = RRR_HTTP_PARSE_HARD_ERR;
		goto out;
	}

	out:
	return ret;
}

static char *__rrr_http_header_parse_base64_value_callback (
		const void *str,
		rrr_nullsafe_len len,
		void *arg
) {
	rrr_biglength *result_len = arg;
	return (char *) rrr_base64_decode (
			str,
			len,
			result_len
	);
}

static int __rrr_http_header_parse_base64_value (RRR_HTTP_HEADER_FIELD_PARSER_DEFINITION) {
	int ret = 0;

	void *base64_data = NULL;
	rrr_biglength base64_len = 0;

	if ((ret = __rrr_http_header_parse_single_string_value(field)) != 0) {
		goto out;
	}

	if (rrr_nullsafe_str_isset(field->binary_value_nullsafe)) {
		RRR_BUG("BUG: binary_value was not NULL in __rrr_http_header_parse_base64_value\n");
	}

	if ((base64_data = rrr_nullsafe_str_with_raw_do_const_return_str (
			field->value,
			__rrr_http_header_parse_base64_value_callback,
			&base64_len
	)) == NULL) {
		RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(name,field->name);
		RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(value,field->value);
		RRR_MSG_0("Base64 decoding failed for field '%s' value was '%s'\n", name, value);
		ret = RRR_HTTP_SOFT_ERROR;
		goto out;
	}

	if ((ret = rrr_nullsafe_str_new_or_replace_raw(&field->binary_value_nullsafe, NULL, 0)) != 0) {
		RRR_MSG_0("Failed to allocate memory in __rrr_http_header_parse_base64_value\n");
		goto out;
	}

	rrr_length base64_len_final;
	if (rrr_length_from_biglength_err (&base64_len_final, base64_len) != 0) {
		RRR_MSG_0("Base64 decoding failed for a field, value too long (%llu>%llu)\n",
			(unsigned long long) base64_len,
			(unsigned long long) RRR_LENGTH_MAX
		);
		ret = RRR_HTTP_SOFT_ERROR;
		goto out;
	}

	rrr_nullsafe_str_set_allocated(field->binary_value_nullsafe, &base64_data, base64_len_final);

	out:
	RRR_FREE_IF_NOT_NULL(base64_data);
	return ret;
}

static int __rrr_http_header_parse_first_string_value (RRR_HTTP_HEADER_FIELD_PARSER_DEFINITION) {
	int ret = RRR_HTTP_PARSE_OK;

	RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(name,field->name);

	if (RRR_LL_COUNT(&field->fields) == 0) {
		RRR_MSG_0("No value found for HTTP header field '%s'\n", name);
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	if (rrr_nullsafe_str_isset(RRR_LL_FIRST(&field->fields)->value)) {
		RRR_MSG_0("name=value pair not valid for HTTP header field '%s' first value\n", name);
		return RRR_HTTP_PARSE_SOFT_ERR;
	}

	struct rrr_http_field *subvalue = RRR_LL_FIRST(&field->fields);

	if (field->value != NULL) {
		RRR_BUG("BUG: value was not NULL in __rrr_http_header_parse_first_string_value\n");
	}

	if ((ret = __rrr_http_header_field_trim_name_if_required(field)) != RRR_HTTP_PARSE_OK) {
		goto out;
	}

	if (rrr_nullsafe_str_dup (&field->value, subvalue->name) != 0) {
		RRR_MSG_0("Could not allocate memory in __rrr_http_header_parse_first_string_value\n");
		ret = RRR_HTTP_PARSE_HARD_ERR;
		goto out;
	}

	out:
	return ret;
}

static int __rrr_http_header_parse_field_name_matches (
		struct rrr_http_field *field,
		const char *names_match[],
		size_t names_match_count
) {
	for (size_t i = 0; i < names_match_count; i++) {
		if (rrr_nullsafe_str_cmpto_case(field->name, names_match[i]) == 0) {
			return 1;
		}
	}
	return 0;
}

static void __rrr_http_header_parse_unquote_fields (
		rrr_nullsafe_len *unquoted_length,
		struct rrr_http_field *field,
		const char *parent_field_name
) {
	*unquoted_length = 0;

	if (!rrr_nullsafe_str_isset(field->value)) {
		return;
	}

	if (rrr_http_util_unquote_string(field->value) != 0) {
		RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(name,field->value);
		RRR_DBG_1("Warning: Syntax error in '%s' subvalue field of '%s' in HTTP header\n",
				name, parent_field_name);
		return;
	}

	if (rrr_http_util_urlencoded_string_decode(field->value) != 0) {
		RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(name,field->value);
		RRR_DBG_1("Warning: Error while decoding url encoding of '%s' subvalue field of '%s' in HTTP header\n",
				name, parent_field_name);
		return;
	}

	*unquoted_length = rrr_nullsafe_str_len(field->value);
}

static int __rrr_http_header_parse_content_type_value (RRR_HTTP_HEADER_FIELD_PARSER_DEFINITION) {
	int ret = 0;

	if ((ret = __rrr_http_header_parse_first_string_value(field)) != 0) {
		goto out;
	}

	RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(parent_name,field->name);

	RRR_LL_ITERATE_BEGIN(&field->fields, struct rrr_http_field);
		const char *unquote_field_names[] = {"charset", "boundary"};
		rrr_nullsafe_len unquoted_length;

		if (!__rrr_http_header_parse_field_name_matches(node, unquote_field_names, 2))
			RRR_LL_ITERATE_NEXT();

		__rrr_http_header_parse_unquote_fields(&unquoted_length, node, parent_name);

		if (unquoted_length == 0) {
			RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(node_name, node->name);
			RRR_DBG_1("Error: Mandatory HTTP header field '%s' of '%s' has no value\n",
				node_name, parent_name);
			ret = RRR_HTTP_SOFT_ERROR;
			goto out;
		}
	RRR_LL_ITERATE_END();

	out:
	return ret;
}

static int __rrr_http_header_parse_content_disposition_value (RRR_HTTP_HEADER_FIELD_PARSER_DEFINITION) {
	int ret = 0;

	if ((ret = __rrr_http_header_parse_first_string_value(field)) != 0) {
		goto out;
	}

	RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(parent_name,field->name);

	RRR_LL_ITERATE_BEGIN(&field->fields, struct rrr_http_field);
		if (RRR_LL_FIRST(&field->fields) == node) {
			if (rrr_nullsafe_str_cmpto_case(node->name, "form-data") != 0 &&
			    rrr_nullsafe_str_cmpto_case(node->name, "attachment") != 0 &&
			    rrr_nullsafe_str_cmpto_case(node->name, "inline") != 0
			) {
				RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(node_name,node->name);
				RRR_DBG_1("Warning: Unknown content-disposition type '%s'\n", node_name);
				RRR_LL_ITERATE_BREAK();
			}
			RRR_LL_ITERATE_NEXT();
		}

		rrr_nullsafe_len unquoted_length;
		const char *unquote_field_names[] = {"name", "filename"};

		if (!__rrr_http_header_parse_field_name_matches(node, unquote_field_names, 2))
			RRR_LL_ITERATE_NEXT();

		__rrr_http_header_parse_unquote_fields(&unquoted_length, node, parent_name);

		if (unquoted_length == 0) {
			RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(node_name, node->name);
			RRR_DBG_1("Error: Mandatory HTTP header field '%s' of '%s' has no value\n",
				node_name, parent_name);
			ret = RRR_HTTP_SOFT_ERROR;
			goto out;
		}
	RRR_LL_ITERATE_END();

	out:
	return ret;
}

static int __rrr_http_header_parse_alt_svc_value (RRR_HTTP_HEADER_FIELD_PARSER_DEFINITION) {
	int ret = 0;

	RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(parent_name,field->name);

	RRR_LL_ITERATE_BEGIN(&field->fields, struct rrr_http_field);
		// h3=":443"; ma=2592000,h3-29=":443"; ma=2592000

		rrr_nullsafe_len unquoted_length;
		const char *unquote_field_names[] = {"h2", "h3", "h3-29", "h3-32", "ma", "persist"};

		if (!__rrr_http_header_parse_field_name_matches(node, unquote_field_names, 6))
			RRR_LL_ITERATE_NEXT();

		__rrr_http_header_parse_unquote_fields(&unquoted_length, node, parent_name);

		if (unquoted_length == 0) {
			RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(node_name, node->name);
			RRR_DBG_1("Error: Mandatory HTTP header field '%s' of '%s' has no value\n",
				node_name, parent_name);
			ret = RRR_HTTP_SOFT_ERROR;
			goto out;
		}
	RRR_LL_ITERATE_END();

	out:
	return ret;
}

static const struct rrr_http_header_field_definition definitions[] = {
        {":status",                RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_single_unsigned_value},
        {":method",                RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_single_string_value},
        {":path",                  RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_single_string_value},

        {":authority",             RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_single_string_value},
        {"host",                   RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_single_string_value},

        {"accept",                 RRR_HTTP_HEADER_FIELD_ALLOW_MULTIPLE,    NULL},
        {"accept-language",        RRR_HTTP_HEADER_FIELD_ALLOW_MULTIPLE,    NULL},
        {"accept-encoding",        RRR_HTTP_HEADER_FIELD_ALLOW_MULTIPLE,    __rrr_http_header_parse_single_string_value},
	{"access-control-request-headers",
	                           RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_single_string_value},
//	{"access-control-allow-methods",
//	                           RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_single_string_value},
        {"cache-control",          RRR_HTTP_HEADER_FIELD_ALLOW_MULTIPLE,    NULL},
        {"connection",             RRR_HTTP_HEADER_FIELD_ALLOW_MULTIPLE,    __rrr_http_header_parse_single_string_value},
        {"upgrade",                RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_single_string_value},
        {"content-disposition",    0,                                       __rrr_http_header_parse_content_disposition_value},
        {"content-length",         RRR_HTTP_HEADER_FIELD_NO_PAIRS |
	                           RRR_HTTP_HEADER_FIELD_TRIM,              __rrr_http_header_parse_single_unsigned_value},
        {"content-type",           RRR_HTTP_HEADER_FIELD_TRIM,              __rrr_http_header_parse_content_type_value},
        {"content-encoding",       RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_single_string_value},
        {"date",                   RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_single_string_value},
        {"link",                   RRR_HTTP_HEADER_FIELD_ALLOW_MULTIPLE |
	                           RRR_HTTP_HEADER_FIELD_ANGLED_QUOTE_NAME, NULL},
        {"location",               RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_single_string_value},
        {"server",                 0,                                       __rrr_http_header_parse_single_string_value},
        {"server-timing",          RRR_HTTP_HEADER_FIELD_ALLOW_MULTIPLE,    __rrr_http_header_parse_first_string_value},
        {"transfer-encoding",      RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_single_string_value},
        {"user-agent",             RRR_HTTP_HEADER_FIELD_NO_PAIRS,          NULL},
        {"vary",                   RRR_HTTP_HEADER_FIELD_ALLOW_MULTIPLE,    __rrr_http_header_parse_single_string_value},
        {"x-clue",                 RRR_HTTP_HEADER_FIELD_NO_PAIRS,          NULL},
        {"sec-websocket-key",      RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_base64_value},
        {"sec-websocket-accept",   RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_base64_value},
        {"sec-websocket-version",  RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_single_string_value},
        {"http2-settings",         RRR_HTTP_HEADER_FIELD_NO_PAIRS,          __rrr_http_header_parse_single_string_value},
        {"alt-svc",                RRR_HTTP_HEADER_FIELD_ALLOW_MULTIPLE,    __rrr_http_header_parse_alt_svc_value},
        {NULL, 0, NULL}
};

static const struct rrr_http_header_field_definition *__rrr_http_header_field_definition_get (
		const char *field,
		rrr_length field_len
) {
	for (int i = 0; 1; i++) {
		const struct rrr_http_header_field_definition *def = &definitions[i];

		if (def->name_lowercase == NULL) {
			break;
		}

		const char *result = NULL;

		rrr_length result_len = 0;
		if (rrr_http_util_strcasestr (
				&result,
				&result_len,
				field,
				field + field_len,
				def->name_lowercase
			) == 0 &&
			result == field &&
			field_len == rrr_length_from_size_t_bug_const(strlen(def->name_lowercase))
		) {
			return def;
		}
	}

	return NULL;
}

void rrr_http_header_field_destroy (
		struct rrr_http_header_field *field
) {
	rrr_http_field_collection_clear(&field->fields);
	rrr_nullsafe_str_destroy_if_not_null(&field->name);
	rrr_nullsafe_str_destroy_if_not_null(&field->binary_value_nullsafe);
	rrr_nullsafe_str_destroy_if_not_null(&field->value);
	rrr_nullsafe_str_destroy_if_not_null(&field->value_full);
	rrr_free (field);
}

void rrr_http_header_field_collection_clear (
		struct rrr_http_header_field_collection *collection
) {
	RRR_LL_DESTROY(collection, struct rrr_http_header_field, rrr_http_header_field_destroy(node));
}

const struct rrr_http_header_field *rrr_http_header_field_collection_get (
		const struct rrr_http_header_field_collection *collection,
		const char *name
) {
	RRR_LL_ITERATE_BEGIN(collection, struct rrr_http_header_field);
		if (rrr_nullsafe_str_cmpto_case(node->name, name) == 0) {
			if (node->definition == NULL || node->definition->parse == NULL) {
				RRR_BUG("Attempted to retrieve field %s which was not parsed in %s, definition must be added\n",
						name, __func__);
			}
			return node;
		}
	RRR_LL_ITERATE_END();
	return NULL;
}

const struct rrr_http_header_field *rrr_http_header_field_collection_get_raw (
		const struct rrr_http_header_field_collection *collection,
		const char *name
) {
	RRR_LL_ITERATE_BEGIN(collection, struct rrr_http_header_field);
		if (rrr_nullsafe_str_cmpto_case(node->name, name) == 0) {
			return node;
		}
	RRR_LL_ITERATE_END();
	return NULL;
}

const struct rrr_http_header_field *rrr_http_header_field_collection_get_with_value_case (
		const struct rrr_http_header_field_collection *collection,
		const char *name_lowercase,
		const char *value_anycase
) {
	RRR_LL_ITERATE_BEGIN(collection, struct rrr_http_header_field);
		if (rrr_nullsafe_str_cmpto(node->name, name_lowercase) == 0) {
			if (node->definition == NULL || node->definition->parse == NULL) {
				RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(name,node->name);
				RRR_BUG("BUG: Attempted to retrieve field %s which was not parsed in %s, definition must be added\n",
						name, __func__);
			}
			if (rrr_nullsafe_str_cmpto_case(node->value, value_anycase) == 0) {
				return node;
			}
		}
	RRR_LL_ITERATE_END();
	return NULL;
}

int rrr_http_header_field_collection_subvalues_iterate (
		const struct rrr_http_header_field_collection *collection,
		const char *name_lowercase,
		int (*callback)(const struct rrr_nullsafe_str *name, const struct rrr_nullsafe_str *value, void *arg),
		void *callback_arg
) {
	int ret = 0;

	RRR_LL_ITERATE_BEGIN(collection, struct rrr_http_header_field);
		if (rrr_nullsafe_str_cmpto(node->name, name_lowercase) != 0) {
			RRR_LL_ITERATE_NEXT();
		}

		if (node->definition == NULL || node->definition->parse == NULL) {
			RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(name,node->name);
			RRR_BUG("BUG: Attempted to retrieve field %s which was not parsed in %s, definition must be added\n",
					name, __func__);
		}

		RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(name,node->name);
		RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(value,node->value);

		const struct rrr_http_header_field *field = node;
		RRR_LL_ITERATE_BEGIN(&field->fields, struct rrr_http_field);
			if ((ret = callback(node->name, node->value, callback_arg)) != 0) {
				goto out;
			}
		RRR_LL_ITERATE_END();
	RRR_LL_ITERATE_END();

	out:
	return ret;
}

static int __rrr_http_header_field_collection_has_subvalue_iterate_callback (
		const struct rrr_nullsafe_str *name,
		const struct rrr_nullsafe_str *value,
		void *arg
) {
	const char *name_subvalue_lowercase = arg;

	(void)(name);

	return rrr_nullsafe_str_cmpto_case(value, name_subvalue_lowercase) == 0;
}

int rrr_http_header_field_collection_has_subvalue (
		const struct rrr_http_header_field_collection *collection,
		const char *name_lowercase,
		const char *name_subvalue_lowercase
) {
	return rrr_http_header_field_collection_subvalues_iterate (
			collection,
			name_lowercase,
			__rrr_http_header_field_collection_has_subvalue_iterate_callback,
			(void *) name_subvalue_lowercase
	);

/*
	RRR_LL_ITERATE_BEGIN(collection, struct rrr_http_header_field);
		if (rrr_nullsafe_str_cmpto(node->name, name_lowercase) == 0) {
			if (node->definition == NULL || node->definition->parse == NULL) {
				RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(name,node->name);
				RRR_BUG("BUG: Attempted to retrieve field %s which was not parsed in %s, definition must be added\n",
						name, __func__);
			}

			const struct rrr_http_header_field *field = node;
			RRR_LL_ITERATE_BEGIN(&field->fields, struct rrr_http_field);
				if (rrr_nullsafe_str_cmpto_case(node->name, name_subvalue_lowercase) == 0) {
					return 1;
				}
			RRR_LL_ITERATE_END();
		}
	RRR_LL_ITERATE_END();
	return 0;
*/
}

int rrr_http_header_field_new_raw (
		struct rrr_http_header_field **result,
		const char *field_name,
		rrr_length field_name_len
) {
	int ret = 0;

	*result = NULL;

	struct rrr_http_header_field *field = rrr_allocate(sizeof(*field));
	if (field == NULL) {
		RRR_MSG_0("Could not allocate memory in __rrr_http_header_field_new\n");
		ret = 1;
		goto out;
	}

	memset (field, '\0', sizeof(*field));

	// Might return NULL, which is OK
	field->definition = __rrr_http_header_field_definition_get(field_name, field_name_len);

	if ((rrr_nullsafe_str_new_or_replace_raw(&field->name, field_name, field_name_len)) != 0) {
		RRR_MSG_0("Could not allocate memory in __rrr_http_header_field_new\n");
		ret = 1;
		goto out_destroy;
	}

	rrr_nullsafe_str_tolower(field->name);

	*result = field;

	goto out;
	out_destroy:
		rrr_http_header_field_destroy(field);
	out:
		return ret;
}

static int __rrr_http_header_field_new_callback (
		const void *str,
		rrr_nullsafe_len len,
		void *arg
) {
	struct rrr_http_header_field **result = arg;

	if (len > RRR_LENGTH_MAX) {
		RRR_MSG_0("HTTP header field too long to be stored (%" PRIrrrbl ">%llu)\n",
			len, (unsigned long long) RRR_LENGTH_MAX);
		return 1;
	}

	return rrr_http_header_field_new_raw (result, str, (rrr_length) len);
}

int rrr_http_header_field_new (
		struct rrr_http_header_field **result,
		const struct rrr_nullsafe_str *nullsafe
) {
	return rrr_nullsafe_str_with_raw_do_const(nullsafe, __rrr_http_header_field_new_callback, result);
}

int rrr_http_header_field_new_with_value (
		struct rrr_http_header_field **result,
		const char *name,
		const char *value
) {
	int ret = 0;

	struct rrr_http_header_field *field = NULL;

	if ((ret = rrr_http_header_field_new_raw(&field, name, rrr_length_from_size_t_bug_const (strlen(name)))) != 0) {
		RRR_MSG_0("Could not create header field in rrr_http_header_field_new\n");
		goto out;
	}

	if ((ret = rrr_nullsafe_str_new_or_replace_raw(&field->value, value, rrr_length_from_size_t_bug_const (strlen(value)))) != 0) {
		RRR_MSG_0("Could not allocate memory for value in rrr_http_header_field_new\n");
		goto out_destroy;
	}

	*result = field;

	goto out;
	out_destroy:
		rrr_http_header_field_destroy(field);
	out:
		return ret;
}

int rrr_http_header_field_new_with_value_nullsafe (
		struct rrr_http_header_field **result,
		const char *name,
		const struct rrr_nullsafe_str *value
) {
	int ret = 0;

	struct rrr_http_header_field *field = NULL;

	if ((ret = rrr_http_header_field_new_raw(&field, name, rrr_length_from_size_t_bug_const (strlen(name)))) != 0) {
		RRR_MSG_0("Could not create header field in %s\n", __func__);
		goto out;
	}

	if ((ret = rrr_nullsafe_str_new_or_replace(&field->value, value)) != 0) {
		RRR_MSG_0("Could not allocate memory for value in %s\n", __func__);
		goto out_destroy;
	}

	*result = field;

	goto out;
	out_destroy:
		rrr_http_header_field_destroy(field);
	out:
		return ret;
}

int rrr_http_header_field_new_with_value_raw (
		struct rrr_http_header_field **result,
		const char *name,
		rrr_length name_length,
		const char *value,
		rrr_length value_length
) {
	int ret = 0;

	struct rrr_http_header_field *field = NULL;

	if ((ret = rrr_http_header_field_new_raw(&field, name, name_length)) != 0) {
		RRR_MSG_0("Could not create header field in %s\n", __func__);
		goto out;
	}

	if ((ret = rrr_nullsafe_str_new_or_replace_raw(&field->value, value, value_length)) != 0) {
		RRR_MSG_0("Could not allocate memory for value in %s\n", __func__);
		goto out_destroy;
	}

	*result = field;

	goto out;
	out_destroy:
		rrr_http_header_field_destroy(field);
	out:
		return ret;

}

static const char *__rrr_http_header_field_parse_get_first_position (
		const char *a,
		const char *b,
		const char *c,
		const char *crlf_never_null
) {
	const char *first = crlf_never_null;

	if (a != NULL) {
		first = a;
	}
	if (b != NULL && b < first) {
		first = b;
	}
	if (c != NULL && c < first) {
		first = c;
	}

	return first;
}

enum rrr_http_header_field_parse_line_end_mode {
	RRR_HTTP_HEADER_FIELD_PARSE_LINE_END_MODE_END,
	RRR_HTTP_HEADER_FIELD_PARSE_LINE_END_MODE_CRLF
};

static int __rrr_http_header_field_line_end_find (
		const char **target,
		const char *start,
		const char *end,
		enum rrr_http_header_field_parse_line_end_mode line_end_mode
) {
	if (line_end_mode == RRR_HTTP_HEADER_FIELD_PARSE_LINE_END_MODE_END) {
		*target = end;
	}
	else {
		*target = rrr_http_util_find_crlf(start, end);
	}
	if (*target == NULL) {
		return RRR_HTTP_PARSE_INCOMPLETE;
	}
	return RRR_HTTP_OK;
}

#define FIND_LINE_END()                                                                                 \
    do {if ((ret = __rrr_http_header_field_line_end_find(&line_end,start,end,line_end_mode)) != 0) {    \
        goto out;                                                                                       \
    }} while(0)

#define SIZE_CHECK()                                                                      \
	do {if ((rrr_length) (end - start_orig) > RRR_LENGTH_MAX) {                       \
		RRR_MSG_0("Data too long while parsing HTTP header fields\n");            \
		ret = RRR_HTTP_PARSE_SOFT_ERR;                                            \
		goto out;                                                                 \
	}} while(0)

static int __rrr_http_header_field_parse_subvalue (
		struct rrr_http_field_collection *target_list,
		rrr_length *parsed_bytes,
		const char * const start_orig,
		const char * const end,
		enum rrr_http_header_field_parse_line_end_mode line_end_mode,
		int field_flags,
		int no_whitespace_check
) {
	int ret = 0;

	struct rrr_http_field *subvalue = NULL;

	*parsed_bytes = 0;

	SIZE_CHECK();

	const char *start = start_orig;

	while (start < end && (*start == ';' || *start == ',')) {
		start++;
	}

	// New value always begins with spaces, except for in bad implementations
	if (!no_whitespace_check) {
		rrr_length whitespace_count = rrr_http_util_count_whsp(start, end);
		if (whitespace_count == 0) {
			// No more values
			*parsed_bytes = rrr_length_from_ptr_sub_bug_const (start, start_orig);
			ret = RRR_HTTP_PARSE_OK;
			goto out;
		}

		start += whitespace_count;
	}

	const char *line_end = NULL;
	FIND_LINE_END();

	const char *quote_end = NULL;
	const char *comma = NULL;
	const char *equal = NULL;
	const char *semicolon = NULL;

	while (*start == ' ' && start < end) {
		start++;
	}

	if (start == line_end) {
		// Line ended with , or ; propably, assume we are done
		ret = RRR_HTTP_PARSE_OK;
		goto out;
	}

	const char *name_end = NULL;
	const char *separator_search_start = start;

	if (	(field_flags & RRR_HTTP_HEADER_FIELD_ANGLED_QUOTE_NAME) &&
		*start == '<' &&
		(quote_end = rrr_http_util_strchr(start, line_end, '>')) != NULL
	) {
		name_end = separator_search_start = quote_end + 1;
	}

	if ((field_flags & RRR_HTTP_HEADER_FIELD_NO_PAIRS) == 0) {
		equal = rrr_http_util_strchr(separator_search_start, line_end, '=');
		semicolon = rrr_http_util_strchr(separator_search_start, line_end, ';');
	}

	if (field_flags & RRR_HTTP_HEADER_FIELD_ALLOW_MULTIPLE) {
		comma = rrr_http_util_strchr(separator_search_start, line_end, ',');
	}

	if (name_end == NULL) {
		name_end =  __rrr_http_header_field_parse_get_first_position(comma, semicolon, equal, line_end);
	}

	rrr_length name_length = rrr_length_from_ptr_sub_bug_const(name_end, start);
	if (name_length <= 0) {
		RRR_MSG_0("No name found while parsing subvalues of HTTP header field\n");
		rrr_http_util_print_where_message(start, end);
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	if (rrr_http_field_new_no_value_raw(&subvalue, start, name_length) != 0) {
		RRR_MSG_0("Could not allocate field in __rrr_http_header_field_subvalue_parse\n");
		ret = RRR_HTTP_PARSE_HARD_ERR;
		goto out;
	}

/*	{
		RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(name,subvalue->name);
		RRR_DBG_3("\tsubvalue name: %s\n", name);
	}*/

	if (name_end == line_end) {
		start = name_end + (line_end_mode == RRR_HTTP_HEADER_FIELD_PARSE_LINE_END_MODE_CRLF ? 2 : 1);
		goto no_value;
	}
	else if (name_end == comma || name_end == semicolon) {
		start = name_end;
		goto no_value;
	}
	else {
		start = name_end + 1 + rrr_http_util_count_whsp(name_end, line_end);
	}

	if (start >= line_end) {
		RRR_MSG_0("Could not find value after = while parsing subvalues of HTTP header field\n");
		ret = RRR_HTTP_PARSE_SOFT_ERR;
		goto out;
	}

	// TODO : This method to find value end is naive, need to parse quoted values correctly

	const char *value_end = __rrr_http_header_field_parse_get_first_position(comma, semicolon, NULL, line_end);
	rrr_length value_length = rrr_length_from_ptr_sub_bug_const (value_end, start);

	if (rrr_http_field_value_set(subvalue, start, value_length) != 0) {
		RRR_MSG_0("Could not allocate memory for value in __rrr_http_header_field_subvalue_parse\n");
		ret = RRR_HTTP_PARSE_HARD_ERR;
		goto out;
	}

	{
		RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(value,subvalue->value);
//		RRR_DBG_3("\tsubvalue value: %s (%" PRIrrrl ")\n", value, subvalue->value->len);
	}

	start += value_length;

	no_value:
	RRR_LL_APPEND(target_list, subvalue);
	subvalue = NULL;

	// Don't parse the last character, needs to be checked by caller
	*parsed_bytes = rrr_length_from_ptr_sub_bug_const (start, start_orig);

	out:
	if (subvalue != NULL) {
		rrr_http_field_destroy(subvalue);
	}
	return ret;
}

static int __rrr_http_header_field_parse_subvalues (
		rrr_length *parsed_bytes,
		int *comma_found_do_duplicate_field,
		struct rrr_http_header_field *field,
		const char * const start_orig,
		const char * const end,
		enum rrr_http_header_field_parse_line_end_mode line_end_mode,
		int bad_client_missing_space_after_comma
) {
	int ret = 0;

	*parsed_bytes = 0;
	*comma_found_do_duplicate_field = 0;

	SIZE_CHECK();

	const char *start = start_orig;

	int prev_subvalue_count = 0;
	do {
		prev_subvalue_count = RRR_LL_COUNT(&field->fields);

		rrr_length parsed_bytes_tmp = 0;
		//RRR_DBG_3("subvalue start: %c bad client: %i\n", *start, bad_client_missing_space_after_comma);
		if ((ret = __rrr_http_header_field_parse_subvalue (
				&field->fields,
				&parsed_bytes_tmp,
				start,
				end,
				line_end_mode,
				(field->definition != NULL ? field->definition->flags : 0),
				bad_client_missing_space_after_comma
		)) != 0) {
			goto out;
		}
		start += parsed_bytes_tmp;

		bad_client_missing_space_after_comma = 0;

		if (start >= end) {
			ret = (line_end_mode == RRR_HTTP_HEADER_FIELD_PARSE_LINE_END_MODE_CRLF ? RRR_HTTP_PARSE_INCOMPLETE : RRR_HTTP_PARSE_OK);
			goto out;
		}

		if (*start == ';') {
			const char *next = start + 1;
			if (next >= end) {
				ret = RRR_HTTP_PARSE_INCOMPLETE;
				goto out;
			}
			if (*next != ' ' && *next != '\t' && *next != '\r') {
				bad_client_missing_space_after_comma = 1;
			}
			if (*next == ',') {
				if (RRR_DEBUGLEVEL_1) {
					RRR_MSG_0("Warning: Comma found after semicolon in HTTP header, bad implementation\n");
					rrr_http_util_print_where_message(start, end);
				}
				start++;
			}
		}

		if (*start == ',') {
			*comma_found_do_duplicate_field = 1;
			break;
		}
	} while (prev_subvalue_count != RRR_LL_COUNT(&field->fields));

	*parsed_bytes = rrr_length_from_ptr_sub_bug_const (start, start_orig);

	out:
	return ret;
}

#define CALLBACK_ARGS                                          \
    rrr_length *parsed_bytes,                                  \
    const char * const start_orig,                             \
    const char * const end,                                    \
    struct rrr_http_header_field **field,                      \
    struct rrr_http_header_field_collection *fields_tmp,       \
    int *missing_space_after_comma,                            \
    void *arg                                                  \

#define CALL_CALLBACK(name)                                                                      \
    do {rrr_length parsed_bytes = 0;                                                             \
        if ((ret = name(&parsed_bytes, start, end, &field, &fields_tmp, &missing_space_after_comma, callback_arg)) != 0) { \
            goto out;                                                                            \
        }                                                                                        \
        start += parsed_bytes;                                                                   \
    } while(0)

static int __rrr_http_header_field_parse (
		struct rrr_http_header_field_collection *target_list,
		rrr_length *parsed_bytes,
		const char * const start_orig,
		const char * const end,
		enum rrr_http_header_field_parse_line_end_mode line_end_mode,
		int (*field_create_callback)(CALLBACK_ARGS),
		int (*whitespace_check_callback)(CALLBACK_ARGS),
		void *callback_arg
) {
	int ret = 0;
	struct rrr_http_header_field_collection fields_tmp = {0};
	struct rrr_http_header_field *field = NULL;

	*parsed_bytes = 0;

	SIZE_CHECK();

	const char *start = start_orig;
	const char *value_start = NULL;

	int empty_subvalues = 0;
	int missing_space_after_comma = 0;
	int more_fields = 1;
	while (more_fields) {
		CALL_CALLBACK(field_create_callback);

		RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(name,field->name);
		RRR_DBG_3("parsing field with name: %s%s\n", name, RRR_LL_COUNT(&fields_tmp) != 0 ? " (multi-value)" : "");

		if (start >= end) {
			RRR_MSG_0("No value for HTTP header field\n");
			ret = RRR_HTTP_PARSE_SOFT_ERR;
			goto out;
		}

		if (value_start == NULL)
			value_start = start;

		CALL_CALLBACK(whitespace_check_callback);

		rrr_length subvalues_parsed_bytes = 0;
		if ((ret = __rrr_http_header_field_parse_subvalues (
				&subvalues_parsed_bytes,
				&more_fields,
				field,
				start,
				end,
				line_end_mode,
				missing_space_after_comma
		)) != 0) {
			if (ret != RRR_HTTP_PARSE_INCOMPLETE) {
				RRR_MSG_0("Invalid syntax in HTTP header field\n");
				rrr_http_util_print_where_message(start, end);
				ret = RRR_HTTP_PARSE_SOFT_ERR;
			}
			goto out;
		}

		if (subvalues_parsed_bytes == 0 && ++empty_subvalues == 4) {
			/* Prevent spamming with long sequences of commas */
			RRR_DBG_1("Error: Too many empty subvalues in HTTP header field with name %s\n", name);
			ret = RRR_HTTP_PARSE_SOFT_ERR;
			goto out;
		}

		start += subvalues_parsed_bytes;

		if (field->definition != NULL && field->definition->parse != NULL && field->definition->parse(field) != 0) {
			RRR_HTTP_UTIL_SET_TMP_NAME_FROM_NULLSAFE(name,field->name);
			RRR_DBG_1("Could not process HTTP header field '%s'\n", name);
			ret = RRR_HTTP_PARSE_SOFT_ERR;
			goto out;
		}

		RRR_LL_APPEND(&fields_tmp, field);
		field = NULL;
	}

	value_start += rrr_http_util_count_whsp(value_start, start);
	field = RRR_LL_FIRST(&fields_tmp);

	if ((ret = rrr_nullsafe_str_new_or_replace_raw (
			&field->value_full,
			value_start,
			rrr_length_from_ptr_sub_bug_const(start, value_start)
	)) != 0) {
		goto out;
	}

	rrr_nullsafe_str_trim_set (
		field->value_full,
		"\r\n\t ",
		4
	);

	field = NULL;

	RRR_LL_MERGE_AND_CLEAR_SOURCE_HEAD(target_list, &fields_tmp);
	*parsed_bytes = rrr_length_from_ptr_sub_bug_const (start, start_orig);

	out:
	if (field != NULL) {
		rrr_http_header_field_destroy(field);
	}
	RRR_LL_DESTROY(&fields_tmp, struct rrr_http_header_field, rrr_http_header_field_destroy(node));
	return ret;
}

struct rrr_http_header_field_parse_value_callback_data {
	const char *name;
	rrr_length name_length;
};

static int __rrr_http_header_field_parse_value_field_create_callback (CALLBACK_ARGS) {
	struct rrr_http_header_field_parse_value_callback_data *callback_data = arg;

	(void)(parsed_bytes);
	(void)(missing_space_after_comma);

	const char *start = start_orig;

	if (RRR_LL_COUNT(fields_tmp) == 0) {
		if (rrr_http_header_field_new_raw (
				field,
				callback_data->name,
				callback_data->name_length
		) != 0) {
			return RRR_HTTP_PARSE_HARD_ERR;
		}
	}
	else {
		// Duplicate field (after comma) or name from caller
		if (rrr_http_header_field_new(field, RRR_LL_LAST(fields_tmp)->name) != 0) {
			return RRR_HTTP_PARSE_HARD_ERR;
		}

		if (*start == ',') {
			start++;
		}

		if (start >= end) {
			return RRR_HTTP_PARSE_INCOMPLETE;
		}
	}

	return RRR_HTTP_PARSE_OK;
}

static int __rrr_http_header_field_parse_value_whitespace_check_callback (CALLBACK_ARGS) {
	struct rrr_http_header_field_parse_value_callback_data *callback_data = arg;

	(void)(callback_data);
	(void)(parsed_bytes);
	(void)(field);
	(void)(fields_tmp);
	(void)(start_orig);
	(void)(end);

	*missing_space_after_comma = 1;

	return RRR_HTTP_PARSE_OK;
}

int rrr_http_header_field_parse_value_raw (
		struct rrr_http_header_field_collection *target_list,
		rrr_length *parsed_bytes,
		const char *name,
		rrr_length name_length,
		const char *value,
		rrr_length value_length
) {
	struct rrr_http_header_field_parse_value_callback_data callback_data = {
		name,
		name_length
	};

	return __rrr_http_header_field_parse (
			target_list,
			parsed_bytes,
			value,
			value + value_length,
			RRR_HTTP_HEADER_FIELD_PARSE_LINE_END_MODE_END,
			__rrr_http_header_field_parse_value_field_create_callback,
			__rrr_http_header_field_parse_value_whitespace_check_callback,
			&callback_data
	);
}

int rrr_http_header_field_parse_value (
		struct rrr_http_header_field_collection *target_list,
		rrr_length *parsed_bytes,
		const char *name,
		const char *value
) {
	return rrr_http_header_field_parse_value_raw (
			target_list,
			parsed_bytes,
			name,
			rrr_length_from_biglength_bug_const(strlen(name)),
			value,
			rrr_length_from_biglength_bug_const(strlen(value))
	);
}

static int __rrr_http_header_field_parse_name_and_value_field_create_callback (CALLBACK_ARGS) {
	(void)(missing_space_after_comma);
	(void)(arg);

	const char *start = start_orig;

	const char *crlf = rrr_http_util_find_crlf(start, end);
	if (crlf == NULL) {
		return RRR_HTTP_PARSE_INCOMPLETE;
	}

	if (start >= crlf) {
		RRR_MSG_0("No value for HTTP header field\n");
		return RRR_HTTP_PARSE_SOFT_ERR;
	}

	if (RRR_LL_COUNT(fields_tmp) == 0) {
		const char *colon = rrr_http_util_strchr(start, crlf, ':');
		if (colon == NULL) {
			RRR_MSG_0("Colon not found in HTTP header field\n");
			rrr_http_util_print_where_message(start, end);
			return RRR_HTTP_PARSE_SOFT_ERR;
		}

		if (rrr_http_header_field_new_raw (
				field,
				start,
				rrr_length_from_ptr_sub_bug_const (colon, start)
		) != 0) {
			return RRR_HTTP_PARSE_HARD_ERR;
		}

		start = colon + 1;
	}
	else {
		// Duplicate field (after comma) or name from caller
		if (rrr_http_header_field_new(field, RRR_LL_LAST(fields_tmp)->name) != 0) {
			return RRR_HTTP_PARSE_HARD_ERR;
		}

		if (*start == ',') {
			start++;
		}

		if (start >= end) {
			return RRR_HTTP_PARSE_INCOMPLETE;
		}
	}

	*parsed_bytes = rrr_length_from_ptr_sub_bug_const (start, start_orig);

	return RRR_HTTP_PARSE_OK;
}

static int __rrr_http_header_field_parse_name_and_value_whitespace_check_callback (CALLBACK_ARGS) {
	(void)(field);
	(void)(fields_tmp);
	(void)(arg);

	const char *start = start_orig;

	const char *crlf = rrr_http_util_find_crlf(start, end);
	if (crlf == NULL) {
		return RRR_HTTP_PARSE_INCOMPLETE;
	}

	rrr_length whitespace_count = rrr_http_util_count_whsp(start, crlf);
	if (start + whitespace_count == crlf) {
		// Continue on next line
		start = crlf + 2;
		crlf = NULL;
	}
	else if (whitespace_count == 0) {
		if (RRR_DEBUGLEVEL_3) {
			RRR_MSG_3("Note: No whitespace after separator while parsing HTTP header field subvalues\n");
			rrr_http_util_print_where_message(start, end);
		}
		*missing_space_after_comma = 1;
	}

	crlf = rrr_http_util_find_crlf(start, end);
	if (crlf == NULL) {
		return RRR_HTTP_PARSE_INCOMPLETE;
	}

	*parsed_bytes = rrr_length_from_ptr_sub_bug_const (start, start_orig);

	return RRR_HTTP_PARSE_OK;
}

int rrr_http_header_field_parse_name_and_value (
		struct rrr_http_header_field_collection *target_list,
		rrr_length *parsed_bytes,
		const char *start_orig,
		const char *end
) {
	if ((unsigned long long) (end - start_orig) > (unsigned long long) RRR_LENGTH_MAX) {
		RRR_MSG_0("HTTP header too long to be parsed (%llu>%llu)\n",
			(unsigned long long) (end - start_orig),
			(unsigned long long) RRR_LENGTH_MAX
		);
		return RRR_HTTP_SOFT_ERROR;
	}

	return __rrr_http_header_field_parse (
			target_list,
			parsed_bytes,
			start_orig,
			end,
			RRR_HTTP_HEADER_FIELD_PARSE_LINE_END_MODE_CRLF,
			__rrr_http_header_field_parse_name_and_value_field_create_callback,
			__rrr_http_header_field_parse_name_and_value_whitespace_check_callback,
			NULL
	);
}
