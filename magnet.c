/* Compile: gcc   -o magnet{,.c} -W -Wall -O2 -g -lfcgi -llua -lm -ldl -pedantic -ansi -std=c89
**        : clang -o magnet{,.c} -W -Wall -O2 -g -lfcgi -llua -lm -ldl -pedantic -ansi -std=c89
** ---------------------------------------------------------------------------------------------
** I find clang to be so much more descriptive for errors and warnings. <3 */

#include <sys/stat.h>    /* stat()                             */
#include <assert.h>      /* assert() -- *duh*                  */
#include <stdio.h>       /* fwrite(), fprintf(), fputs(), ...  */
#include <stdlib.h>      /* EXIT_SUCCESS, EXIT_FAILURE         */
#include <fcgi_stdio.h>  /* FCGI_Accept()                      */
#include <errno.h>       /* int errno (used with stat() later) */
#include <lualib.h>      /* LUA'Y STUFF :D -S-<                 */
#include <lauxlib.h>

/* For getting the length of an array (obviously) */
#define ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))

/* A more efficient puts("read-only literal str")
** Because I have fun micro-optimizing without reason */
#define WRITE_LITERAL_STR(string, stream) \
	fwrite((string), sizeof((string)[0]), ARRAY_LEN(string), (stream))

/* I don't care how ugly this name is,
** just write the damn page with the
** provided mime type, status, and message. */
#define WRITE_STATIC_PAGE(type, status, message) \
	WRITE_LITERAL_STR("Content-Type: " type "\r\nStatus: " status "\r\n\r\n" message, stdout)

/* For conveniently sending error headers
** with the content being the status. */
#define ERR_PAGE(status) \
	WRITE_STATIC_PAGE("text/html", status, status "\r\n")

/* magnet_print() becomes the underlying C function for print(),
** it is equivalent to io.stdout:write(...) except in that it
** calls tostring() on each arg, possibly invoking a __tostring metamethod */
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
			lua_pushvalue    (L, -1        ); /* Push tostring()                      */
			lua_pushvalue    (L,  i        ); /* Push argument                        */
			lua_call         (L,  1,      1); /* tostring(argument), take 1, return 1 */
			s = lua_tolstring(L, -1, &s_len); /* Fetch result.                        */
			
			if (s == NULL)
				return luaL_error(L, LUA_QL("tostring") " must return a string to " LUA_QL("print"));

			/* sizeof(char) == 1 */
			fwrite((char *) s, 1, s_len, stdout);

			/* Pop <tostring(argument)> */
			lua_pop(L, 1);
		}
	}
	/* Nothing returned on the Lua stack,
	** so return 0; (exposed cfunction) */
	return 0;
}

