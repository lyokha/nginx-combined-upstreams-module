/*
 * =============================================================================
 *
 *       Filename:  ngx_http_combined_upstreams_module.c
 *
 *    Description:  nginx module for building combined upstreams
 *
 *                  The module introduces two directives 'add_upstream' and
 *                  'combine_server_singlets' available inside upstream
 *                  configuration blocks.
 *
 *                  Directive 'add_upstream'
 *                  ------------------------
 *                  Populates the host upstream with servers listed in an
 *                  already defined upstream specified by the mandatory 1st
 *                  parameter of the directive. Optional 2nd parameter may have
 *                  only value 'backup' which marks all servers of the sourced
 *                  upstream as backups.
 *
 *                  An example:
 *
 *                  upstream  combined {
 *                      add_upstream    upstream1;            # src upstream 1
 *                      add_upstream    upstream2;            # src upstream 2
 *                      server          some_another_server;  # if needed
 *                      add_upstream    upstream3 backup;     # src upstream 3
 *                  }
 *
 *                  Directive 'combine_server_singlets'
 *                  -----------------------------------
 *                  Produces multiple 'singlet' upstreams from servers so far
 *                  defined in the host upstream. A 'singlet' upstream contains
 *                  only one active server whereas other servers are marked as
 *                  backups. If no parameters were passed then the singlet
 *                  upstreams will have names of the host upstream appended by
 *                  the ordering number of the active server in the host
 *                  upstream. Optional 2 parameters can be used to adjust their
 *                  names. The 1st parameter is a suffix added after the name of
 *                  the host upstream and before the ordering number. The 2nd
 *                  parameter must be an integer value which defines
 *                  'zero-alignment' of the ordering number, for example if it
 *                  has value 2 then the ordering numbers could be
 *                  '01', '02', ..., '10', ... '100' ...
 *
 *                  An example:
 *
 *                  upstream  uhost {
 *                      server                   s1;
 *                      server                   s2;
 *                      server                   s3 backup;
 *                      server                   s4;
 *                      # build singlet upstreams uhost_single_01,
 *                      # uhost_single_02, uhost_single_03 and uhost_single_04
 *                      combine_server_singlets  _single_ 2;
 *                  }
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


static char *ngx_http_add_upstream(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_combine_server_singlets(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);


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

      ngx_null_command
};


static ngx_http_module_t  ngx_http_combined_upstreams_module_ctx = {
    NULL,                                    /* preconfiguration */
    NULL,                                    /* postconfiguration */

    NULL,                                    /* create main configuration */
    NULL,                                    /* init main configuration */

    NULL,                                    /* create server configuration */
    NULL,                                    /* merge server configuration */

    NULL,                                    /* create location configuration */
    NULL                                     /* merge location configuration */
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
            uscf->flags |= NGX_HTTP_UPSTREAM_BACKUP;
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
                if (uscf->servers == NULL) {
                    return NGX_CONF_ERROR;
                }
            }
            us = ngx_array_push_n(uscf->servers, uscfp[i]->servers->nelts);
            if (us == NULL) {
                return NGX_CONF_ERROR;
            }
            ngx_memcpy(us, uscfp[i]->servers->elts,
                sizeof(ngx_http_upstream_server_t) * uscfp[i]->servers->nelts);
            uscf->flags |= uscfp[i]->flags;
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
    ngx_http_upstream_main_conf_t  *usmf;
    ngx_http_upstream_srv_conf_t   *uscf, **uscfp;
    ngx_http_upstream_server_t     *us;
    ngx_str_t                      *value;
    const char                     *suf = "", *fmt = "%s%d";

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

    if (uscf->servers == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "no server so far declared to build singlets");
        return NGX_CONF_ERROR;
    }

    usmf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_module);
    us = uscf->servers->elts;
    value = cf->args->elts;

    if (cf->args->nelts > 1) {
        unsigned char  *buf = ngx_pnalloc(cf->pool, value[1].len + 1);
        ngx_snprintf(buf, value[1].len, "%V", &value[1]);
        suf = (const char*)buf;
        if (cf->args->nelts > 2) {
            if (ngx_atoi(value[2].data, value[2].len) == NGX_ERROR) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "second parameter \"%V\" is not an integer "
                                   "value", &value[2]);
                return NGX_CONF_ERROR;
            }
            buf = ngx_pnalloc(cf->pool, value[2].len + 6);
            ngx_snprintf(buf, sizeof(buf), "%s%V%s", "%s%0", &value[2], "d");
            fmt = (const char*)buf;
        }
    }

    for (i = 0; i < uscf->servers->nelts; i++) {
        ngx_url_t                      u;
        ngx_http_upstream_srv_conf_t  *uscfn;
        ngx_http_upstream_server_t    *usnp;
        unsigned char                  buf[128];
        unsigned char                 *end;

        ngx_memzero(&u, sizeof(ngx_url_t));
        end = ngx_snprintf(buf, sizeof(buf), fmt, suf, i + 1);
        u.url.len = uscf->host.len + (end - buf);
        u.url.data = ngx_pnalloc(cf->pool, u.url.len);

        if (u.url.data == NULL)
            return NGX_CONF_ERROR;

        ngx_memcpy(u.url.data, uscf->host.data, uscf->host.len);
        ngx_memcpy(u.url.data + uscf->host.len, buf, end - buf);
        u.no_resolve = 1;

        /* uscfp may have changed after addition of a singlet upstream, so it
         * must be re-assigned in every iteration of the outer for-loop */
        uscfp = usmf->upstreams.elts;

        for (j = 0; j < usmf->upstreams.nelts; j++) {
            if (uscfp[j]->host.len == u.url.len &&
                ngx_strncasecmp(uscfp[j]->host.data,
                                u.url.data, u.url.len) == 0) {
                ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                                   "duplicate upstream \"%V\"", &u.url);
                return NGX_CONF_ERROR;
            }
        }

        uscfn = ngx_http_upstream_add(cf, &u, 0);

        if (uscfn == NULL) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "failed to add upstream \"%V\"", &u.url);
            return NGX_CONF_ERROR;
        }

        uscfn->servers = ngx_array_create(cf->pool, uscf->servers->nelts,
                                          sizeof(ngx_http_upstream_server_t));
        if (uscf->servers == NULL)
            return NGX_CONF_ERROR;

        usnp = ngx_array_push_n(uscfn->servers, uscf->servers->nelts);

        if (usnp == NULL) {
            ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                               "failed to add upstream \"%V\"", &u.url);
            return NGX_CONF_ERROR;
        }

        for (j = 0; j < uscf->servers->nelts; ++j) {
            usnp[j] = us[j];
            usnp[j].backup = i == j ? 0 : 1;
        }
    }

    return NGX_CONF_OK;
}

