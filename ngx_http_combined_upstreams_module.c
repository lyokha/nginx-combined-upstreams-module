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


typedef struct {
    ngx_array_t                upstrands;
} ngx_http_upstrand_main_conf_t;


typedef struct {
    ngx_array_t                dyn_upstrands;
} ngx_http_upstrand_loc_conf_t;


typedef enum {
    ngx_http_upstrand_order_normal = 0,
    ngx_http_upstrand_order_start_random
} ngx_http_upstrand_order_e;


typedef struct {
    ngx_array_t                upstreams;
    ngx_array_t                b_upstreams;
    ngx_array_t                next_upstream_statuses;
    ngx_int_t                  cur;
    ngx_int_t                  b_cur;
    ngx_http_upstrand_order_e  order;
    ngx_uint_t                 order_per_request:1;
    ngx_uint_t                 debug_intermediate_stages:1;
} ngx_http_upstrand_conf_t;


typedef struct {
    ngx_http_upstrand_conf_t  *upstrand;
    ngx_conf_t                *cf;
    ngx_uint_t                 order_done:1;
} ngx_http_upstrand_conf_ctx_t;


typedef struct {
    ngx_array_t               *next_upstream_statuses;
    ngx_int_t                  start_cur;
    ngx_int_t                  start_bcur;
    ngx_int_t                  cur;
    ngx_int_t                  b_cur;
    ngx_uint_t                 last:1;
    ngx_uint_t                 last_buf:1;
    ngx_uint_t                 debug_intermediate_stages:1;
} ngx_http_upstrand_request_ctx_t;


typedef struct {
    ngx_str_t                  key;
    ngx_int_t                  index;
}  ngx_http_cu_varhandle_t;


typedef struct {
    ngx_array_t                data;
    ngx_int_t                  index;
} ngx_http_cu_varlist_elem_t;


static ngx_int_t ngx_http_upstrand_init(ngx_conf_t *cf);
static void *ngx_http_upstrand_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_upstrand_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_upstrand_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);
static ngx_int_t ngx_http_upstrand_response_header_filter(
    ngx_http_request_t *r);
static ngx_int_t ngx_http_upstrand_response_body_filter(ngx_http_request_t *r,
    ngx_chain_t *in);
