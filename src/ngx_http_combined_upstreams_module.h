/*
 * =============================================================================
 *
 *       Filename:  ngx_http_combined_upstreams_module.h
 *
 *    Description:  nginx module for building combined upstreams
 *
 *        Version:  2.3
 *        Created:  13.08.2020 16:57:22
 *
 *         Author:  Alexey Radkov (), 
 *        Company:  
 *
 * =============================================================================
 */

#ifndef NGX_HTTP_COMBINED_UPSTREAMS_MODULE_H
#define NGX_HTTP_COMBINED_UPSTREAMS_MODULE_H

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#ifdef NGX_HTTP_COMBINED_UPSTREAMS_PERSISTENT_UPSTRAND_INTERCEPT_CTX
#include "ngx_easy_context.h"
#endif


typedef struct {
    ngx_array_t                 upstrands;
#ifdef NGX_HTTP_COMBINED_UPSTREAMS_PERSISTENT_UPSTRAND_INTERCEPT_CTX
    ngx_http_easy_ctx_handle_t  upstrand_intercept_ctx;
#endif
} ngx_http_combined_upstreams_main_conf_t;


typedef struct {
    ngx_array_t                 dyn_upstrands;
    ngx_uint_t                  upstrand_gw_modules_checked;
} ngx_http_combined_upstreams_loc_conf_t;


extern ngx_module_t  ngx_http_combined_upstreams_module;

#endif /* NGX_HTTP_COMBINED_UPSTREAMS_MODULE_H */

