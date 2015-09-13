# THIS == DEAD

I pushed this to Github to preserve it.  This is a FastCGI Lua server ~thing~ I wrote and worked on a long time ago.

I don't work on it anymore.

This was one of the first things that got me into webdev.  From FastCGI I went to SCGI, and then WSGI, and then I started making the application the webserver (nodejs).  This is where I started.

--------------------------------------------------------------------------------------------------

	Compile:

		gcc   -Wall -O2 -g -o magnet magnet.c -lfcgi -llua -lm -ldl -pedantic -ansi -std=c89
		clang -Wall -O2 -g -o magnet magnet.c -lfcgi -llua -lm -ldl -pedantic -ansi -std=c89

			> I find clang to be so much more descriptive for errors and warnings. <3

--------------------------------------------------------------------------------------------------

	Flags to consider for gcc:
		-flto                 - link-time optimization (feed zeh linkerzzzzz)
		-fomit-frame-pointer  - omit frame pointer
		-fstack-protector-all - enable the stack protector (can't hurt?)

--------------------------------------------------------------------------------------------------

	Environmental options:

		-DDIRLIST               - Enable magnet.dirlist(directory)
		-DGC_COLLECT_AFTER_RUN  - Force a gc collection after script exec. (hurts performance)
		-DPLAINTEXT_ERROR_PAGES - text/plain in the Content-Type for errors, text/html by default

--------------------------------------------------------------------------------------------------

Associate the .lua extension with the `magnet` fastcgi launcher in your web server.

On my webserver I use the spawn-fcgi program (originally from lighttpd?) like so:

	/usr/bin/spawn-fcgi -u httpd -g httpd -s /tmp/randomstring-webserver-lua.sock -f /path/to/lua-magnet

I then set a rule for all requests to files with the .lua extension to communicate
with that fastcgi server through the /tmp/randomstring-webserver-lua.sock socket.

Request: /path/to/whatever.lua

if ($REQUEST_URI =~ m{\.lua$})
{
	--> Query FastCGI server with SCRIPT_FILENAME='/path/to/whatever.lua'
	<-- FastCGI Server Response... (magnet response)
}

*Happy User* :D-S-<

At least, that's my understanding of it... I am *STILL LEARNING* about FastCGI
and backend-oriented webdev, but so far my attempts have worked out pretty well...

Features (so far)
-----------------

	- Caches scripts under _G.magnet.cache['<script>']
	- Keeps track of how many hits each script gets (_G.magnet.cache['<script'].hits)
	- Also exports the mtime for each script (which it uses to re-cache automagically if it has been altered)

	^ I take no credit for these. ^

	- print(...) has been redefined

		Arguments are converted to a string (if necessary) with Lua's tostring(), which will invoke a defined __tostring).
		This does not exactly like the regular print(), it doesn't print intermittent tabs with multiple arguments.

		Example usage:

			local header =
			{
				['Content-Type'] = 'text/plain',
				['Status']       = '200 OK',
			}

			buildheader =
				function (header_desc)
					return table.concat(header_desc) .. '\r\n\r\n'
				end

			print(build_header(header) .. 'Hello World!', '\r\n', 'How', 'are', 'you?\r\n')

		Outputs:

			Content-Type: text/plain
			Status: 200 OK

			Hello World!
			How are you?

	- Improved error-handling?
		Sends the ~correct~ response if the script can't be accessed for interpretation (403 Forbidden, 404 Not Found, 503 Service Unavailable).
		Errors generated when interpreting the script into bytecode are reported (200 OK, Content-Type: text/html, error...)
		Errors generated at runtime are reported in-place with a stack trace with the stock debug.traceback()
	
	- Reports how many connections served through magnet.conns_served
		(conns_served = sum of all scripts' magnet.cache['<script>'].hits)

	- magnet.dirlist(directory) - Enable with -DDIRLIST at compile-time.
		Function for returning the items in a specificed directory, very basic.
		Items are pre-sorted alphabetically. (scandir() + alphasort())

		If `directory' is not a string or is not a directory path
		scandir() fails, return looks like: false, 'error'
		Returns at least:                   { '.', '..' }

		Example return value: { [1] = '.', [2] = '..', [3] = 'a.ext', [4] = 'b.ext', ... }
		
		Example usage:

			-- Could return nil, error
			local str_fmt = string.format
			local t, e = magnet.dirlist('directory')
			local content = ''

			if not t then
			    content = e
			else
			    for i, filename in ipairs(t) do
			        content = content .. str_fmt("%s\t'%s'\n", tostring(i), filename)
			    end
			end

			print(
			    'Content-Type: text/plain\r\n' ..
			    'Status: 200 OK\r\n' ..
			    '\r\n' ..
			    content
			)

	- -DERRPAGE_TYPE=text/html (use at compile-time)
		Error pages are sent as text/plain by default, browsers often
		use their own error pages if the status is not 200 and the type
		is plaintext... You can text text/html or something else to override
		that behavior.

	- -DGC_COLLECT_AFTER_CONNECT (use at compile-time)
		Forces a garbage collection after each script run (the very last thing done)

		Note: This is probably a bad idea since it isn't a gc step and
		probably hurts performance since Lua manages the gc better anyway.

Tip
---

You can check out the interesting CGI environment variables with
os.getenv('REQUEST_URI') or os.getenv('QUERY_STRING'), etc... it's
just up to you to parse said query string.  I've already done
something in pure Lua which you can view at http://partyvan.us/lua/bleh.lua,
the source is icky and viewable with '&highlight' appended: http://partyvan.us/lua/bleh.lua&highlight

List of CGI Environment Variables
---------------------------------

	- 'AUTH_TYPE'
	- 'CONTENT_LENGTH'
	- 'CONTENT_TYPE'
	- 'DOCUMENT_ROOT'
	- 'GATEWAY_INTERFACE'
	- 'HTTP_ACCEPT'
	- 'HTTP_COOKIE'
	- 'HTTP_HOST'
	- 'HTTP_REFERRER'
	- 'HTTP_USER_AGENT'
	- 'HTTPS'
	- 'PATH'
	- 'PATH_INFO'
	- 'PATH_TRANSLATED'
	- 'QUERY_STRING',
	- 'REMOTE_ADDR'
	- 'REMOTE_HOST'
	- 'REMOTE_IDENT'
	- 'REMOTE_USER'
	- 'REQUEST_METHOD'
	- 'REQUEST_URI'
	- 'SCRIPT_NAME'
	- 'SCRIPT_FILENAME'
	- 'SERVER_ADMIN'
	- 'SERVER_NAME'
	- 'SERVER_PORT'
	- 'SERVER_PROTOCOL',
	- 'SERVER_SIGNATURE'
	- 'SERVER_SOFTWARE'

	- Note: Please let me know if there is some standard for all
	that exist, I have checked the original CGI site, but I keep
	finding more standard-yet-non-standard conventional CGI vars
	exported by my webserver.  Just curious what is common.

Roadmap
-------

	DO WANT:
		- Export all the CGI environment variables under magnet.env
			- Find out the standard for parsing the QUERY_STRING if it is defined. -.-
				- I believe ; or & is the (conventional?) record separator... need more info.

	Low Priority: (luaFilesystem works spendidly for now)
		- Add a function for stat()'ing a file.  Return attributes like ^ ?

License
-------

Let's assume my derived work is under the protection of An Hero.
You may do whatever you want with this code and you may not
hold me responsible for any negative affect it has on you,
your posessions, girlfriend, therapist, ...  I would be
amazed if it caused damage to your computer, and if it does
I'll laugh at you.  You have been warned.

Compliments of the Greater Lulz License version 2
