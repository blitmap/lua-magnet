/* Compile:
**     + gcc   -o magnet{,.c} -W -Wall -O2 -g -lfcgi -llua -lm -ldl -pedantic -ansi -std=c89 -flto -fstack-protector-all
**     + clang -o magnet{,.c} -W -Wall -O2 -g -lfcgi -llua -lm -ldl -pedantic -ansi -std=c89
**   Notes:
**     + clang does not accept -flto for link-time optimization
**     + Enable the directory listing function with -DDIRLIST
*/

/* {{{1 Header Includes */


/* {{{2 Header conditional for DIRLIST */

#if DIRLIST
#	if _BSD_SOURCE
#	else
#		define _BSD_SOURCE
#	endif
#	include <string.h>  /* strerror() */
#	include <dirent.h>  /* scandir(), alphasort(), ... */
typedef struct dirent dirent_t;
#endif

/* }}} */

#if _BSD_SOURCE
#else
#	define _BSD_SOURCE
#endif

#include <assert.h>      /* assert() -- *duh*                  */
#include <stdio.h>       /* fwrite(), fprintf(), fputs(), ...  */
#include <stdlib.h>      /* EXIT_SUCCESS, EXIT_FAILURE         */
#include <string.h>      /* strdup(), strtok()                 */
#include <sys/stat.h>    /* stat()                             */
#include <fcgi_stdio.h>  /* FCGI_Accept()                      */
#include <errno.h>       /* int errno (used with stat() later) */
#include <lualib.h>      /* LUA'Y STUFF :D -S-<                */
#include <lauxlib.h>


/* }}} */

/* {{{1 Macros */

/* For getting the length of an array (obvious) */
#define LEN(array) (sizeof(array) / sizeof((array)[0]))

/* Literal array write */
#define LA_WRITE(array, fd) \
	fwrite(array, sizeof((array)[0]), LEN(array), fd)

/* LA_FCGIOUT("Hello thar")
** `--> LA_WRITE("Hello thar", FCGI_stdout)
**     `--> fwrite("Hello thar", 1, 11, FCGI_stdout) */
#define LA_FCGIOUT(array) LA_WRITE(array, FCGI_stdout)

/* LA_FCGIERR("Hello thar")
** `--> LA_WRITE("Hello thar", FCGI_stderr)
**     `--> fwrite("Hello thar", 1, 11, FCGI_stderr) */
#define LA_FCGIERR(array) LA_WRITE(array, FCGI_stderr)

/* Write a literal page with the provided mime type,
** status and message (reduces to one fwrite()) */
#define LA_PAGEOUT(type, status, message) \
	LA_FCGIOUT                                          \
	(                                                   \
		"Content-Type: "   type                  "\r\n" \
		"Status: "         status                "\r\n" \
												 "\r\n" \
		message                                         \
	)

/* Some browsers like to use their own error pages,
** this seems to only be if Content-Type == text/plain.
** You can define ERRPAGE_TYPE to whatever mime type you
** want or it will obviously use text/plain */
#if ERRPAGE_TYPE
#else
#	define ERRPAGE_TYPE "text/plain"
#endif

/*
** LA_PAGEERR("404 Not Found")
** `--> LA_PAGEOUT(ERR_TYPE, "404 Not Found", "404 Not Found")
**     `--> Content-Type: text/html\r\n
**          Status: 404 Not Found\r\n
**          Content-Length: 13\r\n
**          \r\n
**          404 Not Found
*/
#define LA_PAGEERR(status) \
	LA_PAGEOUT(ERRPAGE_TYPE, status, status)

/* }}} */

/* {{{1 File-scoped "Globals" */

static int        tostring_ref = LUA_NOREF,
           debug_traceback_ref = LUA_NOREF;

/* }}} */

/* {{{1 Helper C-Side Functions */

/*
** static int ref = LUA_REFNIL;
** if (LUA_REFNIL != reference_in_registry(L, &ref, "string.gmatch"))
**     puts("string.gmatch() is now referenced in the Lua C registry...");
** Later: lua_rawgeti(L, LUA_REGISTRYINDEX, ref); Pushed the referenced item...
*/
static int
reference_in_registry(lua_State * const L, int * const ref, const char * const findme)
{
    int n;
	char *token, *str;
    const char seps[] = ".[]\"\'";

    *ref = LUA_NOREF;

    if (findme == NULL || '\0' == *findme)
        return (*ref);

    str = strdup(findme);

	if (NULL == str)
		return (*ref);

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

    free(str);
    lua_pop(L, n);
    return (*ref);
}

