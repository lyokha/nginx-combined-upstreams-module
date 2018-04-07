/*
 * =============================================================================
 *
 *       Filename:  ngx_http_combined_upstreams_module.c
 *
 *    Description:  nginx module for building combined upstreams
 *
 *        Version:  1.0
 *        Created:  05.10.2011 16:06:15
 *
 *         Author:  Alexey Radkov (), 
 *        Company:  
 *
 * =============================================================================
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define UPSTREAM_VARS_SIZE (sizeof(upstream_vars) / sizeof(upstream_vars[0]))


typedef struct {
    ngx_array_t                              upstrands;
} ngx_http_upstrand_main_conf_t;


typedef struct {
    ngx_array_t                              dyn_upstrands;
} ngx_http_upstrand_loc_conf_t;


typedef enum {
    ngx_http_upstrand_order_normal = 0,
    ngx_http_upstrand_order_start_random
} ngx_http_upstrand_order_e;


typedef struct {
    ngx_str_t                                name;
    ngx_array_t                              upstreams;
    ngx_array_t                              b_upstreams;
    ngx_array_t                              next_upstream_statuses;
    ngx_msec_t                               next_upstream_timeout;
    ngx_int_t                                cur;
    ngx_int_t                                b_cur;
    ngx_http_upstrand_order_e                order;
    ngx_uint_t                               order_per_request:1;
    ngx_uint_t                               intercept_errors:1;
    ngx_uint_t                               retry_non_idempotent:1;
    ngx_uint_t                               debug_intermediate_stages:1;
} ngx_http_upstrand_conf_t;


typedef struct {
    time_t                                   blacklist_interval;
    time_t                                   blacklist_last_occurrence;
    ngx_uint_t                               index;
} ngx_http_upstrand_upstream_conf_t;


typedef struct {
    ngx_http_upstrand_conf_t                *upstrand;
    ngx_conf_t                              *cf;
    ngx_uint_t                               order_done:1;
} ngx_http_upstrand_conf_ctx_t;


/* there is no suitable typedef for finalize_request in ngx_http_upstream.h */
typedef void (*upstream_finalize_request_pt)(ngx_http_request_t *, ngx_int_t);


static const ngx_str_t upstream_vars[] =
{
    ngx_string("upstream_addr"),
    ngx_string("upstream_cache_status"),
    ngx_string("upstream_connect_time"),
    ngx_string("upstream_header_time"),
    ngx_string("upstream_response_length"),
    ngx_string("upstream_response_time"),
    ngx_string("upstream_status")
};


typedef struct {
    ngx_http_request_t                      *r;
    ngx_str_t                                upstream;
    ngx_str_t                                data[UPSTREAM_VARS_SIZE];
} ngx_http_upstrand_status_data_t;


typedef struct {
    ngx_uint_t                               last;
    upstream_finalize_request_pt             upstream_finalize_request;
} ngx_http_upstrand_request_common_ctx_t;


typedef struct {
    ngx_http_request_t                      *r;
    ngx_http_upstrand_conf_t                *upstrand;
    ngx_str_t                                cur_upstream;
    ngx_array_t                              status_data;
    ngx_int_t                                start_cur;
    ngx_int_t                                start_bcur;
    ngx_int_t                                cur;
    ngx_int_t                                b_cur;
    ngx_msec_t                               start_time;
    ngx_http_upstrand_request_common_ctx_t   common;
    ngx_uint_t                               backup_cycle:1;
    ngx_uint_t                               all_blacklisted:1;
} ngx_http_upstrand_request_ctx_t;


typedef struct {
    ngx_http_upstrand_request_common_ctx_t   common;
} ngx_http_upstrand_subrequest_ctx_t;


typedef struct {
    ngx_str_t                                key;
    ngx_int_t                                index;
}  ngx_http_cu_varhandle_t;


typedef struct {
    ngx_array_t                              data;
    ngx_int_t                                index;
} ngx_http_cu_varlist_elem_t;


static ngx_int_t ngx_http_upstrand_init(ngx_conf_t *cf);
static void *ngx_http_upstrand_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_upstrand_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_upstrand_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);
static ngx_int_t ngx_http_combined_upstreams_add_vars(ngx_conf_t *cf);
static ngx_int_t ngx_http_upstrand_intercept_errors(ngx_http_request_t *r,
    ngx_int_t status);
static ngx_int_t ngx_http_upstrand_response_header_filter(
    ngx_http_request_t *r);
static ngx_int_t ngx_http_upstrand_response_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in);
static void ngx_http_upstrand_check_upstream_vars(ngx_http_request_t *r,
    ngx_int_t rc);
