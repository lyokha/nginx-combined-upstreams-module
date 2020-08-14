/*
 * =============================================================================
 *
 *       Filename:  ngx_http_combined_upstreams_upstrand.h
 *
 *    Description:  upstrand is a super-layer of nginx upstreams
 *
 *        Version:  2.0
 *        Created:  13.08.2020 16:58:09
 *
 *         Author:  Alexey Radkov (), 
 *        Company:  
 *
 * =============================================================================
 */

#ifndef NGX_HTTP_COMBINED_UPSTREAMS_UPSTRAND_H
#define NGX_HTTP_COMBINED_UPSTREAMS_UPSTRAND_H

#include <ngx_core.h>
#include <ngx_http.h>


typedef enum {
    ngx_http_upstrand_order_normal = 0,
    ngx_http_upstrand_order_start_random
} ngx_http_upstrand_order_e;


typedef struct {
    ngx_str_t                  name;
    ngx_array_t                upstreams;
    ngx_array_t                b_upstreams;
    ngx_array_t                next_upstream_statuses;
    ngx_msec_t                 next_upstream_timeout;
    ngx_int_t                  cur;
    ngx_int_t                  b_cur;
    ngx_http_upstrand_order_e  order;
    ngx_uint_t                 order_per_request:1;
    ngx_uint_t                 intercept_errors:1;
    ngx_uint_t                 retry_non_idempotent:1;
    ngx_uint_t                 debug_intermediate_stages:1;
} ngx_http_upstrand_conf_t;


typedef struct {
    ngx_array_t  data;
    ngx_int_t    index;
} ngx_http_upstrand_var_list_elem_t;


ngx_int_t ngx_http_upstrand_init(ngx_conf_t *cf);
char *ngx_http_dynamic_upstrand(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
ngx_int_t ngx_http_get_upstrand_path_var_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
ngx_int_t ngx_http_get_upstrand_status_var_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
char *ngx_http_upstrand_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

#endif /* NGX_HTTP_COMBINED_UPSTREAMS_UPSTRAND_H */

