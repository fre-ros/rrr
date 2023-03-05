/*

Read Route Record

Copyright (C) 2023 Atle Solbakken atle@goliathdns.no

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <assert.h>
#include <string.h>

#include "test.h"
#include "test_route_definition.h"
#include "../lib/route.h"
#include "../lib/parse.h"

static const char *fails[] = {
	// Empty definition
	"",
	// Invalid keyword
	"INVALID",
	// Missing value after keywords
	"T    ",
	"H    ",
	"I    ",
	// Invalid topic filter, array tag and instance name
	"T ##\tPOP",
	"H a-",
	"I a$",
	// Not enough arguments
	"AND",
	"OR",
	"NOT",
	"APPLY",
	"POP",
	"T a\tAND",
	"T a\tOR",
	"I a\tAPPLY",
	// Incorrect argument types
	"I a H b AND",
	"H a I b OR",
	"I a     NOT",
	"H a H b APPLY",
	"I a H b APPLY",
	// Stack not empty
	"H a H a POP",
	// End missing
	"T ##",
	"H a"
};

const enum rrr_route_fault fail_codes[] = {
	RRR_ROUTE_FAULT_END_MISSING,

	RRR_ROUTE_FAULT_SYNTAX_ERROR,

	RRR_ROUTE_FAULT_VALUE_MISSING,
	RRR_ROUTE_FAULT_VALUE_MISSING,
	RRR_ROUTE_FAULT_VALUE_MISSING,

	RRR_ROUTE_FAULT_INVALID_VALUE,
	RRR_ROUTE_FAULT_INVALID_VALUE,
	RRR_ROUTE_FAULT_INVALID_VALUE,

	RRR_ROUTE_FAULT_STACK_COUNT,
	RRR_ROUTE_FAULT_STACK_COUNT,
	RRR_ROUTE_FAULT_STACK_COUNT,
	RRR_ROUTE_FAULT_STACK_COUNT,
	RRR_ROUTE_FAULT_STACK_COUNT,
	RRR_ROUTE_FAULT_STACK_COUNT,
	RRR_ROUTE_FAULT_STACK_COUNT,
	RRR_ROUTE_FAULT_STACK_COUNT,

	RRR_ROUTE_FAULT_INVALID_TYPE,
	RRR_ROUTE_FAULT_INVALID_TYPE,
	RRR_ROUTE_FAULT_INVALID_TYPE,
	RRR_ROUTE_FAULT_INVALID_TYPE,
	RRR_ROUTE_FAULT_INVALID_TYPE,

	RRR_ROUTE_FAULT_STACK_COUNT,
	RRR_ROUTE_FAULT_END_MISSING,
	RRR_ROUTE_FAULT_END_MISSING
};

static const char *valids[] = {
	"H a T b\tAND\tPOP",
	"H a T b\tOR\tPOP",
	"H a I c\tAPPLY\tPOP # Comment",
	"H a\tH b I c\tAPPLY\tAND I d\tAPPLY\tPOP"
};

int rrr_test_route_definition(void) {
	int ret = 0;

	struct rrr_route_collection routes = {0};

	assert(sizeof(fails)/sizeof(*fails) == sizeof(fail_codes)/sizeof(*fail_codes));

	for (unsigned int i = 0; i < sizeof(fails)/sizeof(*fails); i++) {
		const char *fail = fails[i];
		const enum rrr_route_fault fail_code = fail_codes[i];

		TEST_MSG("%s\n -> ", fail);

		struct rrr_parse_pos pos;

		rrr_parse_pos_init(&pos, fail, rrr_length_from_biglength_bug_const(strlen(fail)));

		enum rrr_route_fault fault = 0;
		int ret_tmp = 0;
		if ((ret_tmp = rrr_route_interpret (&routes, &fault, &pos, fail)) != 1) {
			TEST_MSG(" NOT OK - Test '%s' did not fail as expected, result was %i fault was %i\n",
					fail, ret_tmp, fault);
			ret = 1;
		}
		else if (fault != fail_code) {
			TEST_MSG(" -> NOT OK - Test '%s' fault code mismatch %i<>%i\n",
					fail, fault, fail_code);
			ret = 1;
		}
		else {
			TEST_MSG(" -> OK\n");
			assert(RRR_LL_COUNT(&routes) == 0);
		}
	}

	for (unsigned int i = 0; i < sizeof(valids)/sizeof(*valids); i++) {
		const char *valid = valids[i];

		TEST_MSG("%s\n -> ", valid);

		struct rrr_parse_pos pos;

		rrr_parse_pos_init(&pos, valid, rrr_length_from_biglength_bug_const(strlen(valid)));

		enum rrr_route_fault fault = 0;
		int ret_tmp = 0;
		if ((ret_tmp = rrr_route_interpret (&routes, &fault, &pos, valid)) != 0) {
			assert(fault != 0);
			TEST_MSG(" -> NOT OK - Test did not succeed as expected, result was %i fault was %i\n",
					ret_tmp, fault);
			ret = 1;
		}
		else {
			assert(fault == 0);
			TEST_MSG(" OK\n");
		}
	}

	if (ret == 0) {
		assert(RRR_LL_COUNT(&routes) == sizeof(valids)/sizeof(*valids));
	}

	rrr_route_collection_clear(&routes);

	return ret;
}