static char *ngx_http_upstrand_block(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_upstrand(ngx_conf_t *cf, ngx_command_t *dummy,
    void *conf);
static char *ngx_http_dynamic_upstrand(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_upstrand_add_upstream(ngx_conf_t *cf,
    ngx_array_t *upstreams, ngx_str_t *name, time_t blacklist_interval);
#if (NGX_PCRE)
static char *ngx_http_upstrand_regex_add_upstream(ngx_conf_t *cf,
    ngx_array_t *upstreams, ngx_str_t *name, time_t blacklist_interval);
#endif
static ngx_int_t ngx_http_upstrand_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_get_dynamic_upstrand_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_get_upstrand_path_var_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_get_upstrand_status_var_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);


static char *ngx_http_add_upstream(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_combine_server_singlets(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


static ngx_command_t  ngx_http_combined_upstreams_commands[] = {

    { ngx_string("add_upstream"),
      NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
      ngx_http_add_upstream,
      0,
      0,
      NULL },
    { ngx_string("combine_server_singlets"),
      NGX_HTTP_UPS_CONF|NGX_CONF_NOARGS|NGX_CONF_TAKE12,
      ngx_http_combine_server_singlets,
      0,
      0,
      NULL },
    { ngx_string("upstrand"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_BLOCK|NGX_CONF_TAKE1,
      ngx_http_upstrand_block,
      NGX_HTTP_MAIN_CONF_OFFSET,
      0,
      NULL },
    { ngx_string("dynamic_upstrand"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_2MORE,
      ngx_http_dynamic_upstrand,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_variable_t  ngx_http_conbined_upstreams_vars[] =
{
    { ngx_string("upstrand_path"), NULL,
      ngx_http_get_upstrand_path_var_value, 0,
      NGX_HTTP_VAR_NOCACHEABLE, 0 },
    { ngx_string("upstrand_addr"), NULL,
      ngx_http_get_upstrand_status_var_value, (uintptr_t) 0,
      NGX_HTTP_VAR_NOCACHEABLE, 0 },
    { ngx_string("upstrand_cache_status"), NULL,
      ngx_http_get_upstrand_status_var_value, (uintptr_t) 1,
      NGX_HTTP_VAR_NOCACHEABLE, 0 },
    { ngx_string("upstrand_connect_time"), NULL,
      ngx_http_get_upstrand_status_var_value, (uintptr_t) 2,
      NGX_HTTP_VAR_NOCACHEABLE, 0 },
    { ngx_string("upstrand_header_time"), NULL,
      ngx_http_get_upstrand_status_var_value, (uintptr_t) 3,
      NGX_HTTP_VAR_NOCACHEABLE, 0 },
    { ngx_string("upstrand_response_length"), NULL,
      ngx_http_get_upstrand_status_var_value, (uintptr_t) 4,
      NGX_HTTP_VAR_NOCACHEABLE, 0 },
    { ngx_string("upstrand_response_time"), NULL,
      ngx_http_get_upstrand_status_var_value, (uintptr_t) 5,
      NGX_HTTP_VAR_NOCACHEABLE, 0 },
    { ngx_string("upstrand_status"), NULL,
      ngx_http_get_upstrand_status_var_value, (uintptr_t) 6,
      NGX_HTTP_VAR_NOCACHEABLE, 0 },

    { ngx_null_string, NULL, NULL, 0, 0, 0 }
};


static ngx_http_module_t  ngx_http_combined_upstreams_module_ctx = {
    ngx_http_combined_upstreams_add_vars,    /* preconfiguration */
    ngx_http_upstrand_init,                  /* postconfiguration */

    ngx_http_upstrand_create_main_conf,      /* create main configuration */
    NULL,                                    /* init main configuration */

    NULL,                                    /* create server configuration */
    NULL,                                    /* merge server configuration */

    ngx_http_upstrand_create_loc_conf,       /* create location configuration */
    ngx_http_upstrand_merge_loc_conf         /* merge location configuration */
};


ngx_module_t  ngx_http_combined_upstreams_module = {
    NGX_MODULE_V1,
    &ngx_http_combined_upstreams_module_ctx, /* module context */
    ngx_http_combined_upstreams_commands,    /* module directives */
    NGX_HTTP_MODULE,                         /* module type */
    NULL,                                    /* init master */
    NULL,                                    /* init module */
    NULL,                                    /* init process */
    NULL,                                    /* init thread */
    NULL,                                    /* exit thread */
    NULL,                                    /* exit process */
    NULL,                                    /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_upstrand_init(ngx_conf_t *cf)
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_upstrand_response_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_upstrand_response_body_filter;

    return NGX_OK;
}


/* FIXME: simplified version of ngx_http_upstream_intercept_errors(),
 * works fine but may have bugs related to the simplified aspects */
static ngx_int_t
ngx_http_upstrand_intercept_errors(ngx_http_request_t *r, ngx_int_t status)
{
    ngx_uint_t                 i;
    ngx_http_err_page_t       *err_page;
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    if (clcf->error_pages == NULL) {
        return NGX_DECLINED;
    }

    err_page = clcf->error_pages->elts;
    for (i = 0; i < clcf->error_pages->nelts; i++) {
        if (err_page[i].status == status) {
            return ngx_http_filter_finalize_request(r, NULL, status);
        }
    }

    return NGX_DECLINED;
}


static ngx_int_t
ngx_http_upstrand_response_header_filter(ngx_http_request_t *r)
{
    ngx_uint_t                               i;
    ngx_http_upstrand_request_ctx_t         *ctx;
    ngx_http_upstrand_subrequest_ctx_t      *sr_ctx;
    ngx_http_upstrand_request_common_ctx_t  *common;
    ngx_http_upstream_t                     *u;
    ngx_int_t                                status;
    ngx_int_t                               *next_upstream_statuses;
    ngx_uint_t                               is_next_upstream_status;
    ngx_http_upstrand_status_data_t         *status_data;

    ctx = ngx_http_get_module_ctx(r->main, ngx_http_combined_upstreams_module);
    if (ctx == NULL) {
        return ngx_http_next_header_filter(r);
    }

    if (r != ctx->r) {
        sr_ctx = ngx_http_get_module_ctx(r, ngx_http_combined_upstreams_module);
        if (sr_ctx == NULL) {
            return NGX_ERROR;
        }
    }
    common = r == ctx->r ? &ctx->common : &sr_ctx->common;

    u = r->upstream;

    status = r->headers_out.status;

    next_upstream_statuses = ctx->upstrand->next_upstream_statuses.elts;
    is_next_upstream_status = 0;

    for (i = 0; i < ctx->upstrand->next_upstream_statuses.nelts; i++) {

        if ((next_upstream_statuses[i] == -4 && status >= 400 && status < 500)
            ||
            (next_upstream_statuses[i] == -5 && status >= 500 && status < 600)
            ||
            next_upstream_statuses[i] == status
            ||
            (next_upstream_statuses[i] == -101
             && status == NGX_HTTP_BAD_GATEWAY
             && u && u->peer.connection == NULL)
            ||
            (next_upstream_statuses[i] == -102
             && status == NGX_HTTP_GATEWAY_TIME_OUT
             && u && u->peer.connection == NULL))
        {
            is_next_upstream_status = 1;
            break;
        }
    }

    status_data = ngx_array_push(&ctx->status_data);
    if (status_data == NULL) {
        return NGX_ERROR;
    }
    status_data->r = r;
    status_data->upstream = ctx->cur_upstream;
    ngx_memzero(&status_data->data, sizeof(status_data->data));

    if (u) {
        if (u->finalize_request) {
            common->upstream_finalize_request = u->finalize_request;
        }
        u->finalize_request = ngx_http_upstrand_check_upstream_vars;
    }

    if (is_next_upstream_status) {
        ngx_http_upstrand_upstream_conf_t  *u_elts, *bu_elts;
        ngx_uint_t                          bu_nelts;
        time_t                              now = ngx_time();

        u_elts = ctx->upstrand->upstreams.elts;
        bu_elts = ctx->upstrand->b_upstreams.elts;
        bu_nelts = ctx->upstrand->b_upstreams.nelts;

        /* do not blacklist last upstream immediately after whitelisting */
        if (!ctx->all_blacklisted) {
            if (ctx->backup_cycle && bu_nelts > 0) {
                if (bu_elts[ctx->b_cur].blacklist_interval > 0) {
                    bu_elts[ctx->b_cur].blacklist_last_occurrence = now;
                }
            } else {
                if (u_elts[ctx->cur].blacklist_interval > 0) {
                    u_elts[ctx->cur].blacklist_last_occurrence = now;
                }
            }
        }

        if (r->method & (NGX_HTTP_POST|NGX_HTTP_LOCK|NGX_HTTP_PATCH)
            && !ctx->upstrand->retry_non_idempotent
            && u && u->peer.connection != NULL)
        {
            common->last = 1;

        } else {
            if (u && ctx->start_time == 0) {
                ctx->start_time = u->peer.start_time;
            }

            if (!common->last) {
                ngx_http_request_t  *sr;

                if (ctx->upstrand->next_upstream_timeout
                    && ngx_current_msec - ctx->start_time
                        >= ctx->upstrand->next_upstream_timeout)
                {

                    common->last = 1;
                } else {

                    if (ngx_http_subrequest(r, &r->main->uri, &r->main->args,
                                            &sr, NULL, 0) != NGX_OK)
                    {
                        return NGX_ERROR;
                    }

                    /* subrequest must use method of the original request */
                    sr->method = r->method;
                    sr->method_name = r->method_name;

                    return NGX_OK;
                }
            }
        }

    } else {
        common->last = 1;
    }

    if (common->last && ctx->upstrand->intercept_errors) {
        if (ngx_http_upstrand_intercept_errors(r->main, status) == NGX_OK) {
            return NGX_OK;
        }
    }

    if (r != r->main && common->last) {
        /* copy HTTP headers to main request */
        r->main->headers_out = r->headers_out;
        /* FIXME: must other fields like upstream_states be copied too? */

        return ngx_http_next_header_filter(r->main);
    }

    return ngx_http_next_header_filter(r);
}


static ngx_int_t
ngx_http_upstrand_response_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_upstrand_request_ctx_t         *ctx;
    ngx_http_upstrand_subrequest_ctx_t      *sr_ctx;
    ngx_http_upstrand_request_common_ctx_t  *common;
    ngx_http_upstream_t                     *u;

    ctx = ngx_http_get_module_ctx(r->main, ngx_http_combined_upstreams_module);
    if (ctx == NULL) {
        return ngx_http_next_body_filter(r, in);
    }

    if (r != ctx->r) {
        sr_ctx = ngx_http_get_module_ctx(r, ngx_http_combined_upstreams_module);
        if (sr_ctx == NULL) {
            return NGX_ERROR;
        }
    }
    common = r == ctx->r ? &ctx->common : &sr_ctx->common;

    u = r->upstream;

    if (!common->last) {
        /* if upstream buffering is off then its out_bufs must be updated
         * right here! (at least in nginx 1.8.0) */
        if (!ctx->upstrand->debug_intermediate_stages && u && !u->buffering)
        {
            u->out_bufs = NULL;
        }
        return ngx_http_next_body_filter(r,
                        ctx->upstrand->debug_intermediate_stages ? in : NULL);
    }

    if (!ctx->upstrand->debug_intermediate_stages && in != NULL) {
        ngx_chain_t  *cl = in;

        while (cl->next) {
            cl = cl->next;
        }
        if (cl->buf->last_in_chain) {
            cl->buf->last_buf = 1;
        }
    }

    return ngx_http_next_body_filter(r, in);
}


static void
ngx_http_upstrand_check_upstream_vars(ngx_http_request_t *r, ngx_int_t  rc)
{
    ngx_uint_t                               i;
    ngx_http_upstrand_request_ctx_t         *ctx;
    ngx_http_upstrand_subrequest_ctx_t      *sr_ctx;
    ngx_http_upstrand_request_common_ctx_t  *common;
    ngx_http_variable_value_t               *var;
    ngx_str_t                                var_name;
    ngx_int_t                                key;
    ngx_http_upstrand_status_data_t         *upstreams, *status = NULL;

    ctx = ngx_http_get_module_ctx(r->main, ngx_http_combined_upstreams_module);
    if (ctx == NULL) {
        return;
    }

    upstreams = ctx->status_data.elts;
    for (i = 0; i < ctx->status_data.nelts; i++) {
        if (r == upstreams[i].r) {
            status = &upstreams[i];
            break;
        }
    }
    if (status == NULL) {
        return;
    }

    for (i = 0; i < UPSTREAM_VARS_SIZE; i++) {
        var_name = upstream_vars[i];
        key = ngx_hash_key(var_name.data, var_name.len);

        var = ngx_http_get_variable(r, &var_name, key);
        if (var == NULL) {
            return;
        }

        if (var->not_found || !var->valid || var->len == 0) {
            continue;
        }

        status->data[i].len = var->len;

        if (r == r->main) {
            /* main request data must be always accessible */
            status->data[i].data = var->data;
        } else {
            status->data[i].data = ngx_pnalloc(r->main->pool, var->len);
            if (status->data[i].data == NULL) {
                return;
            }
            ngx_memcpy(status->data[i].data, var->data, var->len);
        }
    }

    if (r != ctx->r) {
        sr_ctx = ngx_http_get_module_ctx(r, ngx_http_combined_upstreams_module);
        if (sr_ctx == NULL) {
            return;
        }
    }
    common = r == ctx->r ? &ctx->common : &sr_ctx->common;

    if (common->upstream_finalize_request) {
        common->upstream_finalize_request(r, rc);
    }
}


static ngx_int_t
ngx_http_upstrand_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v,
    uintptr_t data)
{
    ngx_http_upstrand_conf_t  *upstrand = (ngx_http_upstrand_conf_t *) data;

    ngx_uint_t                                i;
    ngx_http_upstrand_request_ctx_t          *ctx;
    ngx_http_upstrand_subrequest_ctx_t       *sr_ctx;
    ngx_http_upstrand_request_common_ctx_t   *common;
    ngx_http_upstrand_upstream_conf_t        *u_elts, *bu_elts;
    ngx_uint_t                                u_nelts, bu_nelts;
    ngx_http_upstream_main_conf_t            *umcf;
    ngx_http_upstream_srv_conf_t            **uscfp;
    time_t                                    now;
    ngx_int_t                                 start_cur, start_bcur;
    ngx_int_t                                 cur_cur, cur_bcur;
    ngx_uint_t                                force_last = 0;

    ctx = ngx_http_get_module_ctx(r->main, ngx_http_combined_upstreams_module);

    u_elts = upstrand->upstreams.elts;
    bu_elts = upstrand->b_upstreams.elts;
    u_nelts = upstrand->upstreams.nelts;
    bu_nelts = upstrand->b_upstreams.nelts;

    if (ctx != NULL) {
        if (ctx->upstrand->name.len != upstrand->name.len
            || ngx_strncmp(ctx->upstrand->name.data, upstrand->name.data,
                           upstrand->name.len) != 0)
        {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "accessing multiple upstrand variables in a single "
                          "request is prohibited (original upstrand is \"%V\", "
                          "new upstrand is \"%V\")",
                          &ctx->upstrand->name, &upstrand->name);
            return NGX_ERROR;
        }

        if (r == ctx->r
            || ngx_http_get_module_ctx(r, ngx_http_combined_upstreams_module)
                != NULL)
        {
            goto was_accessed;
        }
    }

    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->main->pool,
                          sizeof(ngx_http_upstrand_request_ctx_t));
        if (ctx == NULL) {
            return NGX_ERROR;
        }

        ctx->r = r;
        ctx->upstrand = upstrand;
        if (ngx_array_init(&ctx->status_data, r->pool, 1,
                           sizeof(ngx_http_upstrand_status_data_t)) != NGX_OK)
        {
            return NGX_ERROR;
        }
        if (upstrand->order_per_request &&
            upstrand->order == ngx_http_upstrand_order_start_random)
        {
            ctx->start_cur = ngx_random() % u_nelts;
            ctx->start_bcur = ngx_random() % bu_nelts;
        } else {
            ctx->start_cur = upstrand->cur;
            ctx->start_bcur = upstrand->b_cur;
        }
        ctx->cur = ctx->start_cur;
        ctx->b_cur = ctx->start_bcur;

        if (u_nelts > 0) {
            if (!upstrand->order_per_request) {
                upstrand->cur = (upstrand->cur + 1) % u_nelts;
            }
        } else {
            ctx->backup_cycle = 1;
            if (bu_nelts > 0 && !upstrand->order_per_request) {
                upstrand->b_cur = (upstrand->b_cur + 1) % bu_nelts;
            }
        }

        ngx_http_set_ctx(r->main, ctx, ngx_http_combined_upstreams_module);

    } else if (r != ctx->r) {

        sr_ctx = ngx_pcalloc(r->pool,
                             sizeof(ngx_http_upstrand_subrequest_ctx_t));
        if (sr_ctx == NULL) {
            return NGX_ERROR;
        }
        ngx_http_set_ctx(r, sr_ctx, ngx_http_combined_upstreams_module);

        if (ctx->backup_cycle) {
            if (bu_nelts > 0) {
                ctx->b_cur = (ctx->b_cur + 1) % bu_nelts;
            }
        } else if (u_nelts > 0) {
            ctx->cur = (ctx->cur + 1) % u_nelts;
            if (ctx->cur == ctx->start_cur) {
                ctx->backup_cycle = 1;
            }
        }
    }

    now = ngx_time();
    start_cur = cur_cur = ctx->cur;
    start_bcur = cur_bcur = ctx->b_cur;

    for ( ;; ) {
        if (ctx->backup_cycle) {
            if (bu_nelts > 0) {
                if (difftime(now, bu_elts[cur_bcur].blacklist_last_occurrence)
                    < bu_elts[cur_bcur].blacklist_interval)
                {
                    ngx_int_t  old_bcur = cur_bcur;

                    cur_bcur = (cur_bcur + 1) % bu_nelts;
                    if (cur_bcur == ctx->start_bcur) {
                        force_last = 1;
                    } else if (!force_last) {
                        ctx->b_cur = old_bcur;
                    }
                    if (cur_bcur == start_bcur) {
                        ctx->all_blacklisted = 1;
                        break;
                    }
                } else {
                    break;
                }
            } else {
                ctx->all_blacklisted = 1;
                break;
            }
        } else if (u_nelts > 0) {
            if (difftime(now, u_elts[cur_cur].blacklist_last_occurrence)
                < u_elts[cur_cur].blacklist_interval)
            {
                ngx_int_t  old_cur = cur_cur;

                cur_cur = (cur_cur + 1) % u_nelts;
                if (bu_nelts == 0 && cur_cur == ctx->start_cur) {
                    force_last = 1;
                } else if (!force_last) {
                    ctx->cur = old_cur;
                }
                if (cur_cur == start_cur) {
                    ctx->backup_cycle = 1;
                }
            } else {
                break;
            }
        }
    }

    if (ctx->all_blacklisted) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "all upstreams in upstrand \"%V\" are blacklisted, "
                      "whitelisting them", &upstrand->name);
        ctx->cur = start_cur;
        ctx->b_cur = start_bcur;
        for (i = 0; i < u_nelts; i++) {
            u_elts[i].blacklist_last_occurrence = 0;
        }
        for (i = 0; i < bu_nelts; i++) {
            bu_elts[i].blacklist_last_occurrence = 0;
        }
    }

    if (r != ctx->r) {
        sr_ctx = ngx_http_get_module_ctx(r, ngx_http_combined_upstreams_module);
        if (sr_ctx == NULL) {
            return NGX_ERROR;
        }
    }
    common = r == ctx->r ? &ctx->common : &sr_ctx->common;

    if (force_last ||
        (bu_nelts == 0 &&
         (u_nelts == 0
          || (ctx->cur + 1) % u_nelts == (ngx_uint_t) ctx->start_cur)) ||
        (ctx->backup_cycle &&
         (bu_nelts == 0
          || (ctx->b_cur + 1) % bu_nelts == (ngx_uint_t) ctx->start_bcur)))
    {
        common->last = 1;
    }

was_accessed:

    umcf = ngx_http_get_module_main_conf(r, ngx_http_upstream_module);
    uscfp = umcf->upstreams.elts;

    if (ctx->backup_cycle && bu_nelts > 0) {
        ctx->cur_upstream = uscfp[bu_elts[ctx->b_cur].index]->host;
    } else {
        ctx->cur_upstream = uscfp[u_elts[ctx->cur].index]->host;
    }

    v->valid = 1;
    v->not_found = 0;
    v->len = ctx->cur_upstream.len;
    v->data = ctx->cur_upstream.data;

    return NGX_OK;
}


