/* Compile: gcc   -Wall -O2 -g -o magnet magnet.c -lfcgi -llua -lm -ldl -pedantic -ansi -std=c89
** or     : clang -Wall -O2 -g -o magnet magnet.c -lfcgi -llua -lm -ldl -pedantic -ansi -std=c89
** ---------------------------------------------------------------------------------------------
** I find clang to be so much more descriptive for errors and warnings. <3 */

#include <sys/stat.h>    /* stat()                             */
#include <assert.h>      /* assert() -- *duh*                  */
#include <stdio.h>       /* fwrite(), fprintf(), fputs(), ...  */
#include <stdlib.h>      /* EXIT_SUCCESS, EXIT_FAILURE         */
#include <fcgi_stdio.h>  /* FCGI_Accept()                      */
#include <errno.h>       /* int errno (used with stat() later) */
#include <lualib.h>      /* LUA'Y STUFF :D-S-<                 */
#include <lauxlib.h>

/* A more efficient puts("whatever") -- Because I have fun micro-optimizing without reason */

#define LITERAL_LEN(array) (sizeof(array) / sizeof((array)[0]))

#define WRITE_LITERAL_STR(string, stream) \
	    fwrite((string), sizeof((string)[0]), LITERAL_LEN(string), (stream))

/* This overrides Lua's original print()
** It is functionally equivalent to io.stdout:write(...)
** (Except in that it calls tostring() on each arg) */
static int
magnet_print(lua_State * const L)
{
	const size_t nargs = lua_gettop(L);
	if (nargs)
	{
		const char *s;
		size_t i, s_len;

		lua_getglobal(L, "tostring");
		assert(lua_isfunction(L, -1));

		for (i = 1; i <= nargs; i++)
		{
			lua_pushvalue(L, -1);              /* Push tostring()                      */
			lua_pushvalue(L,  i);              /* Push argument                        */
			lua_call(L, 1, 1);                 /* tostring(argument), take 1, return 1 */
			s =  lua_tolstring(L, -1, &s_len); /* Fetch result.                        */
			
			if (s == NULL)
				return luaL_error(L, LUA_QL("tostring") " must return a string to " LUA_QL("print"));

			fwrite((char *) s, 1, s_len, stdout);

			/* Pop <tostring(argument)> */
			lua_pop(L, 1);
		}
	}
	/* Nothing returned on the Lua stack,
	** so return 0; (exposed cfunction ) */
	return 0;
}

static int
magnet_cache_script(lua_State * const L, const char * const fn, const time_t mtime)
{
	/* Return value from luaL_loadfile() */
	int status = 0;

	/* Compile it as a chunk, push it as a function onto the Lua stack. */
	if ((status = luaL_loadfile(L, fn)))
	{
		switch(status)
		{
			case LUA_ERRSYNTAX:
				printf("Content-Type: text/plain\r\n"
				       "Status: 200 OK\r\n\r\n"
				       "%s",
				       lua_tostring(L, -1));
				break;
			/* Going to assume it exists but we can't open/read it. */
			case LUA_ERRFILE:
				fputs("Status: 403 Forbidden\r\n\r\n", stdout);
				break;
			case LUA_ERRMEM:
				fputs("Status: 503 Service Unavailable\r\n\r\n", stdout);
				break;
		}
		/* Pop err message. */
		lua_pop(L, 1);
		return EXIT_FAILURE;
	}

	/* Make sure loadfile() pushed a function */
	assert(lua_isfunction(L, -1));

	lua_getglobal  (L, "magnet"          ); /* Push magnet from _G                           */
	lua_getfield   (L,       -1,  "cache"); /* Push magnet.cache from _G                     */
	lua_newtable   (L                    ); /* Push a new table                              */
	lua_pushvalue  (L,       -4          ); /* Push the loadfile() function (again)          */
	lua_setfield   (L,       -2, "script"); /* <table>.script = <function>, pops <function>  */
	lua_pushinteger(L,    mtime          ); /* Push mtime                                    */
	lua_setfield   (L,       -2,  "mtime"); /* <table>.mtime = <mtime>, pops <mtime>         */
	lua_pushinteger(L,        0          ); /* Push 1 (beginning value for ~script~.hits     */
	lua_setfield   (L,       -2,   "hits"); /* <table>.hits = 1, pops the 1                  */
	lua_setfield   (L,       -2,       fn); /* magnet.cache.~script~ = <table>, pops <table> */
	lua_pop        (L,        2          ); /* Pops magnet and magnet.cache                  */

	/* Only 2 things on the stack, debug.traceback() and loadfile() function */
	assert(lua_gettop(L) == 2);
	return EXIT_SUCCESS;
}

