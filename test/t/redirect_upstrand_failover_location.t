# vi:filetype=

use Test::Nginx::Socket;

repeat_each(2);
plan tests => repeat_each() * (2 * blocks());

no_shuffle();
run_tests();

__DATA__

=== TEST 1: redirect failover location
--- http_config
    upstream u01 {
        server localhost:8040;
    }
    upstream u02 {
        server localhost:8050;
    }

    upstrand us1 {
        upstream ~^u0;
        order start_random;
        next_upstream_statuses 5xx;
        intercept_statuses 5xx /Internal/failover;
    }

    proxy_read_timeout 5s;
    proxy_intercept_errors on;

    server {
        listen       8040;
        server_name  backend01;

        location / {
            return 503;
        }
    }
    server {
        listen       8050;
        server_name  backend02;

        location / {
            return 503;
        }
    }
--- config
        error_page 503 =200 /Internal/error;

        location /us1 {
            proxy_pass http://$upstrand_us1;
        }

        location /Internal/error {
            internal;
            echo "Caught by error_page";
        }
        location /Internal/redirect {
            internal;
            echo "Redirected by try_files";
        }
        location /Internal/failover {
            internal;
            try_files /unknown.html /Internal/redirect;
        }
--- request
GET /us1
--- response_body
Redirected by try_files
--- error_code: 200