static ngx_int_t
ngx_http_get_dynamic_upstrand_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t  data)
{
    ngx_uint_t                     i;
    ngx_int_t                     *index = (ngx_int_t *) data;
    ngx_int_t                      found_idx = NGX_ERROR;
    ngx_http_upstrand_loc_conf_t  *lcf;
    ngx_array_t                   *upstrands;
    ngx_http_cu_varlist_elem_t    *upstrands_elts;
    ngx_array_t                   *upstrand_cands;
    ngx_http_cu_varhandle_t       *upstrand_cands_elts;
    ngx_http_variable_value_t     *upstrand_var = NULL;
    ngx_str_t                      upstrand_var_name;

    if (index == NULL) {
        return NGX_ERROR;
    }

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_combined_upstreams_module);

    upstrands = &lcf->dyn_upstrands;
    upstrands_elts = upstrands->elts;

    for (i = 0; i < upstrands->nelts; i++) {
        if (*index != upstrands_elts[i].index) {
            continue;
        }
        found_idx = i;
        break;
    }

    if (found_idx == NGX_ERROR) {
        return NGX_ERROR;
    }

    upstrand_cands = &upstrands_elts[found_idx].data;
    upstrand_cands_elts = upstrand_cands->elts;

    for (i = 0; i < upstrand_cands->nelts; i++) {
        ngx_int_t                   key;
        ngx_str_t                   var_name;
        ngx_http_variable_value_t  *found = NULL;

        if (upstrand_cands_elts[i].index == NGX_ERROR) {
            var_name = upstrand_cands_elts[i].key;
        } else {
            found = ngx_http_get_indexed_variable(r,
                                                upstrand_cands_elts[i].index);
            if (found == NULL || found->len == 0) {
                continue;
            }

            var_name.len = found->len;
            var_name.data = found->data;
        }

        upstrand_var_name.len = var_name.len + 9;
        upstrand_var_name.data = ngx_pnalloc(r->pool, upstrand_var_name.len);
        if (upstrand_var_name.data == NULL) {
            return NGX_ERROR;
        }

        ngx_memcpy(upstrand_var_name.data, "upstrand_", 9);
        ngx_memcpy(upstrand_var_name.data + 9, var_name.data, var_name.len);

        key = ngx_hash_strlow(upstrand_var_name.data, upstrand_var_name.data,
                              upstrand_var_name.len);

        upstrand_var = ngx_http_get_variable(r, &upstrand_var_name, key);
        if (upstrand_var == NULL) {
            return NGX_ERROR;
        }

        break;
    }

    if (upstrand_var == NULL) {
        v->len = 0;
        v->data = (u_char *) "";
        v->valid = 1;
        v->no_cacheable = 0;
        v->not_found = 0;
    } else {
        *v = *upstrand_var;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_get_upstrand_path_var_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t  data)
{
    ngx_uint_t                        i, len = 0, cur_len = 0;
    ngx_http_upstrand_request_ctx_t  *ctx;
    ngx_http_upstrand_status_data_t  *upstreams;

    ctx = ngx_http_get_module_ctx(r->main, ngx_http_combined_upstreams_module);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    upstreams = ctx->status_data.elts;

    for (i = 0; i < ctx->status_data.nelts; i++) {
        len += upstreams[i].upstream.len + 4;
    }

    v->data = (u_char *) "";

    if (len > 0) {
        len -= 4;

        v->data = ngx_pnalloc(r->pool, len);
        if (v->data == NULL) {
            return NGX_ERROR;
        }

        for (i = 0; i < ctx->status_data.nelts; i++) {
            ngx_memcpy(v->data + cur_len, upstreams[i].upstream.data,
                       upstreams[i].upstream.len);
            cur_len += upstreams[i].upstream.len;
            if (i < ctx->status_data.nelts - 1) {
                ngx_memcpy(v->data + cur_len, " -> ", 4);
                cur_len += 4;
            }
        }
    }

    v->len = len;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_http_get_upstrand_status_var_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t  data)
{
    ngx_uint_t                        i, len = 0, cur_len = 0;
    ngx_http_upstrand_request_ctx_t  *ctx;
    ngx_http_upstrand_status_data_t  *upstreams;
    ngx_uint_t                        idx = data;

    if (idx >= UPSTREAM_VARS_SIZE) {
        return NGX_ERROR;
    }

    ctx = ngx_http_get_module_ctx(r->main, ngx_http_combined_upstreams_module);
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    upstreams = ctx->status_data.elts;

    for (i = 0; i < ctx->status_data.nelts; i++) {
        len += upstreams[i].upstream.len + 4 + upstreams[i].data[idx].len;
    }

    v->data = (u_char *) "";

    if (len > 0) {
        --len;

        v->data = ngx_pnalloc(r->pool, len);
        if (v->data == NULL) {
            return NGX_ERROR;
        }

        for (i = 0; i < ctx->status_data.nelts; i++) {
            ngx_memcpy(v->data + cur_len, i == 0 ? "(" : " (", i == 0 ? 1 : 2);
            cur_len += (i == 0 ? 1 : 2);
            ngx_memcpy(v->data + cur_len, upstreams[i].upstream.data,
                       upstreams[i].upstream.len);
            cur_len += upstreams[i].upstream.len;
            ngx_memcpy(v->data + cur_len, ") ", 2);
            cur_len += 2;
            ngx_memcpy(v->data + cur_len, upstreams[i].data[idx].data,
                       upstreams[i].data[idx].len);
            cur_len += upstreams[i].data[idx].len;
        }
    }

    v->len = len;
    v->valid = 1;
    v->no_cacheable = 0;
    v->not_found = 0;

    return NGX_OK;
}


