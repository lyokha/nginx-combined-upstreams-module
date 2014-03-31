Nginx Combined Upstreams module
===============================

The module introduces two directives *add_upstream* and
*combine_server_singlets* available inside upstream configuration blocks.

Directive add_upstream
----------------------

Populates the host upstream with servers listed in an already defined upstream
specified by the mandatory 1st parameter of the directive. Optional 2nd
parameter may have only value *backup* which marks all servers of the sourced
upstream as backups.

### An example

```nginx
upstream  combined {
    add_upstream    upstream1;            # src upstream 1
    add_upstream    upstream2;            # src upstream 2
    server          some_another_server;  # if needed
    add_upstream    upstream3 backup;     # src upstream 3
}
```

Directive combine_server_singlets
---------------------------------

Produces multiple *singlet upstreams* from servers so far defined in the host
upstream. A singlet upstream contains only one active server whereas other
servers are marked as backups. If no parameters were passed then the singlet
upstreams will have names of the host upstream appended by the ordering number
of the active server in the host upstream. Optional 2 parameters can be used to
adjust their names. The 1st parameter is a suffix added after the name of the
host upstream and before the ordering number. The 2nd parameter must be an
integer value which defines *zero-alignment* of the ordering number, for example
if it has value 2 then the ordering numbers could be
``'01', '02', ..., '10', ... '100' ...``

### An example:

```nginx
upstream  uhost {
    server                   s1;
    server                   s2;
    server                   s3 backup;
    server                   s4;
    # build singlet upstreams uhost_single_01,
    # uhost_single_02, uhost_single_03 and uhost_single_04
    combine_server_singlets  _single_ 2;
}
```

### Why numbers, not names?

In the example above singlet upstreams will have names like *uhost_single_01*
but names that contain server names like *uhost_single_s1* would look better and
more convenient. Why not use them instead ordering numbers? Unfortunately nginx
does not remember server names after a server has been added into an upstream,
therefore we cannot simply fetch it.

### Where this can be useful

Hmm, I do not know. Anyway a singlet upstream is a prominent category because it
defines a single server with fallback mode. We can use them to provide robust
HTTP session management when backend servers identify themselves using a known
mechanism like HTTP cookies.

```nginx
upstream  uhost {
    server  s1;
    server  s2;
    combine_server_singlets;
}

server {
    listen       8010;
    server_name  main;
    location / {
        proxy_pass http://uhost$cookie_rt;
    }
}
server {
    listen       8020;
    server_name  server1;
    location / {
        add_header Set-Cookie "rt=1";
        echo "Passed to $server_name";
    }
}
server {
    listen       8030;
    server_name  server2;
    location / {
        add_header Set-Cookie "rt=2";
        echo "Passed to $server_name";
    }
}
```

In this configuration the first client request will choose backend server
randomly, the chosen server will set cookie *rt* to a predefined value (*1* or
*2*) and all further requests from this client will be proxied to the chosen
server automatically until it goes down. Say it was *server1*, then when it goes
down the cookie *rt* on the client side will still be *1*. Directive
*proxy_pass* will route the next client request to a singlet upstream *uhost1*
where *server1* is declared active and *server2* is backed up. As soon as
*server1* is not reachable any longer nginx will route the request to *server2*
which will rewrite the cookie *rt* and all further client requests will be
proxied to *server2* until it goes down.