static int
magnet_get_script(lua_State * const L, const char * const fn)
{
	struct stat st;

	if (fn == NULL)
	{
		WRITE_LITERAL_STR("Status: 400 Bad Request\r\n\r\n", stdout);
		return EXIT_FAILURE;
	}

	if (stat(fn, &st) == -1)
	{
		switch (errno)
		{
			case EACCES:
				WRITE_LITERAL_STR("Status: 403 Forbidden\r\n\r\n", stdout);
				break;
			case ENOENT:
				WRITE_LITERAL_STR("Status: 404 Not Found\r\n\r\n", stdout);
				break;
			default:
				WRITE_LITERAL_STR("Status: 503 Service Unavailable\r\n\r\n", stdout);
				break;
		}
		return EXIT_FAILURE;
	}

	/* Not sure why one would SCRIPT_FILENAME='somedirectory/'
	** but let's cover our bases anyway. I believe loadfile()
	** should be possible with anything *but* a directory */
	if (S_ISDIR(st.st_mode))
	{
		WRITE_LITERAL_STR("Status: 400 Bad Request\r\n\r\n", stdout);
		return EXIT_FAILURE;
	}

	lua_getglobal(L, "magnet");   /* Push magnet from _G              */
	assert(lua_istable(L, -1));   /* assert() magnet is a table       */
	lua_getfield(L, -1, "cache"); /* Push magnet.cache                */
	assert(lua_istable(L, -1));   /* assert() magnet.cache is a table */
	lua_getfield(L, -1, fn);      /* Push magnet.cache['<script>']    */

	/* magnet.cache['<script>'] is not a table for some reason, re-cache. */
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 3); /* Pop the nil. */
		if (magnet_cache_script(L, fn, st.st_mtime))
			return EXIT_FAILURE;
	}
	else
	{
		lua_getfield(L, -1, "mtime");
		assert(lua_isnumber(L, -1));

		/* Script has not been modified, continue as usual. */
		if (st.st_mtime == lua_tointeger(L, -1))
		{
			lua_pop(L, 1);

			/* It is in the cache. */
			assert(lua_istable(L, -1));
			lua_getfield(L, -1, "script");
			assert(lua_isfunction(L, -1));
			lua_insert(L, -4);
			lua_pop(L, 3);
			assert(lua_isfunction(L, -1));
		}
		/* Recorded magnet.cache['<script>'].mtime does
		** not match the actual mtime, re-cache. */
		else
		{
			/* Pop <mtime>, magnet.cache['<script>'],
			** magnet.cache, and magnet. */
			lua_pop(L, 4);
			if (magnet_cache_script(L, fn, st.st_mtime))
				return EXIT_FAILURE;
		}
	}
	/* This should be the function (top of Lua stack). */
	assert(lua_gettop(L) == 2 && lua_isfunction(L, 2));
	return EXIT_SUCCESS;
}

static int
magnet_script_hits_increment(lua_State * const L, const char * const script)
{
	lua_Integer hits;

	lua_getglobal(L, "magnet");   /* Push magnet from _G                                         */
	assert(lua_istable(L, -1));   /* assert() magnet is a table                                  */
	lua_getfield(L, -1, "cache"); /* Push magnet.cache                                           */
	assert(lua_istable(L, -1));   /* assert() magnet.cache is a table                            */
	lua_getfield(L, -1, script);  /* Push magnet.cache['<script>']                               */

	lua_getfield(L, -1, "hits");  /* Push magnet.cache['<script>'].hits                          */
	hits = lua_tointeger(L, -1);  /* hits = tointeger(^.hits)                                    */
	lua_pop(L, 1);                /* Pop  magnet.cache['<script>'].hits                          */
	lua_pushinteger(L, hits + 1); /* Push (hits + 1)                                             */
	lua_setfield(L, -2, "hits");  /* magnet.cache['<script>'].hits = (hits + 1), pops (hits + 1) */

	lua_pop(L, 3);                /* Pop magnet.cache['<script>'], magnet.cache, and magnet      */
	return EXIT_SUCCESS;
}