static void *
ngx_http_upstrand_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_upstrand_main_conf_t  *mcf;

    mcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstrand_main_conf_t));
    if (mcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&mcf->upstrands, cf->pool, 1,
                       sizeof(ngx_http_upstrand_conf_t)) != NGX_OK)
    {
        return NULL;
    }

    return mcf;
}


static void *
ngx_http_upstrand_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_upstrand_loc_conf_t  *lcf;

    lcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstrand_loc_conf_t));
    if (lcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&lcf->dyn_upstrands, cf->pool, 1,
                       sizeof(ngx_http_cu_varlist_elem_t)) != NGX_OK)
    {
        return NULL;
    }

    return lcf;
}


static char *
ngx_http_upstrand_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_upstrand_loc_conf_t  *prev = parent;
    ngx_http_upstrand_loc_conf_t  *conf = child;

    ngx_uint_t                     i;

    for (i = 0; i < prev->dyn_upstrands.nelts; i++) {
        ngx_http_cu_varlist_elem_t  *elem;
        
        elem = ngx_array_push(&conf->dyn_upstrands);
        if (elem == NULL) {
            return NGX_CONF_ERROR;
        }

        *elem = ((ngx_http_cu_varlist_elem_t *) prev->dyn_upstrands.elts)[i];
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_combined_upstreams_add_vars(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var, *v;

    for (v = ngx_http_conbined_upstreams_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}


static char *
ngx_http_upstrand_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstrand_main_conf_t  *mcf = conf;

    char                           *rv;
    ngx_str_t                      *value, name;
    ngx_conf_t                      save;
    ngx_http_variable_t            *var;
    ngx_str_t                       var_name;
    ngx_http_upstrand_conf_t       *upstrand;
    ngx_http_upstrand_conf_ctx_t    ctx;
    ngx_uint_t                      u_nelts, bu_nelts;

    upstrand = ngx_array_push(&mcf->upstrands);
    if (upstrand == NULL) {
        return NGX_CONF_ERROR;
    }
    ngx_memzero(upstrand, sizeof(ngx_http_upstrand_conf_t));

    if (ngx_array_init(&upstrand->upstreams, cf->pool, 1,
                       sizeof(ngx_http_upstrand_upstream_conf_t)) != NGX_OK ||
        ngx_array_init(&upstrand->b_upstreams, cf->pool, 1,
                       sizeof(ngx_http_upstrand_upstream_conf_t)) != NGX_OK ||
        ngx_array_init(&upstrand->next_upstream_statuses, cf->pool, 1,
                       sizeof(ngx_int_t)) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    upstrand->order = ngx_http_upstrand_order_normal;

    value = cf->args->elts;
    name = value[1];
    upstrand->name = name;

    var_name.len = name.len + 9;
    var_name.data = ngx_pnalloc(cf->pool, var_name.len);
    if (var_name.data == NULL) {
        return NGX_CONF_ERROR;
    }

    ngx_memcpy(var_name.data, "upstrand_", 9);
    ngx_memcpy(var_name.data + 9, name.data, name.len);

    var = ngx_http_add_variable(cf, &var_name,
                            NGX_HTTP_VAR_CHANGEABLE|NGX_HTTP_VAR_NOCACHEABLE);
    if (var == NULL) {
        return NGX_CONF_ERROR;
    }

    var->get_handler = ngx_http_upstrand_variable;
    var->data = (uintptr_t) upstrand;

    ctx.upstrand = upstrand;
    ctx.cf = &save;
    ctx.order_done = 0;

    save = *cf;
    cf->ctx = &ctx;
    cf->handler = ngx_http_upstrand;
    cf->handler_conf = conf;

    rv = ngx_conf_parse(cf, NULL);

    *cf = save;

    if (rv != NGX_CONF_OK) {
        return rv;
    }

    u_nelts = upstrand->upstreams.nelts;
    bu_nelts = upstrand->b_upstreams.nelts;

    if (u_nelts == 0 && bu_nelts == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "no upstream registered in "
                           "upstrand \"%V\"", &name);
        return NGX_CONF_ERROR;
    }

    if (upstrand->order == ngx_http_upstrand_order_start_random &&
        !upstrand->order_per_request)
    {
        if (u_nelts > 0) {
            upstrand->cur = ngx_random() % u_nelts;
        }
        if (bu_nelts > 0) {
            upstrand->b_cur = ngx_random() % bu_nelts;
        }
    }

    return rv;
}


static char *
ngx_http_upstrand(ngx_conf_t *cf, ngx_command_t *dummy, void *conf)
{
    ngx_uint_t                     i;
    ngx_str_t                     *value;
    ngx_http_upstrand_conf_ctx_t  *ctx;

    value = cf->args->elts;
    ctx = cf->ctx;

    if (cf->args->nelts == 1) {
        if (value[0].len == 16 &&
            ngx_strncmp(value[0].data, "intercept_errors", 16) == 0)
        {
            ctx->upstrand->intercept_errors = 1;
            return NGX_CONF_OK;
        }
        if (value[0].len == 25 &&
            ngx_strncmp(value[0].data, "debug_intermediate_stages", 25) == 0)
        {
            ctx->upstrand->debug_intermediate_stages = 1;
            return NGX_CONF_OK;
        }
    }

    if (cf->args->nelts == 2) {
        if (value[0].len == 21 &&
            ngx_strncmp(value[0].data, "next_upstream_timeout", 21) == 0)
        {
            time_t  timeout;

            if (ctx->upstrand->next_upstream_timeout) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "duplicate upstrand directive \"next_upstream_timeout\"");
                return NGX_CONF_ERROR;
            }

            timeout = ngx_parse_time(&value[1], 1);

            if (timeout == NGX_ERROR) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "bad timeout value: \"%V\"", &value[1]);
                return NGX_CONF_ERROR;
            }

            ctx->upstrand->next_upstream_timeout = timeout * 1000;
            return NGX_CONF_OK;
        }
    }

    if (cf->args->nelts == 2 || cf->args->nelts == 3) {

        if (value[0].len == 5 && ngx_strncmp(value[0].data, "order", 5) == 0) {
            ngx_uint_t  done[2] = {0, 0};

            if (ctx->order_done) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "duplicate upstrand directive \"order\"");
                return NGX_CONF_ERROR;
            }

            for (i = 1; i < cf->args->nelts; i++) {

                if (value[i].len == 12 &&
                    ngx_strncmp(value[i].data, "start_random", 12) == 0)
                {
                    if (done[0]++ > 0) {
                        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                    "bad upstrand directive \"order\" content");
                        return NGX_CONF_ERROR;
                    }
                    ctx->upstrand->order = ngx_http_upstrand_order_start_random;
                }

                if (value[i].len == 11 &&
                    ngx_strncmp(value[i].data, "per_request", 11) == 0)
                {
                    if (done[1]++ > 0) {
                        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                    "bad upstrand directive \"order\" content");
                        return NGX_CONF_ERROR;
                    }
                    ctx->upstrand->order_per_request = 1;
                }
            }

            if (done[0] + done[1] != cf->args->nelts - 1) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "bad upstrand directive \"order\" content");
                return NGX_CONF_ERROR;
            }

            ctx->order_done = 1;
            return NGX_CONF_OK;
        }
    }

    if (cf->args->nelts > 1 && cf->args->nelts < 5) {
        if (value[0].len == 8 && ngx_strncmp(value[0].data, "upstream", 8) == 0)
        {
            ngx_uint_t  done[2] = {0, 0};
            time_t      blacklist_interval = 0;

            for (i = 2; i < cf->args->nelts; i++) {

                if (value[i].len == 6 &&
                    ngx_strncmp(value[i].data, "backup", 6) == 0)
                {
                    if (done[0]++ > 0) {
                        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                "bad upstrand directive \"upstream\" content");
                        return NGX_CONF_ERROR;
                    }
                }

                if (value[i].len > 19 &&
                    ngx_strncmp(value[i].data, "blacklist_interval=", 19) == 0)
                {
                    ngx_str_t  interval = value[i];

                    if (done[1]++ > 0) {
                        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                "bad upstrand directive \"upstream\" content");
                        return NGX_CONF_ERROR;
                    }

                    interval.data += 19;
                    interval.len -= 19;

                    blacklist_interval = ngx_parse_time(&interval, 1);

                    if (blacklist_interval == NGX_ERROR) {
                        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                "bad blacklist interval: \"%V\"", &interval);
                        return NGX_CONF_ERROR;
                    }

                }
            }

            if (done[0] + done[1] != cf->args->nelts - 2) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                "bad upstrand directive \"upstream\" content");
                return NGX_CONF_ERROR;
            }

            return ngx_http_upstrand_add_upstream(ctx->cf,
                        done[0] == 0 ? &ctx->upstrand->upstreams :
                                       &ctx->upstrand->b_upstreams, &value[1],
                        blacklist_interval);
        }
    }

    if (value[0].len == 22 &&
        ngx_strncmp(value[0].data, "next_upstream_statuses", 22) == 0)
    {
        ngx_int_t  *status;

        for (i = 1; i < cf->args->nelts; i++) {
            ngx_uint_t  invalid_status = 0;

            if (value[i].len == 14 &&
                ngx_strncmp(value[i].data, "non_idempotent", 14) == 0)
            {
                ctx->upstrand->retry_non_idempotent = 1;
                continue;
            }

            status = ngx_array_push(&ctx->upstrand->next_upstream_statuses);
            if (status == NULL) {
                return NGX_CONF_ERROR;
            }

            *status = ngx_atoi(value[i].data, value[i].len);

            if (*status == NGX_ERROR) {

                if (value[i].len == 3 &&
                    ngx_strncmp(value[i].data + 1, "xx", 2) == 0)
                {
                    switch (value[i].data[0]) {
                    case '4':
                        *status = -4;
                        break;
                    case '5':
                        *status = -5;
                        break;
                    default:
                        invalid_status = 1;
                        break;
                    }

                } else if (value[i].len == 5 &&
                           ngx_strncmp(value[i].data, "error", 5) == 0)
                {
                    *status = -101;

                } else if (value[i].len == 7 &&
                           ngx_strncmp(value[i].data, "timeout", 7) == 0)
                {
                    *status = -102;

                } else {
                    invalid_status = 1;

                }

                if (invalid_status) {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                       "invalid status \"%V\"", &value[i]);
                    return NGX_CONF_ERROR;
                }
            }
        }

        return NGX_CONF_OK;
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "bad upstrand directive");

    return NGX_CONF_ERROR;
}


