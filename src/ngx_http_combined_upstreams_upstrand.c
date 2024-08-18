/*
 * =============================================================================
 *
 *       Filename:  ngx_http_combined_upstreams_upstrand.c
 *
 *    Description:  upstrand is a super-layer of nginx upstreams
 *
 *        Version:  2.2
 *        Created:  13.08.2020 16:38:53
 *
 *         Author:  Alexey Radkov (), 
 *        Company:  
 *
 * =============================================================================
 */

#include "ngx_http_combined_upstreams_module.h"
#include "ngx_http_combined_upstreams_upstrand.h"

#define UPSTREAM_VARS_SIZE (sizeof(upstream_vars) / sizeof(upstream_vars[0]))


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


typedef struct {
    upstream_finalize_request_pt             upstream_finalize_request;
    ngx_uint_t                               last:1;
    ngx_uint_t                               intercepted:1;
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
    ngx_uint_t                               start_time_done:1;
} ngx_http_upstrand_request_ctx_t;


typedef struct {
    ngx_int_t                                value;
    ngx_str_t                                uri;
} ngx_http_upstrand_intercept_status_data_t;


typedef struct {
    ngx_http_upstrand_request_common_ctx_t   common;
} ngx_http_upstrand_subrequest_ctx_t;


typedef struct {
    ngx_str_t                                key;
    ngx_int_t                                index;
} ngx_http_upstrand_var_handle_t;


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


static ngx_int_t ngx_http_upstrand_intercept_statuses(ngx_http_request_t *r,
    ngx_array_t *statuses, ngx_int_t status, ngx_str_t *uri);
static ngx_int_t ngx_http_upstrand_response_header_filter(
    ngx_http_request_t *r);
static ngx_int_t ngx_http_upstrand_response_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in);
static void ngx_http_upstrand_check_upstream_vars(ngx_http_request_t *r,
    ngx_int_t rc);