int
main(void)
{
	lua_State * const L = luaL_newstate(); 

	luaL_openlibs(L);

	lua_newtable   (L                                ); /* Push a new table.                                 */
	lua_newtable   (L                                ); /* Push a new table.                                 */
	lua_setfield   (L,       -2,              "cache"); /* <table#1>.cache = <table#2>, pops <table#2>       */
	lua_pushinteger(L,        0                      ); /* Push 0                                            */
	lua_setfield   (L,       -2, "connections_served"); /* <table#1>.connections_served = 0, pops 0          */
	lua_setglobal  (L, "magnet"                      ); /* _G.magnet = <table#1>, pops <table#1>             */

	lua_getglobal(L, "debug"             ); /* Push debug from _G     */
	lua_getfield (L,      -1, "traceback"); /* Push debug.traceback() */
	lua_remove   (L,       1             ); /* Pop  debug             */

	while (FCGI_Accept() >= 0)
	{
		/* How many connections
		** we have served. */
		lua_Integer connections_served;

		/* debug.traceback() */
		assert(lua_gettop(L) == 1 && lua_isfunction(L, 1));

		/* We couldn't get the script as a function,
		** the appropriate response has been sent,
		** continue to the next iteration. */
		if (magnet_get_script(L, getenv("SCRIPT_FILENAME")))
			continue;

		lua_newtable     (L                             ); /* Will become the env for our script-function        */
		lua_pushcfunction(L,     magnet_print           ); /* Push static int magnet_print(lua_State * const L)  */
		lua_setfield     (L,               -2,   "print"); /* <table>.print() = magnet_print()                   */
		lua_newtable     (L                             ); /* Push a new table.                                  */
		lua_pushvalue    (L, LUA_GLOBALSINDEX           ); /* Push _G                                            */
		lua_setfield     (L,               -2, "__index"); /* <table#2>.__index = _G                             */
		lua_setmetatable (L,               -2           ); /* setmetatable(<table#1>, <table#2>)                 */
		lua_setfenv      (L,               -2           ); /* setfenv(<script-function>, <table#1>) (modded env) */

		/* debug.traceback() and script-function on stack */
		assert(lua_gettop(L) == 2);

		/* Any errors generated by pcall() are printed in-place,
		** don't count on headers having been sent yet.
		** --------------------------------------------
		** 1 item is always returned, 1 (last arg) is debug.traceback() */
		if (lua_pcall(L, 0, 1, 1))
			fputs(lua_tostring(L, -1), stdout);

		/* Pop the 1 returned item from pcall() */
		lua_pop(L, 1);

		/* Increment our hits counter.
		** Must do this after the script
		** is run so all the scripts' .hits
		** == magnet.connections_served */
		magnet_script_hits_increment(L, getenv("SCRIPT_FILENAME"));

		/* Increment our served requets counter. */
		lua_getglobal(L, "magnet");                 /* Push magnet from _G                                                                */
		assert(lua_istable(L, -1));                 /* assert() magnet is a table                                                         */
		lua_getfield(L, -1, "connections_served");  /* Push magnet.connections_served                                                     */
		connections_served = lua_tointeger(L, -1);  /* connections_served = tointeger(magnet.connections_served)                          */
		lua_pop(L, 1);                              /* Pop magnet.connections_served                                                      */
		lua_pushinteger(L, connections_served + 1); /* Push connections_served + 1                                                        */
		lua_setfield(L, -2, "connections_served");  /* magnet.connections_served = (connections_served + 1), pop (connections_served + 1) */
		lua_pop(L, 1);                              /* Pop magnet                                                                         */
	}
	lua_close(L);
	return EXIT_SUCCESS;
}