static char *
ngx_http_dynamic_upstrand(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_upstrand_loc_conf_t  *lcf = conf;

    ngx_uint_t                     i;
    ngx_str_t                     *value;
    ngx_http_variable_t           *v;
    ngx_http_cu_varlist_elem_t    *resvar;
    ngx_int_t                      v_idx;
    ngx_uint_t                    *v_idx_ptr;

    value = cf->args->elts;

    if (value[1].len < 2 || value[1].data[0] != '$') {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid variable name \"%V\"", &value[1]);
        return NGX_CONF_ERROR;
    }

    value[1].len--;
    value[1].data++;

    resvar = ngx_array_push(&lcf->dyn_upstrands);
    if (resvar == NULL) {
        return NGX_CONF_ERROR;
    }

    if (ngx_array_init(&resvar->data, cf->pool, cf->args->nelts - 2,
                       sizeof(ngx_http_cu_varhandle_t)) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    for (i = 2; i < cf->args->nelts; i++) {
        ngx_http_cu_varhandle_t  *res;
        ngx_int_t                 index = NGX_ERROR;
        ngx_uint_t                isvar;

        res = ngx_array_push(&resvar->data);
        if (res == NULL) {
            return NGX_CONF_ERROR;
        }

        isvar = value[i].len > 1 && value[i].data[0] == '$' ? 1 : 0;
        if (!isvar) {
            res->index = index;
            res->key = value[i];
            break;
        }

        value[i].len--;
        value[i].data++;

        index = ngx_http_get_variable_index(cf, &value[i]);
        if (index == NGX_ERROR) {
            return NGX_CONF_ERROR;
        }

        res->index = index;
        res->key = value[i];
    }

    v = ngx_http_add_variable(cf, &value[1], NGX_HTTP_VAR_CHANGEABLE);
    if (v == NULL) {
        return NGX_CONF_ERROR;
    }

    v_idx = ngx_http_get_variable_index(cf, &value[1]);
    if (v_idx == NGX_ERROR) {
        return NGX_CONF_ERROR;
    }

    v_idx_ptr = ngx_palloc(cf->pool, sizeof(ngx_uint_t));
    if (v_idx_ptr == NULL) {
        return NGX_CONF_ERROR;
    }

    resvar->index = v_idx;
    *v_idx_ptr = v_idx;

    v->data = (uintptr_t) v_idx_ptr;
    v->get_handler = ngx_http_get_dynamic_upstrand_value;

    return NGX_CONF_OK;
}