static char *ngx_http_upstrand_block(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_upstrand(ngx_conf_t *cf, ngx_command_t *dummy,
    void *conf);
static char *ngx_http_dynamic_upstrand(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_upstrand_add_upstream(ngx_conf_t *cf,
    ngx_array_t *upstreams, ngx_str_t *name);
#if (NGX_PCRE)
static char *ngx_http_upstrand_regex_add_upstream(ngx_conf_t *cf,
    ngx_array_t *upstreams, ngx_str_t *name);
#endif
static ngx_int_t ngx_http_upstrand_variable(ngx_http_request_t *r,
    ngx_http_variable_value_t *v, uintptr_t data);
static ngx_int_t ngx_http_get_dynamic_upstrand_value(ngx_http_request_t *r,
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


static ngx_http_module_t  ngx_http_combined_upstreams_module_ctx = {
    NULL,                                    /* preconfiguration */
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


static ngx_int_t
ngx_http_upstrand_response_header_filter(ngx_http_request_t *r)
{
    ngx_uint_t                        i;
    ngx_http_upstrand_request_ctx_t  *ctx;
    ngx_int_t                         status;
    ngx_http_request_t               *sr;
    ngx_int_t                        *next_upstream_statuses;
    ngx_uint_t                        is_next_upstream_status;


    ctx = ngx_http_get_module_ctx(r->main, ngx_http_combined_upstreams_module);
    if (ctx == NULL) {
        return ngx_http_next_header_filter(r);
    }

    status = r->headers_out.status;

    next_upstream_statuses = ctx->next_upstream_statuses->elts;
    is_next_upstream_status = 0;

    for (i = 0; i < ctx->next_upstream_statuses->nelts; i++) {

        if ((next_upstream_statuses[i] == -4 && status >= 400 && status < 500)
            ||
            (next_upstream_statuses[i] == -5 && status >= 500 && status < 600)
            ||
            status == next_upstream_statuses[i])
        {
            is_next_upstream_status = 1;
            break;
        }
    }

    if (is_next_upstream_status && !ctx->last) {
        if (ngx_http_subrequest(r, &r->main->uri, &r->main->args, &sr, NULL, 0)
            != NGX_OK)
        {
            return NGX_ERROR;
        }

        /* subrequest must use method of the original request */
        sr->method = r->method;
        sr->method_name = r->method_name;

        return NGX_OK;
    } else {
        ctx->last = 1;
    }

    if (r != r->main && ctx->last) {
        /* copy HTTP headers to main request */
        r->main->headers_out = r->headers_out;

        return ngx_http_next_header_filter(r->main);
    }

    return ngx_http_next_header_filter(r);
}


static ngx_int_t
ngx_http_upstrand_response_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_upstrand_request_ctx_t  *ctx;

    ctx = ngx_http_get_module_ctx(r->main, ngx_http_combined_upstreams_module);
    if (ctx == NULL || !ctx->last) {
        return ngx_http_next_body_filter(r, in);
    }

    if (ctx->last_buf) {
        /* FIXME: alternatively we could return NGX_OK, however in this case
         * r->buffered and r->postponed must be unset in order to let the
         * current request flush: see ngx_http_writer(). But would it be enough
         * in all cases? On the other hand, returning NGX_ERROR seems to be
         * pretty harmless to the final response. */
        return NGX_ERROR;
    }

    if (!ctx->debug_intermediate_stages && in != NULL) {
        ngx_chain_t *last = in;
        while (last->next) {
            last = last->next;
        }
        if (last->buf->last_in_chain) {
            last->buf->last_buf = 1;
            ctx->last_buf = 1;
        }
    }

    return ngx_http_next_body_filter(r, in);
}


static ngx_int_t
ngx_http_upstrand_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v,
    uintptr_t data)
{
    ngx_http_upstrand_conf_t  *upstrand = (ngx_http_upstrand_conf_t *) data;

    ngx_str_t                          val;
    ngx_http_upstrand_request_ctx_t   *ctx;
    ngx_int_t                         *u_elts, *bu_elts;
    ngx_uint_t                         u_nelts, bu_nelts;
    ngx_http_upstream_main_conf_t     *usmf;
    ngx_http_upstream_srv_conf_t     **uscfp;
    ngx_uint_t                         backup_cycle = 0;

    ctx = ngx_http_get_module_ctx(r->main, ngx_http_combined_upstreams_module);

    u_nelts = upstrand->upstreams.nelts;
    bu_nelts = upstrand->b_upstreams.nelts;

    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_upstrand_request_ctx_t));
        if (ctx == NULL) {
            return NGX_ERROR;
        }

        ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_upstrand_request_ctx_t));
        ctx->next_upstream_statuses = &upstrand->next_upstream_statuses;
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
        ctx->debug_intermediate_stages = upstrand->debug_intermediate_stages;

        if (u_nelts > 0) {
            if (!upstrand->order_per_request) {
                upstrand->cur = (upstrand->cur + 1) % u_nelts;
            }
        } else {
            backup_cycle = 1;
            if (bu_nelts > 0 && !upstrand->order_per_request) {
                upstrand->b_cur = (upstrand->b_cur + 1) % bu_nelts;
            }
        }

        ngx_http_set_ctx(r, ctx, ngx_http_combined_upstreams_module);

    } else if (r != r->main) {

        if (backup_cycle) {
            if (bu_nelts > 0) {
                ctx->b_cur = (ctx->b_cur + 1) % bu_nelts;
            }
        } else if (u_nelts > 0) {
            ctx->cur = (ctx->cur + 1) % u_nelts;
            if (ctx->cur == ctx->start_cur) {
                backup_cycle = 1;
            }
        }
    }

    if ((bu_nelts == 0 &&
         (ctx->cur + 1) % u_nelts == (ngx_uint_t) ctx->start_cur) ||
        (backup_cycle &&
         (ctx->b_cur + 1) % bu_nelts == (ngx_uint_t) ctx->start_bcur))
    {
        ctx->last = 1;
    }

    usmf = ngx_http_get_module_main_conf(r, ngx_http_upstream_module);
    uscfp = usmf->upstreams.elts;

    u_elts = upstrand->upstreams.elts;
    bu_elts = upstrand->b_upstreams.elts;

    if (backup_cycle) {
        val = uscfp[bu_elts[ctx->b_cur]]->host;
    } else {
        val = uscfp[u_elts[ctx->cur]]->host;
    }

    v->valid = 1;
    v->not_found = 0;
    v->len = val.len;
    v->data = val.data;

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

    if (v == NULL) {
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
    ngx_http_upstrand_loc_conf_t *prev = parent;
    ngx_http_upstrand_loc_conf_t *conf = child;

    ngx_uint_t                    i;

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

    if (ngx_array_init(&upstrand->upstreams, cf->pool, 1, sizeof(ngx_int_t))
        != NGX_OK
        ||
        ngx_array_init(&upstrand->b_upstreams, cf->pool, 1, sizeof(ngx_int_t))
        != NGX_OK
        ||
        ngx_array_init(&upstrand->next_upstream_statuses, cf->pool, 1,
                       sizeof(ngx_int_t))
        != NGX_OK)
    {
        return NGX_CONF_ERROR;
    }

    upstrand->order = ngx_http_upstrand_order_normal;

    value = cf->args->elts;
    name = value[1];

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
    ngx_int_t                     *idx;
    ngx_str_t                     *value;
    ngx_http_upstrand_conf_ctx_t  *ctx;
    ngx_uint_t                     check_order = 0;

    value = cf->args->elts;
    ctx = cf->ctx;

    if (cf->args->nelts == 1) {
        if (value[0].len == 25 &&
            ngx_strncmp(value[0].data, "debug_intermediate_stages", 25) == 0)
        {
            ctx->upstrand->debug_intermediate_stages = 1;
            return NGX_CONF_OK;
        }
    }

    if (cf->args->nelts == 2) {
        if (value[0].len == 8 && ngx_strncmp(value[0].data, "upstream", 8) == 0)
        {
            return ngx_http_upstrand_add_upstream(ctx->cf,
                        &ctx->upstrand->upstreams, &value[1]);
        }
        check_order = 1;
    }

    if (cf->args->nelts == 3) {
        if (value[0].len == 8 && value[2].len == 6 &&
            ngx_strncmp(value[0].data, "upstream", 8) == 0 &&
            ngx_strncmp(value[2].data, "backup", 6) == 0)
        {
            return ngx_http_upstrand_add_upstream(ctx->cf,
                        &ctx->upstrand->b_upstreams, &value[1]);
        }
        check_order = 1;
    }

    if (check_order) {

        if (value[0].len == 5 && ngx_strncmp(value[0].data, "order", 5) == 0) {
            ngx_uint_t per_request_done = 0;

            if (ctx->order_done) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "duplicate upstrand directive \"order\"");
                return NGX_CONF_ERROR;
            }

            for (i = 1; i < cf->args->nelts; i++) {

                if (value[i].len == 12 &&
                    ngx_strncmp(value[i].data, "start_random", 12) == 0)
                {
                    ctx->upstrand->order = ngx_http_upstrand_order_start_random;
                }

                if (value[i].len == 11 &&
                    ngx_strncmp(value[i].data, "per_request", 11) == 0)
                {
                    if (per_request_done) {
                        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                    "bad upstrand directive \"order\" content");
                        return NGX_CONF_ERROR;
                    }
                    ctx->upstrand->order_per_request = 1;
                    per_request_done = 1;
                }
            }

            if (cf->args->nelts == 3 && !per_request_done) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "bad upstrand directive \"order\" content");
                return NGX_CONF_ERROR;
            }

            ctx->order_done = 1;
            return NGX_CONF_OK;
        }
    }

    if (value[0].len == 22 &&
        ngx_strncmp(value[0].data, "next_upstream_statuses", 22) == 0)
    {
        for (i = 1; i < cf->args->nelts; i++) {
            ngx_uint_t  invalid_status = 0;

            idx = ngx_array_push(&ctx->upstrand->next_upstream_statuses);
            if (idx == NULL) {
                return NGX_CONF_ERROR;
            }

            *idx = ngx_atoi(value[i].data, value[i].len);

            if (*idx == NGX_ERROR) {

                if (value[i].len == 3 &&
                    ngx_strncmp(value[i].data + 1, "xx", 2) == 0) {
                    switch (value[i].data[0]) {
                    case '4':
                        *idx = -4;
                        break;
                    case '5':
                        *idx = -5;
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
    ngx_str_t *name)
{
    ngx_uint_t                       i;
    ngx_int_t                       *idx, found_idx;
    ngx_http_upstream_main_conf_t   *usmf;
    ngx_http_upstream_srv_conf_t   **uscfp;

#if (NGX_PCRE)
    if (name->len > 1 && name->data[0] == '~') {
        name->len -= 1;
        name->data += 1;

        return ngx_http_upstrand_regex_add_upstream(cf, upstreams, name);
    }
#endif

    usmf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);
    uscfp = usmf->upstreams.elts;

    found_idx = NGX_ERROR;

    for (i = 0; i < usmf->upstreams.nelts; i++) {
        if (uscfp[i]->host.len == name->len &&
            ngx_strncasecmp(uscfp[i]->host.data, name->data, name->len) == 0) {
            found_idx = i;
            break;
        }
    }

    if (found_idx == NGX_ERROR) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "upstream \"%V\" is not found",
                           name);
        return NGX_CONF_ERROR;
    }

    for (i = 0; i < upstreams->nelts; i++) {
        ngx_int_t  *registered = upstreams->elts;
        if (found_idx == registered[i]) {
            return NGX_CONF_OK;
        }
    }

    idx = ngx_array_push(upstreams);
    if (idx == NULL) {
        return NGX_CONF_ERROR;
    }

    *idx = found_idx;

    return NGX_CONF_OK;
}


#if (NGX_PCRE)

static char *
ngx_http_upstrand_regex_add_upstream(ngx_conf_t *cf, ngx_array_t *upstreams,
    ngx_str_t *name)
{
    ngx_uint_t                       i, j;
    ngx_int_t                       *idx;
    ngx_http_upstream_main_conf_t   *usmf;
    ngx_http_upstream_srv_conf_t   **uscfp;
    ngx_regex_compile_t              rc;
    u_char                           errstr[NGX_MAX_CONF_ERRSTR];
    ngx_http_regex_t                *re;

    ngx_memzero(&rc, sizeof(ngx_regex_compile_t));
    rc.pattern = *name;
    rc.err.len = NGX_MAX_CONF_ERRSTR;
    rc.err.data = errstr;

    re = ngx_http_regex_compile(cf, &rc);

    if (re == NULL) {
        return NGX_CONF_ERROR;
    }
    
    usmf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);
    uscfp = usmf->upstreams.elts;

    for (i = 0; i < usmf->upstreams.nelts; i++) {
        ngx_uint_t  is_registered = 0;

        if (ngx_regex_exec(re->regex, &uscfp[i]->host, NULL, 0)
            != NGX_REGEX_NO_MATCHED)
        {
            for (j = 0; j < upstreams->nelts; j++) {
                ngx_int_t  *registered = upstreams->elts;

                if ((ngx_int_t) i == registered[j]) {
                    is_registered = 1;
                    break;
                }
            }

            if (is_registered) {
                continue;
            }

            idx = ngx_array_push(upstreams);
            if (idx == NULL) {
                return NGX_CONF_ERROR;
            }

            *idx = i;
        }
    }

    return NGX_CONF_OK;
}

