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
	lua_pushinteger(L,        1          ); /* Push 1 (beginning value for ~script~.hits     */
	lua_setfield   (L,       -2,   "hits"); /* <table>.hits = 1, pops the 1                  */
	lua_setfield   (L,       -2,       fn); /* magnet.cache.~script~ = <table>, pops <table> */
	lua_pop        (L,        2          ); /* Pops magnet and magnet.cache                  */

	/* Only return 1 thing on the stack --> the script/function */
	assert(lua_gettop(L) == 1);
	return EXIT_SUCCESS;
}

static int
magnet_get_script(lua_State * const L, const char * const fn)
{
	struct stat st;

	assert(lua_gettop(L) == 0);

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
	** but let's cover our bases anyway. */
	if (S_ISDIR(st.st_mode))
	{
		WRITE_LITERAL_STR("Status: 400 Bad Request\r\n\r\n", stdout);
		return EXIT_FAILURE;
	}

	lua_getfield(L, LUA_GLOBALSINDEX, "magnet"); 
	assert(lua_istable(L, -1));
	lua_getfield(L, -1, "cache");
	assert(lua_istable(L, -1));

	lua_getfield(L, -1, fn);

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
			lua_Integer hits;
			lua_pop(L, 1);
	
			/* Increment the hit counter. */	
			lua_getfield(L, -1, "hits");
			hits = lua_tointeger(L, -1);
			lua_pop(L, 1);
			lua_pushinteger(L, hits + 1);
			lua_setfield(L, -2, "hits");

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
			lua_pop(L, 4);
			if (magnet_cache_script(L, fn, st.st_mtime))
				return EXIT_FAILURE;
		}
	}
	/* This should be the function (top of Lua stack). */
	assert(lua_gettop(L) == 1);

	return EXIT_SUCCESS;
}

int
main(void)
{
	lua_State * const L = luaL_newstate(); 

	luaL_openlibs(L);

	lua_newtable(L); /* magnet. */
	lua_newtable(L); /* magnet.cache. */
	lua_setfield(L, -2, "cache");
	lua_setfield(L, LUA_GLOBALSINDEX, "magnet");

	while (FCGI_Accept() >= 0)
	{
		assert(lua_gettop(L) == 0);

		/* We couldn't get the script as a function,
		** the appropriate response has been sent,
		** continue to the next iteration. */
		if (magnet_get_script(L, getenv("SCRIPT_FILENAME")))
			continue;

		/**
		 * We want to create empty environment for our script. 
		 * 
		 * setmetatable({}, {__index = _G})
		 * 
		 * If a function symbol is not defined in our env,
		 * __index will look it up in the global env. 
		 *
		 * All variables created in the script-env will be thrown 
		 * away at the end of the script run. */

		/* Empty environment; will become parent to _G._G */
		lua_newtable(L);

		/* We have to overwrite the print function */
		lua_pushcfunction(L, magnet_print);
		lua_setfield(L, -2, "print");

		lua_newtable(L);                    /* The meta-table for the new env.          */
		lua_pushvalue(L, LUA_GLOBALSINDEX);

		lua_setfield(L,     -2, "__index"); /* { __index = _G }                         */
		lua_setmetatable(L, -2);            /* setmetatable({}, { __index = _G })       */
		lua_setfenv(L,      -2);            /* On the stack should be the modified env. */

		/* The top of the stack is the function from magnet_get_script() again. */
		if (lua_pcall(L, 0, 1, 0))
		{
			/* The error will print in-place, there is no gaurantee
			** that the appropriate headers to display the error have been sent.
			** -----------------------------------------------------------------
			** Maybe in the future I can overload print() to print to something other than stdout?
			** Then we could check for a pcall() error and if none occurred, flush what was sent to
			** that dummy stream to the true stdout. */
			fputs(lua_tostring(L, -1), stdout);
			lua_pop(L, 1); /* Remove the error message. */
			continue;
		}

		/* The pcall() succeeded without error,
		** remove the function copy from the stack. */
		lua_pop(L, 1);
	}

	lua_close(L);
	return EXIT_SUCCESS;
}