static char *
ngx_http_upstrand_add_upstream(ngx_conf_t *cf, ngx_array_t *upstreams,
    ngx_str_t *name, time_t blacklist_interval)
{
    ngx_uint_t                           i;
    ngx_uint_t                           found_idx;
    ngx_http_upstrand_upstream_conf_t   *u;
    ngx_http_upstream_main_conf_t       *umcf;
    ngx_http_upstream_srv_conf_t       **uscfp;

#if (NGX_PCRE)
    if (name->len > 1 && name->data[0] == '~') {
        name->len -= 1;
        name->data += 1;

        return ngx_http_upstrand_regex_add_upstream(cf, upstreams, name,
                                                    blacklist_interval);
    }
#endif

    umcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);
    uscfp = umcf->upstreams.elts;

    found_idx = NGX_ERROR;

    for (i = 0; i < umcf->upstreams.nelts; i++) {
        if (uscfp[i]->host.len == name->len &&
            ngx_strncasecmp(uscfp[i]->host.data, name->data, name->len) == 0) {
            found_idx = i;
            break;
        }
    }

    if (found_idx == (ngx_uint_t) NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "upstream \"%V\" is not found",
                           name);
        return NGX_CONF_ERROR;
    }

    for (i = 0; i < upstreams->nelts; i++) {
        ngx_http_upstrand_upstream_conf_t  *registered = upstreams->elts;
        if (found_idx == registered[i].index) {
            return NGX_CONF_OK;
        }
    }

    u = ngx_array_push(upstreams);
    if (u == NULL) {
        return NGX_CONF_ERROR;
    }

    u->index = found_idx;
    u->blacklist_last_occurrence = 0;
    u->blacklist_interval = blacklist_interval;

    return NGX_CONF_OK;
}