#endif


static char *
ngx_http_add_upstream(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_uint_t                      i, j;
    ngx_http_upstream_main_conf_t  *usmf;
    ngx_http_upstream_srv_conf_t   *uscf, **uscfp;
    ngx_http_upstream_server_t     *us;
    ngx_str_t                      *value;
    ngx_uint_t                      backup = 0;

    usmf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);
    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    uscfp = usmf->upstreams.elts;
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

    for (i = 0; i < usmf->upstreams.nelts; i++) {
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
    const char                     *suf = "", *fmt = "%s%d";

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    if (uscf->servers == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "no server so far declared to build singlets");
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    if (cf->args->nelts > 1) {
        unsigned char  *buf = ngx_pnalloc(cf->pool, value[1].len + 1);

        ngx_snprintf(buf, value[1].len, "%V", &value[1]);
        suf = (const char *) buf;

        if (cf->args->nelts > 2) {
            if (ngx_atoi(value[2].data, value[2].len) == NGX_ERROR) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "second parameter \"%V\" must be an integer "
                                   "value", &value[2]);
                return NGX_CONF_ERROR;
            }

            buf = ngx_pnalloc(cf->pool, value[2].len + 6);
            ngx_snprintf(buf, sizeof(buf), "%s%V%s", "%s%0", &value[2], "d");
            fmt = (const char *) buf;
        }
    }

    for (i = 0; i < uscf->servers->nelts; i++) {
        ngx_url_t                      u;
        ngx_http_upstream_srv_conf_t  *uscfn;
        ngx_http_upstream_server_t    *usn;
        unsigned char                  buf[128];
        unsigned char                 *end;

        ngx_memzero(&u, sizeof(ngx_url_t));
        end = ngx_snprintf(buf, sizeof(buf), fmt, suf, i + 1);

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
    }

    return NGX_CONF_OK;
}

