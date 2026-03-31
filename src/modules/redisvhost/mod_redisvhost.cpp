/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2013 - 2022  LiteSpeed Technologies, Inc.                 *
*                                                                            *
*    This program is free software: you can redistribute it and/or modify    *
*    it under the terms of the GNU General Public License as published by    *
*    the Free Software Foundation, either version 3 of the License, or       *
*    (at your option) any later version.                                     *
*                                                                            *
*    This program is distributed in the hope that it will be useful,         *
*    but WITHOUT ANY WARRANTY; without even the implied warranty of          *
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the            *
*    GNU General Public License for more details.                            *
*                                                                            *
*    You should have received a copy of the GNU General Public License       *
*    along with this program. If not, see http://www.gnu.org/licenses/.      *
*****************************************************************************/

/**
 * mod_redisvhost - Redis-backed dynamic virtual hosting module for
 * OpenLiteSpeed.
 *
 * This module looks up virtual host configuration from Redis on incoming
 * requests, enabling dynamic mass hosting without restarting the server.
 *
 * Redis key format:  {prefix}{hostname}
 * Redis value:       JSON object with vhost configuration fields:
 *   {
 *     "docRoot":    "/var/www/example.com/public_html",
 *     "phpVersion": "lsphp80",
 *     "accessLog":  "/var/log/ols/example.com-access.log",
 *     "errorLog":   "/var/log/ols/example.com-error.log",
 *     "aliases":    "www.example.com",
 *     "uid":        1001,
 *     "gid":        1001,
 *     "sslCert":    "/etc/ssl/certs/example.com.crt",
 *     "sslKey":     "/etc/ssl/private/example.com.key"
 *   }
 *
 * Configuration directives (server or vhost level):
 *   redisVHostEnable     0|1          (default 0)
 *   redisVHostServer     host:port    (default 127.0.0.1:6379)
 *   redisVHostPassword   string       (optional)
 *   redisVHostTTL        seconds      (default 300)
 *   redisVHostKeyPrefix  string       (default "vhost:")
 */

#include <ls.h>
#include <lsr/ls_str.h>
#include <lsr/ls_strtool.h>

#include <hiredis/hiredis.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>


#define MNAME       mod_redisvhost
extern lsi_module_t MNAME;
#define MODULE_VERSION_INFO     "1.0"

/* --------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------- */

#define DEF_REDIS_HOST      "127.0.0.1"
#define DEF_REDIS_PORT      6379
#define DEF_REDIS_TTL       300
#define DEF_REDIS_PREFIX    "vhost:"
#define MAX_HOST_LEN        256
#define MAX_KEY_LEN         512
#define MAX_JSON_VALUE_LEN  4096
#define MAX_PATH_LEN_RV     1024

#define REDIS_CONNECT_TIMEOUT_SEC   2
#define REDIS_CMD_TIMEOUT_SEC       1
#define REDIS_RECONNECT_INTERVAL    5


struct redis_vhost_conf_t
{
    int   enabled;
    char *host;
    int   port;
    char *password;
    int   ttl;
    char *key_prefix;
};


/* --------------------------------------------------------------------
 * In-memory cache for Redis lookups
 * -------------------------------------------------------------------- */

struct cached_vhost_t
{
    char        hostname[MAX_HOST_LEN];
    char        doc_root[MAX_PATH_LEN_RV];
    char        php_version[64];
    char        access_log[MAX_PATH_LEN_RV];
    char        error_log[MAX_PATH_LEN_RV];
    char        aliases[MAX_HOST_LEN * 4];
    int         uid;
    int         gid;
    char        ssl_cert[MAX_PATH_LEN_RV];
    char        ssl_key[MAX_PATH_LEN_RV];
    time_t      expire_time;
    cached_vhost_t *next;
};


/* Maximum cache buckets */
#define CACHE_BUCKETS   256

struct redis_cache_t
{
    cached_vhost_t *buckets[CACHE_BUCKETS];
    pthread_mutex_t lock;
    int             count;
};


/* --------------------------------------------------------------------
 * Global state
 * -------------------------------------------------------------------- */

