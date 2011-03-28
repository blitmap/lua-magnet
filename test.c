/* Compile: gcc -o test{,.c} -W -Wall -llua -lm -ldl -O2 -g -ansi -std=c99 -pedantic */

#include <lualib.h>
#include <lauxlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <assert.h>

#if _WIN32
#	include <process.h>
#else
#	include <unistd.h>
extern const char * const * const environ;
#endif

#define ARRAY_LENGTH(arr)             (sizeof(arr) / sizeof(arr[0]))
#define WRITE_LITERAL(string, stream) fwrite(string, sizeof(char), ARRAY_LENGTH(string), stream)
#define FREE(x)                       (void) (free(x), (x) = NULL)
#define strbytelen(str)               (strlen(str) + 1)

/* This function returns the byte width of the
** longest key in the strings in the array pointed
** to by `environ'. I know `offset' should possibly
** be ptrdiff_t, but we never use negative offsets. */
size_t
get_longest_env_pair_sz(void)
{
	assert(NULL != environ);

	size_t longest = 0;

	for (const char * const * p = environ; NULL != *p; p++)
	{
		const size_t tmp = strbytelen(*p);
		if (tmp > longest)
			longest = tmp;
	}

	return longest;
}

int
main(void)
{
	lua_State * const L = luaL_newstate();

	/* Our buffer. */
	char *buffer;

	/* Character length of longest
	** string in **environ */
	const unsigned int pair_size = get_longest_env_pair_sz();

	luaL_openlibs(L);

	/* Push _G and a new table. */
	lua_pushvalue(L, LUA_GLOBALSINDEX);
	lua_newtable (L                  );

	/* Our buffer will be large enough
	** to hold the environment variable. */
	buffer = malloc(pair_size);

	/* p gets reset to environ */
	for (const char * const * p = environ; NULL != *p; p++)
	{
		const size_t tmp_len = strbytelen(*p);

		strncpy(buffer, *p, tmp_len);
		char * const sep = memchr(buffer, '=', tmp_len);

		/* Terminate the
		** key section. */
		*sep = '\0';

		/* Push the value */
		lua_pushstring(L, sep + 1);

		/* Buffer points to the beginning of the
		** title field we just nul-terminated. */
		lua_setfield(L, -2, buffer);
	}

	FREE(buffer);

	/* Pop the table, set it as the value of
	** member .env in _G (global env/table) */
	lua_setfield(L, -2, "_env");

	/* Pop the global env/table (_G) */
	lua_pop(L, 1);

	luaL_loadstring(L, "for k, v in pairs(_env) do print(k .. '=\\'' .. v .. '\\'') end");
	lua_call(L, 0, 0);

	WRITE_LITERAL("\n\nEnd. :)\n", stdout);

	/* CLEANUP */
	lua_close(L);

	return EXIT_SUCCESS;
}
