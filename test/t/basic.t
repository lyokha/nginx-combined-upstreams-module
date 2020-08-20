# vi:filetype=

use Test::Nginx::Socket;

repeat_each(2);
plan tests => repeat_each() * (2 * blocks());

no_shuffle();
run_tests();

__DATA__

=== TEST 1: combined upstreams rotation
--- http_config
    upstream u1 {
        server localhost:8020;
    }
    upstream u2 {
        server localhost:8030;
    }
    upstream ucombined {
        server localhost:8030;
        add_upstream u1;
        add_upstream u2 backup;
    }
    upstream u3 {
        server localhost:8020;
        server localhost:8030;
        combine_server_singlets;
        combine_server_singlets byname;
        combine_server_singlets _tmp_ 2;
    }

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
        order start_random;
        next_upstream_statuses error timeout non_idempotent 204 5xx;
        next_upstream_timeout 60s;
    }
    upstrand us2 {
        upstream ~^u0;
        order start_random;
        next_upstream_statuses error timeout 5xx;
    }
    upstrand us3 {
        upstream ~^u0;
        order start_random;
        next_upstream_statuses error timeout 5xx;
        intercept_statuses 5xx /Internal/failover;
    }

    proxy_read_timeout 5s;
    proxy_intercept_errors on;

    server {
        listen       8020;
        server_name  backend1;
        location / {
            add_header Set-Cookie "rt=1";
            echo "Passed to $server_name";
        }
    }
    server {
        listen       8030;
        server_name  backend2;
        location / {
            add_header Set-Cookie "rt=2";
            echo "Passed to $server_name";
        }
    }
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
    server {
        listen       8060;
        server_name  backend03;

        location / {
            add_header Upstrand-Server 8060;
            echo "In 8060";
        }
    }
--- config
        error_page 503 =200 /error;

        dynamic_upstrand $dus1 $arg_a us2;

        location /cmb {
            proxy_pass http://ucombined;
        }
        location /cmb1 {
            proxy_pass http://u31;
        }
        location /cmb2 {
            proxy_pass http://u3_tmp_02;
        }
        location /cmb3 {
            proxy_pass http://u3$cookie_rt;
        }

        location /us1 {
            proxy_pass http://$upstrand_us1;
        }
        location /us2 {
            rewrite ^ /index.html last;
        }
        location /index.html {
            proxy_buffering off;
            proxy_pass http://$upstrand_us2;
        }
        location /us3 {
            proxy_pass http://$upstrand_us3;
        }
        location /echo/us1 {
            echo $upstrand_us1;
        }
        location /dus1 {
            dynamic_upstrand $dus2 $arg_b;
            if ($arg_b) {
                proxy_pass http://$dus2;
                break;
            }
            proxy_pass http://$dus1;
        }
        location /echo/dus1 {
            echo $dus1;
        }
        location /error {
            echo "Caught by error_page";
        }

        location /zus1 {
            gzip on;
            gzip_min_length 4;
            gzip_types *;
            proxy_pass http://$upstrand_us1;
        }

        location /Internal/failover {
            internal;
            echo_status 503;
            echo Failover;
        }
--- request
GET /cmb
--- response_body_like
Passed to backend[12]$
--- error_code: 200

=== TEST 2: combined upstreams singlets 1
--- request
GET /cmb1
--- response_body
Passed to backend1
--- error_code: 200

=== TEST 3: combined upstreams singlets 2
--- request
GET /cmb2
--- response_body
Passed to backend2
--- error_code: 200

=== TEST 4: combined upstreams singlets by cookie 1
--- more_headers
Cookie: rt=1
--- request
GET /cmb3
--- response_body
Passed to backend1
--- error_code: 200

=== TEST 5: combined upstreams singlets bu cookie 2
--- more_headers
Cookie: rt=2
--- request
GET /cmb3
--- response_body
Passed to backend2
--- error_code: 200

=== TEST 6: upstrand last returned alive
--- request
GET /us1
--- response_body
In 8060
--- error_code: 200

=== TEST 7: upstrand last returned no live
--- request
GET /us2
--- response_body_filters eval
sub {
    my ($text) = @_;
    my @lines = split /\r\n/, $text;
    my @matches = grep /503 Service Temporarily Unavailable/, @lines;
    scalar @matches ? "FOUND" : "NOT FOUND";
}
--- response_body chomp
FOUND
--- error_code: 503

=== TEST 8: upstrand intercepted
--- request
GET /us3
--- response_body
Failover
--- error_code: 503

=== TEST 9: upstrand variable echo
--- request
GET /echo/us1
--- response_body
b01
--- error_code: 200

=== TEST 10: dynamic upstrand returned alive
--- request
GET /dus1?b=us1
--- response_body
In 8060
--- error_code: 200

=== TEST 11: dynamic upstrand failover
--- request
GET /dus1?b=us3
--- response_body
Failover
--- error_code: 503

=== TEST 12: upstrand zipped response
--- more_headers
Accept-Encoding: gzip
--- request
GET /zus1
--- response_body_filters eval
use IO::Uncompress::Gunzip qw (gunzip);
sub {
    my ($text) = @_;
    my $res;
    gunzip \$text => \$res or die $!;
    $res;
}
--- response_body
In 8060
--- error_code: 200
