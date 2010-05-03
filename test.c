/* Compile: gcc -o test{,.c} -W -Wall -llua -lm -ldl -O2 -g -ansi -std=c89 -pedantic */

#include <lualib.h>
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#if _WIN32
#include <process.h>
#else
#include <unistd.h>
extern const char * const * const environ;
#endif

int
main(void)
{
	lua_State * const L = luaL_newstate();

	/* Our iterative pointers. */
	const char * const *p;
	const char *tmp_ptr;

	/* Our buffer. */
	char *buffer;

	/* Character length of longest
	** string in **environ */
	ptrdiff_t max_var_length = 0;

	luaL_openlibs(L);

	/* Push _G and a new table. */
	lua_pushvalue(L, LUA_GLOBALSINDEX);
	lua_newtable(L);

	for (p = environ; *p != NULL; p++)
	{
		ptrdiff_t tmp;

		/* As far as I know, a '=' will *always* be present. */
		for (tmp_ptr = *p; *tmp_ptr != '='; tmp_ptr++);

		/* The offset from the current env variable
		** address to the = within the env variable. */
		tmp = (tmp_ptr - (*p));

		if (tmp > max_var_length)
			max_var_length = tmp;
	}

	/* Our buffer will be large enough
	** to hold the environment variable. */
	buffer = (char *) malloc(max_var_length + 1);

	/* Don't want to let the requester be able to cause a
	** stack overflow if the webserver translates sent headers
	** to HTTP_* like is possible to enable in Cherokee.
	** buffer = (char [max_var_length + 1]){0}; */

	/* p gets reset to environ */
	for (p = environ; *p != NULL; p++)
	{
		const char *a = *p;
		char *b = buffer;

		/* Copy the key to `buffer', set a to point to the value. */
		while (*a != '=' && b != &buffer[max_var_length + 1])
		{
			*b = *a;
			a++;
			b++;
		}

		/* Make sure we null-terminate our var name. */
		*b = '\0';

		/* Push the value */
		lua_pushstring(L, a + 1);

		/* Pop the value, assign it to the last table
		** we created (-2 in stack) under the "member .<buffer> */
		lua_setfield(L, -2, buffer);
	}

	free(buffer);
	buffer = NULL;

	/* Pop the table, set it as the value of
	** member .env in _G (global env/table) */
	lua_setfield(L, -2, "env");

	/* Pop the global env/table (_G) */
	lua_pop(L, 1);

	luaL_loadstring(L, "for k, v in pairs(_G.env) do print(k .. '=\\'' .. v .. '\\'') end");
	lua_call(L, 0, 0);

	fwrite("\n\nEnd. :)\n", 1, 10, stdout);

	/* CLEANUP */
	lua_close(L);

	return EXIT_SUCCESS;
}
