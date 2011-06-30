/* gcc -o cregistry-test{,.c} -W -Wall -O2 -flto -g -lfcgi -llua -lm -ldl -pedantic -ansi -std=c89 */

#define _BSD_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <lualib.h>
#include <lauxlib.h>

static int
make_ref_in_registry(lua_State * const L, int * const ref, const char * const findme)
{
    int n;
    char *token, *str;
    const char seps[] = ".[]\"\'";

    *ref = LUA_NOREF;

    if (findme == NULL || '\0' == *findme)
        return (*ref);

    str = strdup(findme);
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    n = 1;

    for (token = strtok(str, seps); NULL != token; token = strtok(NULL, seps))
    {
        /* In case the strtok() implementation is non-standard and
        ** it returns empty ("") strings between 2 separators. */
        if ('\0' == *token)
            continue;

        if (!lua_istable(L, -1))
            goto exit;

        lua_getfield(L, -1, token);
        n++;
    }

    /* luaL_ref() pops the last-pushed item */
    *ref = luaL_ref(L, LUA_REGISTRYINDEX);
    n--;

exit:

	lua_pop(L, n);
	free(str);
	return (*ref);
}

int
main(void)
{
	int ref = LUA_REFNIL;

    lua_State * const L = luaL_newstate();

    luaL_openlibs(L);

	set_ref_in_cregistry(L, &ref, "io.write");
	assert(ref != LUA_REFNIL);

	lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
	lua_pushliteral(L, "Hello world!\n");

	lua_call(L, 1, 0);

	lua_close(L);

	return (EXIT_SUCCESS);
}