/* }}} */

/* {{{1 Explosed C functions */

/* {{{2 static int magnet_print(lua_State * const L) */

/* magnet_print() becomes the underlying C function for print(),
** it is equivalent to io.stdout:write(...) except that it
** calls tostring() on each arg, possibly invoking a __tostring metamethod */
static int
magnet_print(register lua_State * const L)
{
	const size_t nargs = lua_gettop(L);

	if (0 != nargs)
	{
		const char *s;
		size_t i, s_sz;

		for (i = 1; i <= nargs; i++)
		{
			lua_rawgeti      (L, LUA_REGISTRYINDEX, tostring_ref); /* Push tostring()                      */
			lua_pushvalue    (L,                 i              ); /* Push argument                        */
			lua_call         (L,                 1,            1); /* tostring(argument), take 1, return 1 */
			s = lua_tolstring(L,                -1,       &s_sz); /* Fetch result.                        */
			
			if (NULL == s)
				return luaL_error(L, LUA_QL("tostring") " must return a string to " LUA_QL("print"));

			FCGI_fwrite((char *) s, 1, s_sz, FCGI_stdout); /* sizeof(char) is always 1 (standard) */
			lua_pop(L, 1);                                 /* Pop result from tostring(argument)  */
		}
	}

	/* Return nothing on the -Lua- stack. */
	return 0;
}

/* }}} */

/* {{{2 static int magnet_dirlist(lua_State * const L) */

#if DIRLIST

/* Eventually I need to put a stat()-like function in here.... */

/* Example: dirlist('/home') Returns: a table of contents as strings
** I tried to follow the nil, 'error' convention on failure... */
static int
magnet_dirlist(register lua_State * const L)
{
	if (lua_gettop(L) != 1)
	{
		/* return nil, '"dirlist" must receive a string' */
		lua_pushnil(L);
		lua_pushstring(L, LUA_QL("dirlist") " must receive a string");
		return 2;
	}
	else
	{
		size_t s_sz;
		const char * const s = luaL_checkstring(L, 1);
		if (s == NULL)
		{
			/* return nil, '"tostring" must return a string to "dirlist"' */
			lua_pushnil(L);
			lua_pushstring(L, LUA_QL("tostring") " must return a string to " LUA_QL("dirlist"));
			return 2;
		}
		else
		{
			dirent_t **namelist;
			int n = scandir(s, &namelist, 0, alphasort);

			if (n < 0)
			{
				lua_pushnil(L);
				lua_pushstring(L, strerror(errno));
				return 2;
			}
			else
			{
				/* Create our result table. */
				lua_newtable(L);

				/* Creating the array backwards but this is okay since ipairs()
				** only uses the numerical key index to iterate forwards and
				** pairs() is order-unspecified */
				for (; n != 0; n--)
				{
					lua_pushnumber(L, ((lua_Number) n) + 1); /* Push key, + 1 because Lua arrays start at 1 */
					lua_pushstring(L,  namelist[n]->d_name); /* Push name of item.                          */
					lua_settable  (L,                   -3); /* t.key = value; pop string and number        */
					free(namelist[n]);                       /* Free as we go.... :o)                       */
				}
				free(namelist);

				/* Table is on top */
				return 1;
			}
		}
	}
}

#endif

/* }}} */

/* }}} */

/* {{{1 static int magnet_cache_script(lua_State * const L, const char * const filepath, const time_t mtime) */