static int
magnet_cache_script(lua_State * const L, const char * const filepath, const time_t mtime)
{
	/* Return value from luaL_loadfile() */
	const int status = luaL_loadfile(L, filepath);

	/* Compile it as a chunk, push it as a function onto the Lua stack. */
	if (status)
	{
		switch (status)
		{
			/* We assume that the only reason loadfile() could
			** fail with this error is it being unreadable to us.
			** We know it exists from the successful stat() */
			case LUA_ERRFILE:   ERR_PAGE("403 Forbidden");           break;
			case LUA_ERRMEM:    ERR_PAGE("503 Service Unavailable"); break;
			case LUA_ERRSYNTAX:
				/* Macros are substitution, we don't
				** need to provide the third arg. --except that's a C99 thing T.T */
				WRITE_STATIC_PAGE("text/html", "200 OK", "");
				fputs(lua_tostring(L, -1), stdout);
				/* Kept for peer review?  Thought it was an interesting comparison to debate.
				printf
				(
					"Content-Type: text/html\r\n"
					"Status: 200 OK\r\n"
					"\r\n"
					"%s\r\n",
					lua_tostring(L, -1)
				);
				*/
				break;
		}
		/* Pop error message. */
		lua_pop(L, 1);
		return EXIT_FAILURE;
	}

	/* Make sure loadfile() pushed a function */
	assert(lua_gettop(L) == 2 && lua_isfunction(L, -1));

	lua_getglobal  (L, "magnet"          ); /* Push magnet from _G                           */
	lua_getfield   (L,       -1,  "cache"); /* Push magnet.cache from _G                     */
	lua_newtable   (L                    ); /* Push a new table                              */
	lua_pushvalue  (L,       -4          ); /* Push the loadfile() function (again)          */
	lua_setfield   (L,       -2, "script"); /* <table>.script = <function>, pops <function>  */
	lua_pushinteger(L,    mtime          ); /* Push mtime                                    */
	lua_setfield   (L,       -2,  "mtime"); /* <table>.mtime = <mtime>, pops <mtime>         */
	lua_pushinteger(L,        0          ); /* Push 1 (beginning value for ~script~.hits     */
	lua_setfield   (L,       -2,   "hits"); /* <table>.hits = 1, pops the 1                  */
	lua_setfield   (L,       -2, filepath); /* magnet.cache.~script~ = <table>, pops <table> */
	lua_pop        (L,        2          ); /* Pops magnet and magnet.cache                  */

	/* Only 2 things on the stack, debug.traceback() and loadfile() function */
	assert(lua_gettop(L) == 2 && lua_isfunction(L, 2));
	return EXIT_SUCCESS;
}

static int
magnet_get_script(lua_State * const L, const char * const filepath)
{
	struct stat script;

	if (filepath == NULL)
	{
		ERR_PAGE("400 Bad Request");
		return EXIT_FAILURE;
	}

	if (stat(filepath, &script) == -1)
	{
		switch (errno)
		{
			case EACCES: ERR_PAGE("403 Forbidden");           break;
			case ENOENT: ERR_PAGE("404 Not Found");           break;
			default:     ERR_PAGE("503 Service Unavailable"); break;
		}
		return EXIT_FAILURE;
	}

	/* Not sure why one would SCRIPT_FILENAME='somedirectory/'
	** but let's cover our bases anyway. I believe loadfile()
	** should be possible with anything *but* a directory */
	if (S_ISDIR(script.st_mode))
	{
		ERR_PAGE("400 Bad Request");
		return EXIT_FAILURE;
	}

	lua_getglobal     (L, "magnet"          );  /* Push magnet from _G              */
	assert(lua_istable(L,       -1          )); /* assert() magnet is a table       */
	lua_getfield      (L,       -1,  "cache");  /* Push magnet.cache                */
	assert(lua_istable(L,       -1          )); /* assert() magnet.cache is a table */
	lua_getfield      (L,       -1, filepath);  /* Push magnet.cache['<script>']    */

	/* magnet.cache['<script>'] is not a
	** table for some reason, re-cache. */
	if (!lua_istable(L, -1))
	{
		lua_pop(L, 3); /* Pop whatever magnet.cache['<script>'] is, magnet.cache, and magnet */
		if (magnet_cache_script(L, filepath, script.st_mtime))
			return EXIT_FAILURE;
	}
	else
	{
		/* Push magnet.cache['<script>'].mtime */
		lua_getfield(L, -1, "mtime");

		/* Script has not been modified, continue as usual. */
		if (script.st_mtime == lua_tointeger(L, -1))
		{
			lua_pop              (L,  1          );  /* Pop <mtime>                                            */
			lua_getfield         (L, -1, "script");  /* Push magnet.cache['<script>'].script                   */
			assert(lua_isfunction(L, -1          )); /* assert() ^ is a function                               */
			lua_insert           (L, -4          );  /* Insert the function 3 indexes before                   */
			lua_pop              (L,  3          );  /* Pop magnet, magnet.cache, and magnet.cache['<script>'] */
		}
		/* Script has been modified since
		** we last stat() it, re-cache */
		else
		{
			/* Pop <mtime>, magnet.cache['<script>'],
			** magnet.cache, and magnet. */
			lua_pop(L, 4);
			if (magnet_cache_script(L, filepath, script.st_mtime))
				return EXIT_FAILURE;
		}
	}
	return EXIT_SUCCESS;
}

