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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "../lib/log.h"
#include "../lib/allocator.h"

#include "test.h"
#include "test_lua.h"
#include "../lib/lua/lua.h"
#include "../lib/lua/lua_message.h"
#include "../lib/util/macro_utils.h"

static int __rrr_test_lua_execute_snippet (
		struct rrr_lua *lua,
		const char *snippet,
		int expect_ret
) {
	int ret = 0;

	if ((ret = rrr_lua_execute_snippet(lua, snippet, strlen(snippet))) != 0) {
		TEST_MSG("Failed to execute Lua snippet '%s'\n", snippet);
		ret = 1;
		goto out;
	}

	out:
	return ret != expect_ret;
}

static int __rrr_test_lua_call (
		struct rrr_lua *lua,
		const char *function,
		int a,
		int b,
		int expect_ret
) {
	int ret = 0;

	rrr_lua_pushint(lua, a);
	rrr_lua_pushint(lua, b);

	if ((ret = rrr_lua_call(lua, function, 2)) != 0) {
		TEST_MSG("Failed to call Lua function '%s'\n", function);
		ret = 1;
		goto out;
	}

	out:
	return ret != expect_ret;
}

int rrr_test_lua(void) {
	int ret = 0;

	struct rrr_lua *lua;

	if ((ret = rrr_lua_new(&lua)) != 0) {
		TEST_MSG("Failed to craete Lua in %s\n", __func__);
		goto out;
	}

	rrr_lua_message_library_register(lua);

	TEST_MSG("Execute Lua snippet...\n");
	ret |= __rrr_test_lua_execute_snippet(lua, "a = 1 - 1\nreturn a", 0);

	TEST_MSG("Iterate RRR table...\n");
	ret |= __rrr_test_lua_execute_snippet(lua,
		"for k, v in pairs(RRR) do\n" \
		"  print(k, \"=>\", v)\n"     \
		"end"
	, 0);

	TEST_MSG("Make RRR Message...\n");
	ret |= __rrr_test_lua_execute_snippet(lua,
		"msg = RRR.Message:new()\n"   \
		"for k, v in pairs(msg) do\n" \
		"  print(k, \"=>\", v)\n"     \
		"end"
	, 0);

	TEST_MSG("Call function (failing, arguments are swapped around)...\n");
	ret |= __rrr_test_lua_execute_snippet(lua,
		"function f(a,b)\n"   \
		"  assert(a==1)\n"    \
		"  assert(b==2)\n"    \
		"  return true\n"     \
		"end"
	, 0);
	ret |= __rrr_test_lua_call (lua, "f", 2, 1, 1);

	TEST_MSG("Call function (succeeding)...\n");
	ret |= __rrr_test_lua_execute_snippet(lua,
		"function f(a,b)\n"   \
		"  assert(a==1)\n"    \
		"  assert(b==2)\n"    \
		"  return true\n"     \
		"end"
	, 0);
	ret |= __rrr_test_lua_call (lua, "f", 1, 2, 0);

	goto out_destroy;
	out_destroy:
		rrr_lua_destroy(lua);
	out:
		return ret;
}