#if (NGX_PCRE)

static char *
ngx_http_upstrand_regex_add_upstream(ngx_conf_t *cf, ngx_array_t *upstreams,
    ngx_str_t *name, time_t blacklist_interval)
{
    ngx_uint_t                           i, j;
    ngx_http_upstrand_upstream_conf_t   *u;
    ngx_http_upstream_main_conf_t       *umcf;
    ngx_http_upstream_srv_conf_t       **uscfp;
    ngx_regex_compile_t                  rc;
    u_char                               errstr[NGX_MAX_CONF_ERRSTR];
    ngx_http_regex_t                    *re;

    ngx_memzero(&rc, sizeof(ngx_regex_compile_t));
    rc.pattern = *name;
    rc.err.len = NGX_MAX_CONF_ERRSTR;
    rc.err.data = errstr;

    re = ngx_http_regex_compile(cf, &rc);

    if (re == NULL) {
        return NGX_CONF_ERROR;
    }
    
    umcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);
    uscfp = umcf->upstreams.elts;

    for (i = 0; i < umcf->upstreams.nelts; i++) {
        ngx_uint_t  is_registered = 0;

        if (ngx_regex_exec(re->regex, &uscfp[i]->host, NULL, 0)
            != NGX_REGEX_NO_MATCHED)
        {
            for (j = 0; j < upstreams->nelts; j++) {
                ngx_http_upstrand_upstream_conf_t  *registered =
                        upstreams->elts;

                if (i == registered[j].index) {
                    is_registered = 1;
                    break;
                }
            }

            if (is_registered) {
                continue;
            }

            u = ngx_array_push(upstreams);
            if (u == NULL) {
                return NGX_CONF_ERROR;
            }

            u->index = i;
            u->blacklist_last_occurrence = 0;
            u->blacklist_interval = blacklist_interval;
        }
    }

    return NGX_CONF_OK;
}

#endif


