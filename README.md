Nginx Combined Upstreams module
===============================

The module introduces three directives *add_upstream*,
*combine_server_singlets*, and *extend_single_peers* available inside upstream
configuration blocks, and a new configuration block *upstrand* for building
super-layers of upstreams. Additionally, directive *dynamic_upstrand* is
introduced for choosing upstrands in run-time.

Table of contents
-----------------

- [Directive add_upstream](#directive-add_upstream)
- [Directive combine_server_singlets](#directive-combine_server_singlets)
- [Directive extend_single_peers](#directive-extend_single_peers)
- [Block upstrand](#block-upstrand)
- [Directive dynamic_upstrand](#directive-dynamic_upstrand)
- [See also](#see-also)

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

### An example

```nginx
upstream  uhost {
    server                   s1;
    server                   s2;
    server                   s3 backup;
    server                   s4;
    # build singlet upstreams uhost_single_01,
    # uhost_single_02, uhost_single_03 and uhost_single_04
    combine_server_singlets  _single_ 2;
    server                   s5;
}
```

### Why numbers, not names?

In the example above singlet upstreams will have names like *uhost_single_01*,
but names that contain server names like *uhost_single_s1* would look better and
more convenient. Why not use them instead ordering numbers? Unfortunately nginx
does not remember server names after a server has been added into an upstream,
therefore we cannot simply fetch them.

*Update.* There is a good news! Since version *1.7.2* nginx remembers server
names in upstream data and now we can use them when referring to a special
keyword *byname*. For example

```nginx
    combine_server_singlets  byname;
    # or
    combine_server_singlets  _single_ byname;
```

All colons (*:*) in the server names are replaced with underscores (*_*).

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

Directive extend_single_peers
-----------------------------

Peers in upstreams fail according to the rules listed in directive
*proxy_next_upstream*. If an upstream has only one peer in its main or backup
part then this peer will never fail. This can be a serious problem when writing
a custom algorithm for active health checks of upstream peers. Directive
*extend_single_peers*, being declared in an upstream block, adds a fake peer
marked as *down* in the main or the backup part of the upstream if the part
originally contains only one peer. This makes nginx mark the original single
peer as failed when it fails to pass the rules of *proxy_next_upstream* just
like in general case of multiple peers.

### An example

```nginx
upstream  upstream1 {
    server  s1;
    extend_single_peers;
}

upstream  upstream2 {
    server  s1;
    server  s2;
    server  s3 backup;
    extend_single_peers;
}
```

Notice that if a part (the main or the backup) of an upstream contains more than
one peer (like the main part in *upstream2* from the example) then the directive
has no effect: particularly, in the *upstream2* it only affects the backup part
of the upstream.

Block upstrand
--------------

Is aimed to configure a super-layer of upstreams that do not lose their
identities. Accepts a number of directives including *upstream*, *order*,
*next_upstream_statuses* and others. Upstreams with names starting with tilde
(*~*) match a regular expression. Only upstreams that already have been declared
before the upstrand block definition are regarded as candidates.

### An example

```nginx
upstrand us1 {
    upstream ~^u0 blacklist_interval=60s;
    upstream b01 backup;
    order start_random;
    next_upstream_statuses non_idempotent 204 5xx;
    next_upstream_timeout 60s;
    #intercept_errors;
}
```

Upstrand *us1* will combine all upstreams whose names start with *u0* and
upstream *b01* as backup. Backup upstreams are checked if all normal upstreams
fail. The *failure* means that all upstreams in normal or backup cycles have
responded with statuses listed in directive *next_upstream_statuses* or were
*blacklisted*. An upstream is set as blacklisted when it has parameter
*blacklist_interval* and responded with a status listed in the
*next_upstream_statuses*. Blacklisting state is not shared between nginx worker
processes.

The next four upstrand directives are akin to those from the nginx proxy module.

Directive *next_upstream_statuses* accepts *4xx* and *5xx* statuses notation and
values *error* and *timeout* to distinguish between cases when errors happen
with the upstream's peer connection from those when backends send statuses *502*
or *504* (plain values *502* and *504* refer to the both cases). It also accepts
value *non_idempotent* to allow further processing of *non-idempotent* requests
when they were responded by a server from an upstream but failed according to
other statuses listed in the directive. Requests are considered to be
non-idempotent when their methods are *POST*, *LOCK* or *PATCH* as in the common
nginx sense.

Directive *next_upstream_timeout* limits the overall duration time the upstrand
cycles through all of its upstreams. If the time elapses while the upstrand is
ready to pass to a next upstream, the last upstream cycle result is returned.

Directive *intercept_errors* (commented out in the example) allows intercepting
the final response by the *error_page*.

Directive *order* currently accepts only one value *start_random* which means
that starting upstreams in normal and backup cycles after worker fired up will
be chosen randomly. Starting upstreams in further requests will be cycled in
round-robin manner. Additionally, a modifier *per_request* is also accepted in
the *order* directive: it turns off the global per-worker round-robin cycle.
The combination of *per_request* and *start_random* makes the starting upstream
in every new request be chosen randomly.

Such a failover between *failure* statuses can be reached during a single
request by feeding a special variable that starts with *upstrand_* to the
*proxy_pass* directive like so:

```nginx
location /us1 {
    proxy_pass http://$upstrand_us1;
}
```

But be careful when accessing this variable from other directives! It starts up
the subrequests machinery which may be not desirable in many cases.

### Upstrand status variables

There are a number of upstrand status variables available: *upstrand_addr*,
*upstrand_cache_status*, *upstrand_connect_time*, *upstrand_header_time*,
*upstrand_response_length*, *upstrand_response_time* and *upstrand_status*. They
all are counterparts of corresponding *upstream* variables and contain the
values of the latter for all upstreams passed through a request and all
subrequests chronologically. Variable *upstrand_path* contains path of all
upstreams visited during request.

### Where this can be useful

The *upstrand* looks very similar to a simple combined upstream but it also has
a crucial difference: the upstreams inside of an upstrand do not get flattened
and keep holding their identities. This gives a possibility to configure a
*failover* status for a group of servers associated with a single upstream
without need to check them all by turn. In the above example upstrand *us1* may
hold a list of upstreams like *u01*, *u02* etc. Imagine that upstream *u01*
holds 10 servers inside and represents a part of a geographically distributed
backend system. Let upstrand *us1* combine all those geographical parts and we
have an application that polls the parts for doing some tasks. Let backends send
HTTP status *204* if they do not have new tasks. In a flat combined upstream all
10 servers may be polled before the application will finally receive a new task
from another upstream. The upstrand *us1* allows skipping to the next upstream
after checking the first server in an upstream that does not have tasks. This
machinery is apparently suitable for *upstreams broadcasting*, when messages are
being sent to all upstreams in an upstrand.

The examples above show that an upstrand can be regarded as a *2-dimensional*
upstream that comprises a number of clusters representing natural upstreams and
allows short-cycling over them.

Directive dynamic_upstrand
--------------------------

Allows choosing an upstrand from passed variables in run-time. The directive can
be set in server, location and location-if clauses.

In the following configuration

```nginx
    upstrand us1 {
        upstream ~^u0;
        upstream b01 backup;
        order start_random;
        next_upstream_statuses 5xx;
    }
    upstrand us2 {
        upstream ~^u0;
        upstream b02 backup;
        order start_random;
        next_upstream_statuses 5xx;
    }

    server {
        listen       8010;
        server_name  main;

        dynamic_upstrand $dus1 $arg_a us2;

        location / {
            dynamic_upstrand $dus2 $arg_b;
            if ($arg_b) {
                proxy_pass http://$dus2;
                break;
            }
            proxy_pass http://$dus1;
        }
    }
```

upstrands returned in variables *dus1* and *dus2* are to be chosen from values
of variables *arg_a* and *arg_b*. If *arg_b* is set then the client request will
be sent to an upstrand with name equal to the value of *arg_b*. If there is not
an upstrand with this name then *dus2* will be empty and *proxy_pass* will
return HTTP status *500*. To prevent initialization of a dynamic upstrand
variable with empty value, its declaration must be terminated with a literal
name that corresponds to an existing upstrand. In this example dynamic upstrand
variable *dus1* will be initialized by the upstrand *us2* if *arg_a* is empty or
not set. Altogether if *arg_b* is not set or empty and *arg_a* is set and has a
value equal to an existing upstrand, the request will be sent to this upstrand,
otherwise (if *arg_b* is not set or empty and *arg_a* is set but does not refer
to an existing upstrand) *proxy_pass* will most likely return HTTP status *500*
(except there is a variable composed from literal string *upstrand_* and the
value of *arg_a* that points to a valid destination), otherwise (both *arg_b*
and *arg_a* are not set or empty) the request will be sent to the upstrand
*us2*.

See also
--------

There are several articles about the module in my blog, in chronological order:

1. [*Простой модуль nginx для создания комбинированных
апстримов*](http://lin-techdet.blogspot.com/2011/10/nginx.html) (in Russian). A
comprehensive article discovering details of implementation of directive
*add_upstream* which can also be regarded as a small tutorial for nginx modules
development.
2. [*nginx upstrand to configure super-layers of
upstreams*](http://lin-techdet.blogspot.com/2015/09/nginx-upstrand-to-configure-super.html).
An overview of block *upstrand* usage and some details on its implementation.
3. [*Не такой уж простой модуль nginx для создания комбинированных
апстримов*](http://lin-techdet.blogspot.com/2015/12/nginx.html) (in Russian). An
overview of all features of the module with configuration examples and testing
session samples.