static int
magnet_cache_script(register lua_State * const L, const char * const filepath, const time_t mtime)
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
			case LUA_ERRFILE: LA_PAGEERR("403 Forbidden"          ); break;
			case LUA_ERRMEM:  LA_PAGEERR("503 Service Unavailable"); break;
			case LUA_ERRSYNTAX:
				fprintf
				(
					FCGI_stdout,
					"Content-Type: text/plain" "\r\n"
					"Status: 200 OK"           "\r\n"
					                           "\r\n"
					"%s"                       "\r\n",
					lua_tostring(L, -1)
				);
				break;
		}
		/* Pop luaL_loadfile()'s error message. */
		lua_pop(L, 1);
		return EXIT_FAILURE;
	}

	/* Make sure loadfile() pushed a function */
	assert(1 == lua_gettop(L));
	assert(lua_isfunction(L, -1));

	lua_getglobal  (L, "magnet"          ); /* Push magnet from _G                           */
	lua_getfield   (L,       -1,  "cache"); /* Push magnet.cache from _G                     */
	lua_newtable   (L                    ); /* Push a new table                              */
	lua_pushvalue  (L,       -4          ); /* Push the loadfile() function (again)          */
	lua_setfield   (L,       -2, "script"); /* <table>.script = <function>, pops <function>  */
	lua_pushinteger(L,    mtime          ); /* Push mtime                                    */
	lua_setfield   (L,       -2,  "mtime"); /* <table>.mtime = <mtime>, pops <mtime>         */
	lua_pushinteger(L,        0          ); /* Push 0 (beginning value for ~script~.hits     */
	lua_setfield   (L,       -2,   "hits"); /* <table>.hits = 0, pops the 0                  */
	lua_setfield   (L,       -2, filepath); /* magnet.cache.~script~ = <table>, pops <table> */
	lua_pop        (L,        2          ); /* Pops magnet and magnet.cache                  */

	/* The result of loadfile() should be
	** the only thing on the stack. */
	assert(1 == lua_gettop(L));
	assert(lua_isfunction(L, 1));

	return EXIT_SUCCESS;
}

/* }}} */

/* {{{1 static int magnet_get_script(lua_State * const L, const char * const filepath) */

