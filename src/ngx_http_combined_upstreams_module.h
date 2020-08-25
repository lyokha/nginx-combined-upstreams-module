/*
 * =============================================================================
 *
 *       Filename:  ngx_http_combined_upstreams_module.h
 *
 *    Description:  nginx module for building combined upstreams
 *
 *        Version:  2.0
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


typedef struct {
    ngx_array_t  upstrands;
} ngx_http_combined_upstreams_main_conf_t;


typedef struct {
    ngx_array_t  dyn_upstrands;
    ngx_uint_t   upstrand_gw_modules_checked;
} ngx_http_combined_upstreams_loc_conf_t;


extern ngx_module_t  ngx_http_combined_upstreams_module;

#endif /* NGX_HTTP_COMBINED_UPSTREAMS_MODULE_H */

