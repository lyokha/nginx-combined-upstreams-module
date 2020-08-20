# vi:filetype=

use Test::Nginx::Socket;

repeat_each(1);
plan tests => repeat_each() * (2 * 3 * blocks());

no_shuffle();
run_tests();

__DATA__

=== TEST 1: upstrand timeout
--- http_config
    upstream u01 {
        server localhost:8040;
    }
    upstream u02 {
        server localhost:8050;
    }
    upstream b01 {
        server localhost:8060;
    }

    upstrand us1 {
        upstream ~^u0 blacklist_interval=60s;
        upstream b01 backup;
        next_upstream_statuses error timeout 5xx;
        next_upstream_timeout 1s;
        intercept_statuses 5xx /Internal/failover;
    }

    server {
        listen       8040;
        server_name  backend01;

        location / {
            echo_sleep 3;
            echo_status 504;
            echo "In 8040";
        }
    }
    server {
        listen       8050;
        server_name  backend02;

        location / {
            echo_sleep 3;
            echo_status 504;
            echo "In 8050";
        }
    }
    server {
        listen       8060;
        server_name  backend03;

        location / {
            add_header Upstrand-Server 8060;
            echo "In 8060";
        }
    }
--- config
        location /us1 {
            proxy_pass http://$upstrand_us1;
        }

        location /Internal/failover {
            internal;
            echo_status 503;
            echo Failover;
        }
--- request eval
["GET /us1", "GET /us1", "GET /us1"]
--- timeout: 10s
--- response_body eval
["Failover\n", "Failover\n", "In 8060\n"]
--- error_code eval: [503, 503, 200]

