/*
 * =============================================================================
 *
 *       Filename:  ngx_http_combined_upstreams_module.c
 *
 *    Description:  nginx module for building combined upstreams
 *
 *        Version:  2.0
 *        Created:  05.10.2011 16:06:15
 *
 *         Author:  Alexey Radkov (), 
 *        Company:  
 *
 * =============================================================================
 */

#include "ngx_http_combined_upstreams_module.h"
#include "ngx_http_combined_upstreams_upstrand.h"


typedef struct {
    ngx_http_upstream_init_pt  original_init_upstream;
    ngx_uint_t                 extended_peers_enabled;
} ngx_http_combined_upstreams_srv_conf_t;


static void *ngx_http_combined_upstreams_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_combined_upstreams_create_srv_conf(ngx_conf_t *cf);
static void *ngx_http_combined_upstreams_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_combined_upstreams_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);
static ngx_int_t ngx_http_combined_upstreams_add_vars(ngx_conf_t *cf);
static ngx_int_t ngx_http_combined_upstreams_init(ngx_conf_t *cf);
static char *ngx_http_add_upstream(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_combine_server_singlets(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_extend_single_peers(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_upstream_init_extend_single_peers(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us);


static ngx_command_t  ngx_http_combined_upstreams_commands[] = {

    { ngx_string("add_upstream"),
      NGX_HTTP_UPS_CONF|NGX_CONF_TAKE123,
      ngx_http_add_upstream,
      0,
      0,
      NULL },
    { ngx_string("combine_server_singlets"),
      NGX_HTTP_UPS_CONF|NGX_CONF_NOARGS|NGX_CONF_TAKE123,
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
    { ngx_string("extend_single_peers"),
      NGX_HTTP_UPS_CONF|NGX_CONF_NOARGS,
      ngx_http_extend_single_peers,
      NGX_HTTP_SRV_CONF_OFFSET,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_variable_t  ngx_http_combined_upstreams_vars[] =
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
    ngx_http_combined_upstreams_init,        /* postconfiguration */

    ngx_http_combined_upstreams_create_main_conf,
                                             /* create main configuration */
    NULL,                                    /* init main configuration */

    ngx_http_combined_upstreams_create_srv_conf,
                                             /* create server configuration */
    NULL,                                    /* merge server configuration */

    ngx_http_combined_upstreams_create_loc_conf,
                                             /* create location configuration */
    ngx_http_combined_upstreams_merge_loc_conf
                                             /* merge location configuration */
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


static void *
ngx_http_combined_upstreams_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_combined_upstreams_main_conf_t  *mcf;

    mcf = ngx_pcalloc(cf->pool,
                      sizeof(ngx_http_combined_upstreams_main_conf_t));
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
ngx_http_combined_upstreams_create_srv_conf(ngx_conf_t *cf)
{
    ngx_http_combined_upstreams_srv_conf_t  *scf;

    scf = ngx_pcalloc(cf->pool,
                      sizeof(ngx_http_combined_upstreams_srv_conf_t));

    return scf;
}


static void *
ngx_http_combined_upstreams_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_combined_upstreams_loc_conf_t  *lcf;

    lcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_combined_upstreams_loc_conf_t));
    if (lcf == NULL) {
        return NULL;
    }

    if (ngx_array_init(&lcf->dyn_upstrands, cf->pool, 1,
                       sizeof(ngx_http_upstrand_var_list_elem_t)) != NGX_OK)
    {
        return NULL;
    }

    return lcf;
}


static char *
ngx_http_combined_upstreams_merge_loc_conf(ngx_conf_t *cf, void *parent,
                                           void *child)
{
    ngx_http_combined_upstreams_loc_conf_t  *prev = parent;
    ngx_http_combined_upstreams_loc_conf_t  *conf = child;

    ngx_uint_t                               i;

    for (i = 0; i < prev->dyn_upstrands.nelts; i++) {
        ngx_http_upstrand_var_list_elem_t  *elem;

        elem = ngx_array_push(&conf->dyn_upstrands);
        if (elem == NULL) {
            return NGX_CONF_ERROR;
        }

        *elem = ((ngx_http_upstrand_var_list_elem_t *)
                                                prev->dyn_upstrands.elts)[i];
    }

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_combined_upstreams_add_vars(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var, *v;

    for (v = ngx_http_combined_upstreams_vars; v->name.len; v++) {
        var = ngx_http_add_variable(cf, &v->name, v->flags);
        if (var == NULL) {
            return NGX_ERROR;
        }

        var->get_handler = v->get_handler;
        var->data = v->data;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_combined_upstreams_init(ngx_conf_t *cf)
{
    return ngx_http_upstrand_init(cf);
}


static char *
ngx_http_add_upstream(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_uint_t                      i, j;
    ngx_http_upstream_main_conf_t  *umcf;
    ngx_http_upstream_srv_conf_t   *uscf, **uscfp;
    ngx_http_upstream_server_t     *us;
    ngx_str_t                      *value;
    ngx_uint_t                      backup = 0;
    ngx_int_t                       weight = 0;

    umcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);
    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    uscfp = umcf->upstreams.elts;
    value = cf->args->elts;

    if (value[1].len == uscf->host.len &&
        ngx_strncasecmp(value[1].data, uscf->host.data, value[1].len) == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "upstream \"%V\" makes recursion", &value[1]);
        return NGX_CONF_ERROR;
    }

    for (i = 2; i < cf->args->nelts; i++) {
        if (ngx_strncmp(value[i].data, "backup", 6) == 0) {
            if (backup) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "parameter \"backup\" has been already declared");
                return NGX_CONF_ERROR;
            }
            backup = 1;
        } else if (ngx_strncmp(value[i].data, "weight=", 7) == 0) {
            if (weight) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                            "parameter \"weight\" has been already declared");
                return NGX_CONF_ERROR;
            }
            weight = ngx_atoi(value[i].data + 7, value[i].len - 7);
            if (weight < 1) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                    "parameter \"weight\" must be a positive integer value");
                return NGX_CONF_ERROR;
            }
        } else {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid parameter \"%V\"",
                               &value[i]);
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
            if (weight) {
                for (j = 0; j < uscfp[i]->servers->nelts; j++) {
                    us[j].weight *= weight;
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
    ngx_uint_t                      byname = 0, nobackup = 0;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    if (uscf->servers == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "no server so far declared to build singlets");
        return NGX_CONF_ERROR;
    }

    value = cf->args->elts;

    if (cf->args->nelts > 1) {
        if (value[cf->args->nelts - 1].len == 8
            && ngx_strncmp(value[cf->args->nelts - 1].data, "nobackup", 8) == 0)
        {
            nobackup = 1;
        }
    }

    if (cf->args->nelts > 3 && nobackup == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "invalid parameters");
        return NGX_CONF_ERROR;
    }

    if (cf->args->nelts > 1 + nobackup) {
        suf = value[1];

#if nginx_version >= 1007002
        if (value[1].len == 6
            && ngx_strncmp(value[1].data, "byname", 6) == 0)
        {
            fmt = "%V";
            byname = 1;
        }
#endif

        if (cf->args->nelts > 2 + nobackup) {
            if (byname > 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "setting field width is not permitted when "
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
                                       "field width \"%V\" must be an "
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
            if (nobackup) {
                usn[j].down = i == j ? usn[j].down : 1;
            } else {
                usn[j].backup = i == j ? 0 : 1;
            }
        }

        /*ngx_conf_log_error(NGX_LOG_ERR, cf, 0,*/
                           /*"created combined upstream \"%V\"", &u.host);*/
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_extend_single_peers(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_combined_upstreams_srv_conf_t  *scf = conf;

    ngx_http_upstream_srv_conf_t            *uscf;

    if (scf->extended_peers_enabled) {
        return "is duplicate";
    }

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    scf->original_init_upstream = uscf->peer.init_upstream
                                  ? uscf->peer.init_upstream
                                  : ngx_http_upstream_init_round_robin;
    scf->extended_peers_enabled = 1;

    uscf->peer.init_upstream = ngx_http_upstream_init_extend_single_peers;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_upstream_init_extend_single_peers(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_uint_t                               i, n = 0, m = 0;
    ngx_http_combined_upstreams_srv_conf_t  *scf;
    ngx_http_upstream_server_t              *server, *s;
    ngx_addr_t                              *addr = NULL;

    if (us->servers) {
        server = us->servers->elts;
        for (i = 0; i < us->servers->nelts; i++) {
            if (server[i].backup) {
                n += server[i].naddrs;
            } else {
                m += server[i].naddrs;
            }
        }

        if (n == 1) {
            s = ngx_array_push(us->servers);
            addr = ngx_pcalloc(cf->pool, sizeof(ngx_addr_t));
            if (s == NULL || addr == NULL) {
                return NGX_ERROR;
            }
            ngx_memzero(s, sizeof(ngx_http_upstream_server_t));
            s->addrs = addr;
            s->naddrs = 1;
            s->down = 1;
        }
        if (m == 1) {
            s = ngx_array_push(us->servers);
            if (addr == NULL) {
                addr = ngx_pcalloc(cf->pool, sizeof(ngx_addr_t));
            }
            if (s == NULL || addr == NULL) {
                return NGX_ERROR;
            }
            ngx_memzero(s, sizeof(ngx_http_upstream_server_t));
            s->backup = 1;
            s->addrs = addr;
            s->naddrs = 1;
            s->down = 1;
        }
    }

    scf = ngx_http_conf_upstream_srv_conf(us,
                                          ngx_http_combined_upstreams_module);

    if (scf->original_init_upstream(cf, us) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