static int
magnet_get_script(register lua_State * const L, const char * const filepath)
{
	struct stat script;

	if (filepath == NULL)
	{
		LA_PAGEERR("400 Bad Request");
		return EXIT_FAILURE;
	}

	if (stat(filepath, &script) == -1)
	{
		switch (errno)
		{
			case EACCES: LA_PAGEERR("403 Forbidden"            ); break;
			case ENOENT: LA_PAGEERR("404 Not Found"            ); break;
			default:     LA_PAGEERR("500 Internal Server Error"); break;
		}
		return EXIT_FAILURE;
	}

	/* Not sure why one would SCRIPT_FILENAME='somedirectory/' -- symlink?
	** but let's cover our bases anyway. I believe loadfile()
	** should be possible with anything *but* a directory...? */
	if (S_ISDIR(script.st_mode))
	{
		LA_PAGEOUT(ERRPAGE_TYPE, "200 OK", "`SCRIPT_FILENAME' references a directory");
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
		** we last stat()'d it, re-cache */
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

/* }}} */

/* {{{1 int main(void) */

int
main(void)
{
	register lua_State * const L = luaL_newstate();

	luaL_openlibs(L);

	/* Set our references for later. */
	reference_in_registry(L, &tostring_ref,        "tostring"       );
	assert(LUA_REFNIL != tostring_ref       );
	reference_in_registry(L, &debug_traceback_ref, "debug.traceback");
	assert(LUA_REFNIL != debug_traceback_ref);

	lua_newtable     (L                                ); /* Push a new table.                           */
	lua_newtable     (L                                ); /* Push a new table.                           */
	lua_setfield     (L,             -2,        "cache"); /* <table#1>.cache = <table#2>, pops <table#2> */
	lua_pushinteger  (L,              0                ); /* Push 0                                      */
	lua_setfield     (L,             -2, "conns_served"); /* <table#1>.conns_served = 0, pops 0          */
#if DIRLIST
	lua_pushcfunction(L, magnet_dirlist                ); /* Push magnet_dirlist()                       */
	lua_setfield     (L,             -2,      "dirlist"); /* magnet.dirlist(), pops cfunction            */
#endif
	lua_setglobal    (L,       "magnet"                ); /* _G.magnet = <table#1>, pops <table#1>       */

	while (FCGI_Accept() >= 0)
	{
		/* For the script's hits counter and the
		** global, requests served counter. */
		lua_Number tmp;
		const char * const script = getenv("SCRIPT_FILENAME");

		/* Should be nothing on the stack. */
		assert(0 == lua_gettop(L));

		/* Couldn't get script-function,
		** response has been sent, skip the rest. */
		if (magnet_get_script(L, script))
			continue;

		lua_newtable     (L                             ); /* Will become the env for our script-function         */
		lua_pushcfunction(L,     magnet_print           ); /* Push static int magnet_print(lua_State * const L)   */
		lua_setfield     (L,               -2,   "print"); /* <table>.print() = magnet_print()                    */
		lua_newtable     (L                             ); /* Push a new table.                                   */
		lua_pushvalue    (L, LUA_GLOBALSINDEX           ); /* Push _G                                             */
		lua_setfield     (L,               -2, "__index"); /* <table#2>.__index = _G                              */
		lua_setmetatable (L,               -2           ); /* setmetatable(<table#1>, <table#2>)                  */
		lua_setfenv      (L,               -2           ); /* setfenv(<script-function>, <table#1>) (modded env)  */

		/* script-function on stack */
		assert(1 == lua_gettop(L));

		/* Push the traceback() reference from the registry */
		lua_rawgeti(L, LUA_REGISTRYINDEX, debug_traceback_ref);
		assert(lua_isfunction(L, -1));

		/* Basically swap the traceback()
		** ref and the script-function */
		lua_insert(L, 1);

		/* Any errors generated by pcall() are printed
		** in-place, don't rely on HTTP headers having been sent. */
		if (lua_pcall(L, 0, 0, 1))
		{
			assert(lua_isstring(L, -1));
			fprintf
			(
				FCGI_stdout,
				"Content-Type: text/plain" "\r\n"
				"Status: 200 OK"           "\r\n"
				                           "\r\n"
				"%s"                       "\r\n",
				lua_tostring(L, -1)
			);
			lua_pop(L, 1);
		}

		/* pcall() pops the script-function
		** this pops the traceback() ref */
		lua_pop(L, 1);

		assert(0 == lua_gettop(L));

		/* First just get the script tables. -.- */
		lua_getglobal      (L, "magnet"         );  /* Push magnet from _G                                  */
		assert(lua_istable (L,       -1         )); /* assert() magnet is a table                           */
		lua_getfield       (L,       -1, "cache");  /* Push magnet.cache                                    */
		assert(lua_istable (L,       -1         )); /* assert() magnet.cache is a table                     */
		lua_getfield       (L,       -1,  script);  /* Push magnet.cache['<script>']                        */
		assert(lua_istable (L,       -1         )); /* assert() magnet.cache['<script>'] is a table         */

/* Gets the field (lua_Number) of the table on the top
** of the lua stack, increments it and re-sets it. */
#define _F_MAIN_INCREMENT_LUA_ACCUMULATOR(lua_state, accumulator, lua_num) \
		do                                                                                                                              \
		{                                                                                                                               \
			lua_getfield          (lua_state,          -1, accumulator); /* Push "accumulator" field from table on top of lua state. */ \
			lua_num = lua_tonumber(lua_state,          -1             ); /* lua_num = tonumber(accumulator)                          */ \
			lua_pop               (lua_state,           1             ); /* Pop numberized accumulator                               */ \
			lua_pushnumber        (lua_state, lua_num + 1             ); /* Push (lua_num + 1)                                       */ \
			lua_setfield          (lua_state,          -2, accumulator); /* -1 (table).accmulator = (hits + 1), pops (hits + 1)      */ \
		} while (0)

		/* Stack: table(magnet), table(magnet.cache), table(<script>) */
		_F_MAIN_INCREMENT_LUA_ACCUMULATOR(L,         "hits", tmp);
		lua_pop(L, 2); /* Pop table(script), table(magnet.cache)      */
		_F_MAIN_INCREMENT_LUA_ACCUMULATOR(L, "conns_served", tmp);
		lua_pop(L, 1); /* Pop table(magnet)                           */
#undef _F_MAIN_INCREMENT_LUA_ACCUMULATOR

/* If you want to be strict about memory;
** hurts web servers with many requests since it's
** a full collection and not a step. */
#if GC_COLLECT_AFTER_CONNECT
		lua_gc(L, LUA_GCCOLLECT, 0);
#endif
	}

	lua_close(L);
	return EXIT_SUCCESS;
}

/* }}} */