static char *
ngx_http_add_upstream(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_uint_t                      i, j;
    ngx_http_upstream_main_conf_t  *umcf;
    ngx_http_upstream_srv_conf_t   *uscf, **uscfp;
    ngx_http_upstream_server_t     *us;
    ngx_str_t                      *value;
    ngx_uint_t                      backup = 0;

    umcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);
    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    uscfp = umcf->upstreams.elts;
    value = cf->args->elts;

    if (cf->args->nelts > 3) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "number of parameters must be 1 or 2");
        return NGX_CONF_ERROR;
    }

    if (value[1].len == uscf->host.len &&
        ngx_strncasecmp(value[1].data, uscf->host.data, value[1].len) == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "upstream \"%V\" makes recursion", &value[1]);
        return NGX_CONF_ERROR;
    }

    if (cf->args->nelts == 3) {
        if (ngx_strncmp(value[2].data, "backup", 6) == 0) {
            backup = 1;
        } else {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid parameter \"%V\"",
                               &value[2]);
            return NGX_CONF_ERROR;
        }
    }

    for (i = 0; i < umcf->upstreams.nelts; i++) {
        if (uscfp[i]->host.len == value[1].len &&
            ngx_strncasecmp(uscfp[i]->host.data,
                            value[1].data, value[1].len) == 0) {
            if (uscf->servers == NULL) {
                uscf->servers = ngx_array_create(cf->pool, 4,
                                            sizeof(ngx_http_upstream_server_t));
                if (uscf->servers == NULL)
                    return NGX_CONF_ERROR;
            }

            us = ngx_array_push_n(uscf->servers, uscfp[i]->servers->nelts);
            if (us == NULL)
                return NGX_CONF_ERROR;

            ngx_memcpy(us, uscfp[i]->servers->elts,
                sizeof(ngx_http_upstream_server_t) * uscfp[i]->servers->nelts);

            if (backup) {
                for (j = 0; j < uscfp[i]->servers->nelts; j++) {
                    us[j].backup = 1;
                }
            }

            return NGX_CONF_OK;
        }
    }

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "upstream \"%V\" not found",
                       &value[1]);
    return NGX_CONF_ERROR;
}


static char *
ngx_http_combine_server_singlets(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_uint_t                      i, j;
    ngx_http_upstream_srv_conf_t   *uscf;
    ngx_str_t                      *value;
    ngx_str_t                       suf = ngx_null_string, oldsuf, newsuf;
    u_char                          buf[128];
    u_char                         *fbuf;
    const char                     *fmt = "%V%d";
    ngx_uint_t                      flen;
    ngx_uint_t                      byname = 0;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    if (uscf->servers == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "no server so far declared to build singlets");
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    if (cf->args->nelts > 1) {
        suf = value[1];

#if nginx_version >= 1007002
        if (value[1].len == 6
            && ngx_strncmp(value[1].data, "byname", 6) == 0)
        {
            fmt = "%V";
            byname = 1;
        }
#endif

        if (cf->args->nelts > 2) {
            if (byname > 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "second parameter is not permitted when "
                                   "using \"byname\" value");
                return NGX_CONF_ERROR;
            }

#if nginx_version >= 1007002
            if (value[2].len == 6
                && ngx_strncmp(value[2].data, "byname", 6) == 0)
            {
                fmt = "%V%V";
                byname = 2;
            } else
#endif
            {
                if (ngx_atoi(value[2].data, value[2].len) == NGX_ERROR) {
                    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                       "second parameter \"%V\" must be an "
                                       "integer value", &value[2]);
                    return NGX_CONF_ERROR;
                }

                flen = value[2].len + 6;
                fbuf = ngx_pnalloc(cf->pool, flen);
                if (fbuf == NULL) {
                    return NGX_CONF_ERROR;
                }
                ngx_snprintf(fbuf, flen, "%s%V%s", "%V%0", &value[2], "d");
                fmt = (const char *) fbuf;
            }
        }
    }

    oldsuf = suf;

    for (i = 0; i < uscf->servers->nelts; i++) {
        ngx_url_t                      u;
        ngx_http_upstream_srv_conf_t  *uscfn;
        ngx_http_upstream_server_t    *usn;
        u_char                        *end;

#if nginx_version >= 1007002
        if (byname > 0) {
            u_char      *start;
            ngx_uint_t   start_idx;

            suf = ((ngx_http_upstream_server_t *) uscf->servers->elts)[i].name;
            start = ngx_strlchr(suf.data, suf.data + suf.len, ':');
            if (start != NULL) {
                start_idx = start - suf.data;
                newsuf.len = suf.len;
                newsuf.data = ngx_pnalloc(cf->pool, suf.len);
                if (newsuf.data == NULL) {
                    return NGX_CONF_ERROR;
                }
                ngx_memcpy(newsuf.data, suf.data, suf.len);
                suf = newsuf;
                suf.data[start_idx] = '_';
                for (j = start_idx + 1; j < suf.len; j++) {
                    if (suf.data[j] == ':') {
                        suf.data[j] = '_';
                    }
                }
            }

            switch (byname) {
            case 1:
                end = ngx_snprintf(buf, sizeof(buf), fmt, &suf);
                break;
            case 2:
            default:
                end = ngx_snprintf(buf, sizeof(buf), fmt, &oldsuf, &suf);
                break;
            }
        } else
#endif
        {
            end = ngx_snprintf(buf, sizeof(buf), fmt, &suf, i + 1);
        }

        ngx_memzero(&u, sizeof(ngx_url_t));
        u.host.len = uscf->host.len + (end - buf);
        u.host.data = ngx_pnalloc(cf->pool, u.host.len);
        if (u.host.data == NULL)
            return NGX_CONF_ERROR;

        ngx_memcpy(u.host.data, uscf->host.data, uscf->host.len);
        ngx_memcpy(u.host.data + uscf->host.len, buf, end - buf);
        u.no_resolve = 1;
        u.no_port = 1;

        uscfn = ngx_http_upstream_add(cf, &u, NGX_HTTP_UPSTREAM_CREATE);
        if (uscfn == NULL) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "failed to add upstream \"%V\"", &u.host);
            return NGX_CONF_ERROR;
        }

        uscfn->servers = ngx_array_create(cf->pool, uscf->servers->nelts,
                                          sizeof(ngx_http_upstream_server_t));
        if (uscf->servers == NULL)
            return NGX_CONF_ERROR;

        usn = ngx_array_push_n(uscfn->servers, uscf->servers->nelts);
        if (usn == NULL) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "failed to add upstream \"%V\"", &u.host);
            return NGX_CONF_ERROR;
        }

        ngx_memcpy(usn, uscf->servers->elts,
                   sizeof(ngx_http_upstream_server_t) * uscf->servers->nelts);
        for (j = 0; j < uscf->servers->nelts; ++j) {
            usn[j].backup = i == j ? 0 : 1;
        }

        /*ngx_conf_log_error(NGX_LOG_ERR, cf, 0,*/
                           /*"created combined upstream \"%V\"", &u.host);*/
    }

    return NGX_CONF_OK;
}