int
main(void)
{
	lua_State * const L = luaL_newstate(); 

	/* require() in our
	** Lua libraries */
	luaL_openlibs(L);

	lua_newtable   (L                          ); /* Push a new table.                           */
	lua_newtable   (L                          ); /* Push a new table.                           */
	lua_setfield   (L,       -2,        "cache"); /* <table#1>.cache = <table#2>, pops <table#2> */
	lua_pushinteger(L,        0                ); /* Push 0                                      */
	lua_setfield   (L,       -2, "conns_served"); /* <table#1>.conns_served = 0, pops 0          */
	lua_setglobal  (L, "magnet"                ); /* _G.magnet = <table#1>, pops <table#1>       */

	lua_getglobal(L, "debug"             ); /* Push debug from _G     */
	lua_getfield (L,      -1, "traceback"); /* Push debug.traceback() */
	lua_remove   (L,       1             ); /* Pop  debug             */

	while (FCGI_Accept() >= 0)
	{
		/* For the script's hits counter and the
		** global, requests served counter. */
		lua_Integer tmp;
		const char * const script = getenv("SCRIPT_FILENAME");

		/* debug.traceback() */
		assert(lua_gettop(L) == 1 && lua_isfunction(L, 1));

		/* Couldn't get script-function,
		** response has been sent, skip the rest. */
		if (magnet_get_script(L, script))
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

		/* Any errors generated by pcall() are printed
		** in-place, don't rely on headers having been sent.
		** --------------------------------------------
		** 1 item is always returned, 1 (last arg) is debug.traceback() */
		if (lua_pcall(L, 0, 1, 1))
			fputs(lua_tostring(L, -1), stdout);

		/* Pop pcall() retval */
		lua_pop(L, 1);

		/* First just get the script tables. -.- */
		lua_getglobal      (L, "magnet"         );  /* Push magnet from _G                                  */
		assert(lua_istable (L,       -1         )); /* assert() magnet is a table                           */
		lua_getfield       (L,       -1, "cache");  /* Push magnet.cache                                    */
		assert(lua_istable (L,       -1         )); /* assert() magnet.cache is a table                     */
		lua_getfield       (L,       -1,  script);  /* Push magnet.cache['<script>']                        */
		assert(lua_istable (L,       -1         )); /* assert() magnet.cache['<script>'] is a table         */

		/* Increment the script's hit counter. */
		lua_getfield       (L,       -1,  "hits");  /* Push magnet.cache['<script>'].hits                   */
		tmp = lua_tointeger(L,       -1         );  /* hits = tointeger(^.hits)                             */
		lua_pop            (L,        1         );  /* Pop  magnet.cache['<script>'].hits                   */
		lua_pushinteger    (L,  tmp + 1         );  /* Push (hits + 1)                                      */
		lua_setfield       (L,       -2,  "hits");  /* magnet.cache['<script>'].hits = (hits + 1), pops ^   */
		lua_pop            (L,        2         );  /* Pop script's table, magnet.cache, leave magnet       */

		/* Increment our served connections counter. */
		lua_getfield       (L,      -1, "conns_served"); /* Push magnet.conns_served                        */
		tmp = lua_tointeger(L,      -1                ); /* conns_served = tointeger(magnet.conns_served)   */
		lua_pop            (L,       1                ); /* Pop magnet.conns_served                         */
		lua_pushinteger    (L, tmp + 1                ); /* Push conns_served + 1                           */
		lua_setfield       (L,      -2, "conns_served"); /* magnet.conns_served = (conns_served + 1), pop ^ */
		lua_pop            (L,       1                ); /* Pop magnet                                      */
	}

	lua_close(L);

	return EXIT_SUCCESS;
}