static redisContext       *s_redis_ctx = NULL;
static pthread_mutex_t     s_redis_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t              s_last_connect_attempt = 0;
static redis_cache_t       s_cache;
static int                 s_initialized = 0;


/* --------------------------------------------------------------------
 * Utility: simple DJB2 hash
 * -------------------------------------------------------------------- */
static unsigned int djb2_hash(const char *str)
{
    unsigned int hash = 5381;
    int c;
    while ((c = (unsigned char)*str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}


/* --------------------------------------------------------------------
 * Cache operations
 * -------------------------------------------------------------------- */

static void cache_init()
{
    memset(&s_cache, 0, sizeof(s_cache));
    pthread_mutex_init(&s_cache.lock, NULL);
}


static void cache_destroy()
{
    pthread_mutex_lock(&s_cache.lock);
    for (int i = 0; i < CACHE_BUCKETS; ++i)
    {
        cached_vhost_t *p = s_cache.buckets[i];
        while (p)
        {
            cached_vhost_t *tmp = p;
            p = p->next;
            free(tmp);
        }
        s_cache.buckets[i] = NULL;
    }
    s_cache.count = 0;
    pthread_mutex_unlock(&s_cache.lock);
    pthread_mutex_destroy(&s_cache.lock);
}


static cached_vhost_t *cache_lookup(const char *hostname)
{
    unsigned int idx = djb2_hash(hostname) % CACHE_BUCKETS;
    time_t now = time(NULL);

    pthread_mutex_lock(&s_cache.lock);
    cached_vhost_t *p = s_cache.buckets[idx];
    cached_vhost_t *prev = NULL;
    while (p)
    {
        if (strcasecmp(p->hostname, hostname) == 0)
        {
            if (p->expire_time > now)
            {
                pthread_mutex_unlock(&s_cache.lock);
                return p;
            }
            // Expired - remove
            if (prev)
                prev->next = p->next;
            else
                s_cache.buckets[idx] = p->next;
            --s_cache.count;
            free(p);
            pthread_mutex_unlock(&s_cache.lock);
            return NULL;
        }
        prev = p;
        p = p->next;
    }
    pthread_mutex_unlock(&s_cache.lock);
    return NULL;
}


static void cache_insert(const cached_vhost_t *entry, int ttl)
{
    unsigned int idx = djb2_hash(entry->hostname) % CACHE_BUCKETS;

    cached_vhost_t *pNew = (cached_vhost_t *)malloc(sizeof(cached_vhost_t));
    if (!pNew)
        return;
    memcpy(pNew, entry, sizeof(cached_vhost_t));
    pNew->expire_time = time(NULL) + ttl;

    pthread_mutex_lock(&s_cache.lock);

    // Check if already exists, replace
    cached_vhost_t *p = s_cache.buckets[idx];
    cached_vhost_t *prev = NULL;
    while (p)
    {
        if (strcasecmp(p->hostname, entry->hostname) == 0)
        {
            pNew->next = p->next;
            if (prev)
                prev->next = pNew;
            else
                s_cache.buckets[idx] = pNew;
            free(p);
            pthread_mutex_unlock(&s_cache.lock);
            return;
        }
        prev = p;
        p = p->next;
    }

    pNew->next = s_cache.buckets[idx];
    s_cache.buckets[idx] = pNew;
    ++s_cache.count;
    pthread_mutex_unlock(&s_cache.lock);
}


/* --------------------------------------------------------------------
 * Redis connection management
 * -------------------------------------------------------------------- */

static void redis_disconnect()
{
    pthread_mutex_lock(&s_redis_lock);
    if (s_redis_ctx)
    {
        redisFree(s_redis_ctx);
        s_redis_ctx = NULL;
    }
    pthread_mutex_unlock(&s_redis_lock);
}


static int redis_connect(const redis_vhost_conf_t *conf)
{
    pthread_mutex_lock(&s_redis_lock);

    if (s_redis_ctx)
    {
        pthread_mutex_unlock(&s_redis_lock);
        return 0;
    }

    time_t now = time(NULL);
    if (now - s_last_connect_attempt < REDIS_RECONNECT_INTERVAL)
    {
        pthread_mutex_unlock(&s_redis_lock);
        return -1;
    }
    s_last_connect_attempt = now;

    struct timeval tv;
    tv.tv_sec = REDIS_CONNECT_TIMEOUT_SEC;
    tv.tv_usec = 0;

    const char *host = conf->host ? conf->host : DEF_REDIS_HOST;
    int port = conf->port > 0 ? conf->port : DEF_REDIS_PORT;

    s_redis_ctx = redisConnectWithTimeout(host, port, tv);
    if (!s_redis_ctx || s_redis_ctx->err)
    {
        if (s_redis_ctx)
        {
            g_api->log(NULL, LSI_LOG_ERROR,
                       "[mod_redisvhost] Redis connect error: %s\n",
                       s_redis_ctx->errstr);
            redisFree(s_redis_ctx);
            s_redis_ctx = NULL;
        }
        else
        {
            g_api->log(NULL, LSI_LOG_ERROR,
                       "[mod_redisvhost] Redis connect failed: cannot allocate context\n");
        }
        pthread_mutex_unlock(&s_redis_lock);
        return -1;
    }

    // Set command timeout
    tv.tv_sec = REDIS_CMD_TIMEOUT_SEC;
    tv.tv_usec = 0;
    redisSetTimeout(s_redis_ctx, tv);

    // Authenticate if password provided
    if (conf->password && conf->password[0])
    {
        redisReply *reply = (redisReply *)redisCommand(s_redis_ctx, "AUTH %s",
                            conf->password);
        if (!reply || reply->type == REDIS_REPLY_ERROR)
        {
            g_api->log(NULL, LSI_LOG_ERROR,
                       "[mod_redisvhost] Redis AUTH failed: %s\n",
                       reply ? reply->str : "no reply");
            if (reply)
                freeReplyObject(reply);
            redisFree(s_redis_ctx);
            s_redis_ctx = NULL;
            pthread_mutex_unlock(&s_redis_lock);
            return -1;
        }
        freeReplyObject(reply);
    }

    g_api->log(NULL, LSI_LOG_INFO,
               "[mod_redisvhost] Connected to Redis at %s:%d\n",
               host, port);

    pthread_mutex_unlock(&s_redis_lock);
    return 0;
}


/* --------------------------------------------------------------------
 * Minimal JSON parser - extract string values from a flat JSON object.
 * We avoid pulling in a full JSON library for this simple use case.
 * -------------------------------------------------------------------- */

static int json_get_string(const char *json, const char *key,
                           char *out, int out_len)
{
    out[0] = '\0';

    char search_key[128];
    int klen = snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    if (klen <= 0)
        return -1;

    const char *pos = strstr(json, search_key);
    if (!pos)
        return -1;

    pos += klen;
    // Skip whitespace and colon
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')
        ++pos;
    if (*pos != ':')
        return -1;
    ++pos;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')
        ++pos;

    if (*pos == '"')
    {
        // String value
        ++pos;
        const char *end = pos;
        while (*end && *end != '"')
        {
            if (*end == '\\' && *(end + 1))
                end += 2;
            else
                ++end;
        }
        int len = end - pos;
        if (len >= out_len)
            len = out_len - 1;
        memcpy(out, pos, len);
        out[len] = '\0';
        return len;
    }
    return -1;
}


static int json_get_int(const char *json, const char *key, int default_val)
{
    char search_key[128];
    int klen = snprintf(search_key, sizeof(search_key), "\"%s\"", key);
    if (klen <= 0)
        return default_val;

    const char *pos = strstr(json, search_key);
    if (!pos)
        return default_val;

    pos += klen;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')
        ++pos;
    if (*pos != ':')
        return default_val;
    ++pos;
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r')
        ++pos;

    // Could be a number or a quoted number
    if (*pos == '"')
        ++pos;

    return atoi(pos);
}


/* --------------------------------------------------------------------
 * Redis lookup
 * -------------------------------------------------------------------- */

static int redis_lookup_vhost(const redis_vhost_conf_t *conf,
                              const char *hostname,
                              cached_vhost_t *result)
{
    if (redis_connect(conf) != 0)
        return -1;

    const char *prefix = conf->key_prefix ? conf->key_prefix : DEF_REDIS_PREFIX;
    char key[MAX_KEY_LEN];
    int klen = snprintf(key, sizeof(key), "%s%s", prefix, hostname);
    if (klen <= 0 || klen >= (int)sizeof(key))
        return -1;

    pthread_mutex_lock(&s_redis_lock);
    if (!s_redis_ctx)
    {
        pthread_mutex_unlock(&s_redis_lock);
        return -1;
    }

    redisReply *reply = (redisReply *)redisCommand(s_redis_ctx, "GET %s", key);
    if (!reply)
    {
        // Connection lost
        g_api->log(NULL, LSI_LOG_WARN,
                   "[mod_redisvhost] Redis connection lost, will reconnect\n");
        redisFree(s_redis_ctx);
        s_redis_ctx = NULL;
        pthread_mutex_unlock(&s_redis_lock);
        return -1;
    }

    if (reply->type == REDIS_REPLY_ERROR)
    {
        g_api->log(NULL, LSI_LOG_ERROR,
                   "[mod_redisvhost] Redis GET error: %s\n", reply->str);
        freeReplyObject(reply);
        pthread_mutex_unlock(&s_redis_lock);
        return -1;
    }

    if (reply->type == REDIS_REPLY_NIL || reply->type != REDIS_REPLY_STRING)
    {
        freeReplyObject(reply);
        pthread_mutex_unlock(&s_redis_lock);
        return -1;  // Key not found
    }

    // Copy reply data before releasing lock
    char json_buf[MAX_JSON_VALUE_LEN];
    int json_len = reply->len;
    if (json_len >= (int)sizeof(json_buf))
        json_len = sizeof(json_buf) - 1;
    memcpy(json_buf, reply->str, json_len);
    json_buf[json_len] = '\0';

    freeReplyObject(reply);
    pthread_mutex_unlock(&s_redis_lock);

    // Parse JSON
    memset(result, 0, sizeof(*result));
    strncpy(result->hostname, hostname, MAX_HOST_LEN - 1);
    result->hostname[MAX_HOST_LEN - 1] = '\0';

    json_get_string(json_buf, "docRoot", result->doc_root, sizeof(result->doc_root));
    json_get_string(json_buf, "phpVersion", result->php_version, sizeof(result->php_version));
    json_get_string(json_buf, "accessLog", result->access_log, sizeof(result->access_log));
    json_get_string(json_buf, "errorLog", result->error_log, sizeof(result->error_log));
    json_get_string(json_buf, "aliases", result->aliases, sizeof(result->aliases));
    json_get_string(json_buf, "sslCert", result->ssl_cert, sizeof(result->ssl_cert));
    json_get_string(json_buf, "sslKey", result->ssl_key, sizeof(result->ssl_key));

    result->uid = json_get_int(json_buf, "uid", 0);
    result->gid = json_get_int(json_buf, "gid", 0);

    // Validate docRoot is present
    if (!result->doc_root[0])
    {
        g_api->log(NULL, LSI_LOG_ERROR,
                   "[mod_redisvhost] Redis vhost [%s]: missing docRoot\n",
                   hostname);
        return -1;
    }

    g_api->log(NULL, LSI_LOG_DEBUG,
               "[mod_redisvhost] Redis lookup [%s]: docRoot=%s, "
               "phpVersion=%s, uid=%d, gid=%d\n",
               hostname, result->doc_root,
               result->php_version[0] ? result->php_version : "(default)",
               result->uid, result->gid);

    return 0;
}


/* --------------------------------------------------------------------
 * Hook handler: on receiving request header, resolve vhost from Redis
 * -------------------------------------------------------------------- */

static int on_rcvd_req_header(lsi_param_t *param)
{
    redis_vhost_conf_t *conf = (redis_vhost_conf_t *)
                               g_api->get_config(param->session, &MNAME);

    if (!conf || !conf->enabled)
        return LSI_OK;

    // Get the Host header
    int host_len = 0;
    const char *pHost = g_api->get_req_header_by_id(param->session,
                        LSI_HDR_HOST, &host_len);
    if (!pHost || host_len <= 0 || host_len >= MAX_HOST_LEN)
        return LSI_OK;

    // Copy and lowercase the hostname, strip port if present
    char hostname[MAX_HOST_LEN];
    int hlen = host_len;
    if (hlen >= MAX_HOST_LEN)
        hlen = MAX_HOST_LEN - 1;
    for (int i = 0; i < hlen; ++i)
    {
        char c = pHost[i];
        if (c == ':')
        {
            hlen = i;
            break;
        }
        hostname[i] = (c >= 'A' && c <= 'Z') ? (c + 32) : c;
    }
    hostname[hlen] = '\0';

    // Strip www. prefix for lookup
    const char *lookup_host = hostname;
    if (hlen > 4 && strncmp(hostname, "www.", 4) == 0)
        lookup_host = hostname + 4;

    // Check cache first
    cached_vhost_t *cached = cache_lookup(lookup_host);
    if (!cached && strncmp(hostname, "www.", 4) == 0)
        cached = cache_lookup(hostname);

    if (cached)
    {
        // Set document root from cache
        if (cached->doc_root[0])
        {
            g_api->set_req_env(param->session, "REDIS_VHOST_DOCROOT",
                               19, cached->doc_root, strlen(cached->doc_root));
        }
        g_api->log(param->session, LSI_LOG_DEBUG,
                   "[mod_redisvhost] Cache hit for [%s] -> docRoot=%s\n",
                   lookup_host, cached->doc_root);
        return LSI_OK;
    }

    // Cache miss - query Redis
    cached_vhost_t result;
    if (redis_lookup_vhost(conf, lookup_host, &result) != 0)
    {
        // Try with www. prefix if we stripped it
        if (lookup_host != hostname)
        {
            if (redis_lookup_vhost(conf, hostname, &result) != 0)
            {
                g_api->log(param->session, LSI_LOG_DEBUG,
                           "[mod_redisvhost] No Redis entry for [%s]\n",
                           hostname);
                return LSI_OK;  // Fall through to file-based config
            }
        }
        else
        {
            g_api->log(param->session, LSI_LOG_DEBUG,
                       "[mod_redisvhost] No Redis entry for [%s]\n",
                       lookup_host);
            return LSI_OK;  // Fall through to file-based config
        }
    }

    int ttl = conf->ttl > 0 ? conf->ttl : DEF_REDIS_TTL;
    cache_insert(&result, ttl);

    // Set environment variables for the request
    if (result.doc_root[0])
    {
        g_api->set_req_env(param->session, "REDIS_VHOST_DOCROOT",
                           19, result.doc_root, strlen(result.doc_root));
    }
    if (result.php_version[0])
    {
        g_api->set_req_env(param->session, "REDIS_VHOST_PHPVER",
                           18, result.php_version, strlen(result.php_version));
    }
    if (result.access_log[0])
    {
        g_api->set_req_env(param->session, "REDIS_VHOST_ACCESSLOG",
                           21, result.access_log, strlen(result.access_log));
    }
    if (result.error_log[0])
    {
        g_api->set_req_env(param->session, "REDIS_VHOST_ERRORLOG",
                           20, result.error_log, strlen(result.error_log));
    }
    if (result.ssl_cert[0])
    {
        g_api->set_req_env(param->session, "REDIS_VHOST_SSLCERT",
                           19, result.ssl_cert, strlen(result.ssl_cert));
    }
    if (result.ssl_key[0])
    {
        g_api->set_req_env(param->session, "REDIS_VHOST_SSLKEY",
                           18, result.ssl_key, strlen(result.ssl_key));
    }
    if (result.uid > 0)
    {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d", result.uid);
        g_api->set_req_env(param->session, "REDIS_VHOST_UID", 15, buf, len);
    }
    if (result.gid > 0)
    {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d", result.gid);
        g_api->set_req_env(param->session, "REDIS_VHOST_GID", 15, buf, len);
    }

    g_api->log(param->session, LSI_LOG_INFO,
               "[mod_redisvhost] Resolved [%s] from Redis: docRoot=%s\n",
               lookup_host, result.doc_root);

    return LSI_OK;
}


/* --------------------------------------------------------------------
 * URI map hook: apply the document root from Redis lookup
 * -------------------------------------------------------------------- */

static int on_uri_map(lsi_param_t *param)
{
    redis_vhost_conf_t *conf = (redis_vhost_conf_t *)
                               g_api->get_config(param->session, &MNAME);

    if (!conf || !conf->enabled)
        return LSI_OK;

    // Check if we have a Redis vhost docroot set
    int val_len = 0;
    const char *docroot = g_api->get_req_env(param->session,
                          "REDIS_VHOST_DOCROOT", 19, &val_len);

    if (!docroot || val_len <= 0)
        return LSI_OK;

    // Verify the document root directory exists
    struct stat st;
    if (stat(docroot, &st) == -1 || !S_ISDIR(st.st_mode))
    {
        g_api->log(param->session, LSI_LOG_WARN,
                   "[mod_redisvhost] docRoot [%.*s] does not exist or is "
                   "not a directory\n", val_len, docroot);
        return LSI_OK;
    }

    // Build the full file path: docroot + URI
    char uri_buf[MAX_PATH_LEN_RV];
    int uri_len = g_api->get_req_org_uri(param->session, uri_buf,
                                          sizeof(uri_buf) - 1);
    if (uri_len <= 0)
        return LSI_OK;
    uri_buf[uri_len] = '\0';

    char full_path[MAX_PATH_LEN_RV * 2];
    int flen = snprintf(full_path, sizeof(full_path), "%.*s%s",
                        val_len, docroot, uri_buf);
    if (flen > 0 && flen < (int)sizeof(full_path))
    {
        // Set the mapped file path via environment; the actual rewrite
        // to serve from this docroot should be handled by rewrite rules
        // or context configuration that reads REDIS_VHOST_DOCROOT.
        g_api->set_req_env(param->session, "REDIS_VHOST_FILEPATH",
                           20, full_path, flen);
    }

    g_api->log(param->session, LSI_LOG_DEBUG,
               "[mod_redisvhost] URI map: docRoot=%.*s, uri=%s\n",
               val_len, docroot, uri_buf);

    return LSI_OK;
}


/* --------------------------------------------------------------------
 * Config parsing
 * -------------------------------------------------------------------- */

enum
{
    PARAM_ENABLE      = 0,
    PARAM_SERVER      = 1,
    PARAM_PASSWORD    = 2,
    PARAM_TTL         = 3,
    PARAM_KEY_PREFIX  = 4,
};


const int paramArrayCount = 5;
lsi_config_key_t paramArray[paramArrayCount + 1] =
{
    {"redisvhostenable",    PARAM_ENABLE,     LSI_CFG_SERVER | LSI_CFG_VHOST},
    {"redisvhostserver",    PARAM_SERVER,     LSI_CFG_SERVER | LSI_CFG_VHOST},
    {"redisvhostpassword",  PARAM_PASSWORD,   LSI_CFG_SERVER | LSI_CFG_VHOST},
    {"redisvhostttl",       PARAM_TTL,        LSI_CFG_SERVER | LSI_CFG_VHOST},
    {"redisvhostkeyprefix", PARAM_KEY_PREFIX, LSI_CFG_SERVER | LSI_CFG_VHOST},
    {NULL, 0, 0}
};


static void parse_host_port(const char *val, int val_len,
                            char **out_host, int *out_port)
{
    // Format: host:port or just host
    char buf[256];
    if (val_len >= (int)sizeof(buf))
        val_len = sizeof(buf) - 1;
    memcpy(buf, val, val_len);
    buf[val_len] = '\0';

    char *colon = strrchr(buf, ':');
    if (colon)
    {
        *colon = '\0';
        *out_host = strdup(buf);
        *out_port = atoi(colon + 1);
        if (*out_port <= 0 || *out_port > 65535)
            *out_port = DEF_REDIS_PORT;
    }
    else
    {
        *out_host = strdup(buf);
        *out_port = DEF_REDIS_PORT;
    }
}


static void *redisvhost_parseConfig(module_param_info_t *param,
                                    int param_count,
                                    void *_initial_config,
                                    int level,
                                    const char *name)
{
    redis_vhost_conf_t *initConf = (redis_vhost_conf_t *)_initial_config;
    redis_vhost_conf_t *myConf = (redis_vhost_conf_t *)malloc(
                                     sizeof(redis_vhost_conf_t));
    if (!myConf)
        return NULL;

    // Initialize with defaults or inherited values
    memset(myConf, 0, sizeof(redis_vhost_conf_t));
    if (initConf)
    {
        myConf->enabled = initConf->enabled;
        if (initConf->host)
            myConf->host = strdup(initConf->host);
        myConf->port = initConf->port;
        if (initConf->password)
            myConf->password = strdup(initConf->password);
        myConf->ttl = initConf->ttl;
        if (initConf->key_prefix)
            myConf->key_prefix = strdup(initConf->key_prefix);
    }
    else
    {
        myConf->host = strdup(DEF_REDIS_HOST);
        myConf->port = DEF_REDIS_PORT;
        myConf->ttl = DEF_REDIS_TTL;
        myConf->key_prefix = strdup(DEF_REDIS_PREFIX);
    }

    if (!param || param_count <= 0)
        return (void *)myConf;

    for (int i = 0; i < param_count; ++i)
    {
        switch (param[i].key_index)
        {
        case PARAM_ENABLE:
            myConf->enabled = (param[i].val_len > 0 && param[i].val[0] == '1')
                              ? 1 : 0;
            break;

        case PARAM_SERVER:
            if (myConf->host)
                free(myConf->host);
            parse_host_port(param[i].val, param[i].val_len,
                            &myConf->host, &myConf->port);
            break;

        case PARAM_PASSWORD:
            if (myConf->password)
                free(myConf->password);
            myConf->password = strndup(param[i].val, param[i].val_len);
            break;

        case PARAM_TTL:
            myConf->ttl = atoi(param[i].val);
            if (myConf->ttl < 0)
                myConf->ttl = DEF_REDIS_TTL;
            break;

        case PARAM_KEY_PREFIX:
            if (myConf->key_prefix)
                free(myConf->key_prefix);
            myConf->key_prefix = strndup(param[i].val, param[i].val_len);
            break;
        }
    }

    return (void *)myConf;
}


static void redisvhost_freeConfig(void *_config)
{
    redis_vhost_conf_t *pConfig = (redis_vhost_conf_t *)_config;
    if (pConfig)
    {
        if (pConfig->host)
            free(pConfig->host);
        if (pConfig->password)
            free(pConfig->password);
        if (pConfig->key_prefix)
            free(pConfig->key_prefix);
        free(pConfig);
    }
}


/* --------------------------------------------------------------------
 * Module init / cleanup
 * -------------------------------------------------------------------- */

static int _init(lsi_module_t *pModule)
{
    if (!s_initialized)
    {
        cache_init();
        s_initialized = 1;
        g_api->log(NULL, LSI_LOG_INFO,
                   "[mod_redisvhost] Module initialized (v%s)\n",
                   MODULE_VERSION_INFO);
    }
    return 0;
}


/* --------------------------------------------------------------------
 * Module definition
 * -------------------------------------------------------------------- */

static lsi_serverhook_t server_hooks[] =
{
    {
        LSI_HKPT_RCVD_REQ_HEADER,
        on_rcvd_req_header,
        LSI_HOOK_EARLY,
        LSI_FLAG_ENABLED
    },
    {
        LSI_HKPT_URI_MAP,
        on_uri_map,
        LSI_HOOK_NORMAL,
        LSI_FLAG_ENABLED
    },
    LSI_HOOK_END
};


lsi_confparser_t redisvhost_dealConfig =
{
    redisvhost_parseConfig,
    redisvhost_freeConfig,
    paramArray
};


LSMODULE_EXPORT lsi_module_t MNAME =
{
    LSI_MODULE_SIGNATURE,
    _init,
    NULL,
    &redisvhost_dealConfig,
    MODULE_VERSION_INFO,
    server_hooks,
    {0}
};