static ngx_int_t ngx_http_upstrand_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_get_dynamic_upstrand_value(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static char *ngx_http_upstrand(ngx_conf_t *cf, ngx_command_t *dummy,
    void *conf);
static char *ngx_http_upstrand_add_upstream(ngx_conf_t *cf,
    ngx_array_t *upstreams, ngx_str_t *name, time_t blacklist_interval);
#if (NGX_PCRE)
static char *ngx_http_upstrand_regex_add_upstream(ngx_conf_t *cf,
    ngx_array_t *upstreams, ngx_str_t *name, time_t blacklist_interval);
#endif
static ngx_http_upstrand_subrequest_ctx_t
    *ngx_http_get_upstrand_subrequest_ctx(ngx_http_request_t *r,
    ngx_http_request_t *ctx_r);


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;


extern ngx_module_t  ngx_http_proxy_module;

#if 0
extern ngx_module_t  ngx_http_uwsgi_module;
extern ngx_module_t  ngx_http_fastcgi_module;
extern ngx_module_t  ngx_http_scgi_module;

#if (NGX_HTTP_V2)
extern ngx_module_t  ngx_http_grpc_module;
#endif
#endif

static ngx_uint_t  ngx_http_upstrand_gw_modules[5];

#define UPSTRAND_EFFECTIVE_GW_MODULES_SIZE 1


ngx_int_t
ngx_http_upstrand_init(ngx_conf_t *cf)
{
#ifdef NGX_HTTP_COMBINED_UPSTREAMS_PERSISTENT_UPSTRAND_INTERCEPT_CTX
    ngx_http_combined_upstreams_main_conf_t  *mcf;

    mcf = ngx_http_conf_get_module_main_conf(cf,
                                    ngx_http_combined_upstreams_module);

    if (ngx_http_register_easy_ctx(cf, &ngx_http_combined_upstreams_module,
                                   &mcf->upstrand_intercept_ctx)
        == NGX_ERROR)
    {
        return NGX_ERROR;
    }
#endif

    ngx_http_upstrand_gw_modules[0] = ngx_http_proxy_module.ctx_index;

#if 0
    ngx_http_upstrand_gw_modules[1] = ngx_http_uwsgi_module.ctx_index;
    ngx_http_upstrand_gw_modules[3] = ngx_http_fastcgi_module.ctx_index;
    ngx_http_upstrand_gw_modules[4] = ngx_http_scgi_module.ctx_index;

#if (NGX_HTTP_V2)
    ngx_http_upstrand_gw_modules[4] = ngx_http_grpc_module.ctx_index;
#endif
#endif

    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_upstrand_response_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_upstrand_response_body_filter;

    return NGX_OK;
}


static ngx_int_t
ngx_http_upstrand_intercept_statuses(ngx_http_request_t *r,
                                     ngx_array_t *statuses, ngx_int_t status,
                                     ngx_str_t *uri)
{
    ngx_uint_t                                  i;
    ngx_http_upstrand_intercept_status_data_t  *elts;

    elts = statuses->elts;

    for (i = 0; i < statuses->nelts; i++) {
        if ((elts[i].value == -4 && status >= 400 && status < 500)
            ||
            (elts[i].value == -5 && status >= 500 && status < 600)
            ||
            elts[i].value == status)
        {
            *uri = elts[i].uri;
            return NGX_OK;
        }
    }

    return NGX_DECLINED;
}


static ngx_int_t
ngx_http_upstrand_response_header_filter(ngx_http_request_t *r)
{
    ngx_uint_t                                i;
#ifdef NGX_HTTP_COMBINED_UPSTREAMS_PERSISTENT_UPSTRAND_INTERCEPT_CTX
    ngx_http_combined_upstreams_main_conf_t  *mcf;
#endif
    ngx_http_request_t                       *sr;
    ngx_http_upstrand_request_ctx_t          *ctx;
    ngx_http_upstrand_subrequest_ctx_t       *sr_ctx;
    ngx_http_upstrand_request_common_ctx_t   *common;
    ngx_http_upstream_t                      *u;
    ngx_int_t                                 status;
    ngx_int_t                                *next_upstream_statuses;
    ngx_uint_t                                is_next_upstream_status;
    ngx_http_upstrand_status_data_t          *status_data;
    ngx_int_t                                 rc;

    static const ngx_str_t    intercepted = ngx_string("<intercepted>");

    ctx = ngx_http_get_module_ctx(r->main, ngx_http_combined_upstreams_module);
    if (ctx == NULL) {
        return ngx_http_next_header_filter(r);
    }

    if (r != ctx->r) {
        sr_ctx = ngx_http_get_upstrand_subrequest_ctx(r, ctx->r);
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
    status_data->upstream = common->intercepted ?
            intercepted : ctx->cur_upstream;
    ngx_memzero(&status_data->data, sizeof(status_data->data));

    if (u) {
        if (u->finalize_request) {
            common->upstream_finalize_request = u->finalize_request;
        }
        /* BEWARE: the finalizer won't run when proxy_intercept_errors is on */
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
            if (!ctx->start_time_done) {
                if (u) {
                    ctx->start_time = u->peer.start_time;
                }
                ctx->start_time_done = 1;
            }

            if (!common->last) {
                if (ctx->upstrand->next_upstream_timeout
                    && ngx_current_msec - ctx->start_time
                        >= ctx->upstrand->next_upstream_timeout)
                {
                    common->last = 1;

                } else {
                    if (ngx_http_subrequest(r, &ctx->r->uri, &ctx->r->args,
                                            &sr, NULL,
                                            NGX_HTTP_SUBREQUEST_CLONE)
                        != NGX_OK)
                    {
                        return NGX_ERROR;
                    }

                    /* subrequest must use method of the original request */
                    sr->method = r->method;
                    sr->method_name = r->method_name;

                    sr->header_in = r->header_in;

                    /* adjust pointers to last elements in lists when needed */
                    if (r->headers_in.headers.last
                        == &r->headers_in.headers.part)
                    {
                        sr->headers_in.headers.last =
                                &sr->headers_in.headers.part;
                    }

                    return NGX_OK;
                }
            }
        }

    } else {
        common->last = 1;
    }

    if (common->last) {
        ngx_str_t  failover_uri;

        if (ctx->upstrand->intercept_statuses.nelts > 0
            && !common->intercepted
            && ngx_http_upstrand_intercept_statuses(r,
                    &ctx->upstrand->intercept_statuses, status, &failover_uri)
                == NGX_OK)
        {
            common->last = 0;

            sr_ctx = ngx_pcalloc(ctx->r->pool,
                                 sizeof(ngx_http_upstrand_subrequest_ctx_t));
            if (sr_ctx == NULL) {
                return NGX_ERROR;
            }
            sr_ctx->common.last = 1;
            sr_ctx->common.intercepted = 1;

            /* BEWARE: no special adjustments in the failover subrequest */
            rc = ngx_http_subrequest(r, &failover_uri, NULL, &sr, NULL, 0);
            if (rc != NGX_OK) {
                return NGX_ERROR;
            }

            ngx_http_set_ctx(sr, sr_ctx, ngx_http_combined_upstreams_module);

#ifdef NGX_HTTP_COMBINED_UPSTREAMS_PERSISTENT_UPSTRAND_INTERCEPT_CTX
            mcf = ngx_http_get_module_main_conf(r,
                                        ngx_http_combined_upstreams_module);

            if (ngx_http_set_easy_ctx(ctx->r, &mcf->upstrand_intercept_ctx,
                                      sr_ctx) == NGX_ERROR)
            {
                return NGX_ERROR;
            }
#else
            /* location must be protected from interceptions by error_page,
             * this is achieved by setting error_page flag for the request */
            sr->error_page = 1;
#endif

            return NGX_OK;
        }

        if (r != ctx->r) {
            /* copy HTTP headers to main request */
            ctx->r->headers_out = r->headers_out;
            /* FIXME: must other fields like upstream_states be copied too? */
            /* BEWARE: while upstream_states is not copied, it will contain data
             * regarding only the first upstream visited; when copied, it will
             * contain data regarding the last upstream visited */

            /* adjust pointers to last elements in lists when needed */
            if (r->headers_out.headers.last == &r->headers_out.headers.part) {
                ctx->r->headers_out.headers.last =
                        &ctx->r->headers_out.headers.part;
            }
#if nginx_version >= 1013002
            if (r->headers_out.trailers.last == &r->headers_out.trailers.part) {
                ctx->r->headers_out.trailers.last =
                        &ctx->r->headers_out.trailers.part;
            }
#endif

            return ngx_http_next_header_filter(ctx->r);
        }
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
        sr_ctx = ngx_http_get_upstrand_subrequest_ctx(r, ctx->r);
        if (sr_ctx == NULL) {
            return NGX_ERROR;
        }
    }
    common = r == ctx->r ? &ctx->common : &sr_ctx->common;

    u = r->upstream;

    if (!common->last) {
        /* if upstream buffering is off then its out_bufs must be updated
         * right here! (at least in nginx 1.8.0) */
        if (u && !u->buffering) {
            u->out_bufs = NULL;
        }
        return NGX_OK;
    }

    if (in != NULL) {
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

        if (r == ctx->r) {
            /* main request data must be always accessible */
            status->data[i].data = var->data;
        } else {
            status->data[i].data = ngx_pnalloc(ctx->r->pool, var->len);
            if (status->data[i].data == NULL) {
                return;
            }
            ngx_memcpy(status->data[i].data, var->data, var->len);
        }
    }

    if (r != ctx->r) {
        sr_ctx = ngx_http_get_upstrand_subrequest_ctx(r, ctx->r);
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
    ngx_http_combined_upstreams_loc_conf_t   *lcf;
    ngx_http_upstrand_request_ctx_t          *ctx;
    ngx_http_upstrand_subrequest_ctx_t       *sr_ctx;
    ngx_http_upstrand_request_common_ctx_t   *common;
    ngx_http_upstrand_upstream_conf_t        *u_elts, *bu_elts;
    ngx_uint_t                                u_nelts, bu_nelts;
    ngx_http_upstream_main_conf_t            *umcf;
    ngx_http_upstream_srv_conf_t            **uscfp;
    ngx_http_upstream_conf_t                 *u;
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

    /* location must be protected from interceptions by error_page,
     * this is achieved by setting error_page flag for the request */
    r->error_page = 1;

    lcf = ngx_http_get_module_loc_conf(r, ngx_http_combined_upstreams_module);

    if (!lcf->upstrand_gw_modules_checked) {
        for (i = 0; i < UPSTRAND_EFFECTIVE_GW_MODULES_SIZE; i++) {
            /* FIXME: this is a dirty hack: getting proxy module's location
             * configuration as an upstream configuration is safe only if the
             * upstream configuration is the first field of the location
             * configuration; fortunately, this is true for proxy, uwsgi,
             * fastcgi, scgi, and grpc modules */
            u = r->loc_conf[ngx_http_upstrand_gw_modules[i]];
            /* location must be protected against X-Accel-Redirect headers */
            u->ignore_headers |= NGX_HTTP_UPSTREAM_IGN_XA_REDIRECT;
        }
        lcf->upstrand_gw_modules_checked = 1;
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

        /* ctx->start_time will be reset to the value of the upstream's first
         * peer's start_time in ngx_http_upstrand_response_header_filter() */
        ctx->start_time = ngx_current_msec;

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
                if (now - bu_elts[cur_bcur].blacklist_last_occurrence
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
            if (now - u_elts[cur_cur].blacklist_last_occurrence
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
        sr_ctx = ngx_http_get_upstrand_subrequest_ctx(r, ctx->r);
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
    ngx_uint_t                               i;
    ngx_int_t                               *index = (ngx_int_t *) data;
    ngx_int_t                                found_idx = NGX_ERROR;
    ngx_http_combined_upstreams_loc_conf_t  *lcf;
    ngx_array_t                             *upstrands;
    ngx_http_upstrand_var_list_elem_t       *upstrands_elts;
    ngx_array_t                             *upstrand_cands;
    ngx_http_upstrand_var_handle_t          *upstrand_cands_elts;
    ngx_http_variable_value_t               *upstrand_var = NULL;
    ngx_str_t                                upstrand_var_name;

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


ngx_int_t
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


ngx_int_t
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


char *
ngx_http_upstrand_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_combined_upstreams_main_conf_t  *mcf = conf;

    char                                     *rv;
    ngx_str_t                                *value, name;
    ngx_conf_t                                save;
    ngx_http_variable_t                      *var;
    ngx_str_t                                 var_name;
    ngx_http_upstrand_conf_t                 *upstrand;
    ngx_http_upstrand_conf_ctx_t              ctx;
    ngx_uint_t                                u_nelts, bu_nelts;

    upstrand = ngx_array_push(&mcf->upstrands);
    if (upstrand == NULL) {
        return NGX_CONF_ERROR;
    }
    ngx_memzero(upstrand, sizeof(ngx_http_upstrand_conf_t));

    if (ngx_array_init(&upstrand->upstreams, cf->pool, 1,
                       sizeof(ngx_http_upstrand_upstream_conf_t))
            != NGX_OK
        || ngx_array_init(&upstrand->b_upstreams, cf->pool, 1,
                          sizeof(ngx_http_upstrand_upstream_conf_t))
            != NGX_OK
        || ngx_array_init(&upstrand->next_upstream_statuses, cf->pool, 1,
                          sizeof(ngx_int_t))
            != NGX_OK
        || ngx_array_init(&upstrand->intercept_statuses, cf->pool, 1,
                          sizeof(ngx_http_upstrand_intercept_status_data_t))
            != NGX_OK)
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

    if (cf->args->nelts == 2) {
        if (value[0].len == 21 &&
            ngx_strncmp(value[0].data, "next_upstream_timeout", 21) == 0)
        {
            time_t  timeout;

            if (ctx->upstrand->next_upstream_timeout) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "duplicate upstrand directive \"%V\"", &value[0]);
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
                                   "duplicate upstrand directive \"%V\"",
                                   &value[0]);
                return NGX_CONF_ERROR;
            }

            for (i = 1; i < cf->args->nelts; i++) {

                if (value[i].len == 12 &&
                    ngx_strncmp(value[i].data, "start_random", 12) == 0)
                {
                    if (done[0]++ > 0) {
                        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                    "bad upstrand directive \"%V\" content",
                                    &value[0]);
                        return NGX_CONF_ERROR;
                    }
                    ctx->upstrand->order = ngx_http_upstrand_order_start_random;
                }

                if (value[i].len == 11 &&
                    ngx_strncmp(value[i].data, "per_request", 11) == 0)
                {
                    if (done[1]++ > 0) {
                        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                    "bad upstrand directive \"%V\" content",
                                    &value[0]);
                        return NGX_CONF_ERROR;
                    }
                    ctx->upstrand->order_per_request = 1;
                }
            }

            if (done[0] + done[1] != cf->args->nelts - 1) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "bad upstrand directive \"%V\" content",
                                   &value[0]);
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
                                           "bad upstrand directive \"%V\" "
                                           "content", &value[0]);
                        return NGX_CONF_ERROR;
                    }
                }

                if (value[i].len > 19 &&
                    ngx_strncmp(value[i].data, "blacklist_interval=", 19) == 0)
                {
                    ngx_str_t  interval = value[i];

                    if (done[1]++ > 0) {
                        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                           "bad upstrand directive \"%V\" "
                                           "content", &value[0]);
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
                                   "bad upstrand directive \"%V\" content",
                                   &value[0]);
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

        if (cf->args->nelts < 2) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "bad upstrand directive \"%V\" content",
                               &value[0]);
            return NGX_CONF_ERROR;
        }

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

    if (value[0].len == 18 &&
        ngx_strncmp(value[0].data, "intercept_statuses", 18) == 0)
    {
        ngx_http_upstrand_intercept_status_data_t  *status;

        if (cf->args->nelts < 3) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "bad upstrand directive \"%V\" content",
                               &value[0]);
            return NGX_CONF_ERROR;
        }

        for (i = 1; i < cf->args->nelts - 1; i++) {
            ngx_uint_t  invalid_status = 0;

            status = ngx_array_push(&ctx->upstrand->intercept_statuses);
            if (status == NULL) {
                return NGX_CONF_ERROR;
            }

            status->uri = value[cf->args->nelts - 1];
            status->value = ngx_atoi(value[i].data, value[i].len);

            if (status->value == NGX_ERROR) {

                if (value[i].len == 3 &&
                    ngx_strncmp(value[i].data + 1, "xx", 2) == 0)
                {
                    switch (value[i].data[0]) {
                    case '4':
                        status->value = -4;
                        break;
                    case '5':
                        status->value = -5;
                        break;
                    default:
                        invalid_status = 1;
                        break;
                    }

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

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "bad upstrand directive \"%V\"",
                       &value[0]);

    return NGX_CONF_ERROR;
}


char *
ngx_http_dynamic_upstrand(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_combined_upstreams_loc_conf_t  *lcf = conf;

    ngx_uint_t                               i;
    ngx_str_t                               *value;
    ngx_http_variable_t                     *v;
    ngx_http_upstrand_var_list_elem_t       *resvar;
    ngx_int_t                                v_idx;
    ngx_uint_t                              *v_idx_ptr;

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
                       sizeof(ngx_http_upstrand_var_handle_t)) != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    for (i = 2; i < cf->args->nelts; i++) {
        ngx_http_upstrand_var_handle_t  *res;
        ngx_int_t                        index = NGX_ERROR;
        ngx_uint_t                       isvar;

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


static ngx_http_upstrand_subrequest_ctx_t*
ngx_http_get_upstrand_subrequest_ctx(ngx_http_request_t *r,
                                     ngx_http_request_t *ctx_r)
{
#ifdef NGX_HTTP_COMBINED_UPSTREAMS_PERSISTENT_UPSTRAND_INTERCEPT_CTX
    ngx_http_combined_upstreams_main_conf_t  *mcf;
#endif
    ngx_http_upstrand_subrequest_ctx_t       *sr_ctx;

    sr_ctx = ngx_http_get_module_ctx(r, ngx_http_combined_upstreams_module);

#ifdef NGX_HTTP_COMBINED_UPSTREAMS_PERSISTENT_UPSTRAND_INTERCEPT_CTX
    mcf = ngx_http_get_module_main_conf(r, ngx_http_combined_upstreams_module);

    /* restore context that could have been erased by internal redirections */
    if (sr_ctx == NULL) {
        sr_ctx = ngx_http_get_easy_ctx(ctx_r, &mcf->upstrand_intercept_ctx);
    }
#endif

    return sr_ctx;
}

