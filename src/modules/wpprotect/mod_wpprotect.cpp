/*****************************************************************************
*    Open LiteSpeed is an open source HTTP server.                           *
*    Copyright (C) 2013 - 2026  LiteSpeed Technologies, Inc.                 *
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
 * mod_wpprotect - WordPress Brute Force Protection Module
 *
 * Monitors POST requests to /wp-login.php and /xmlrpc.php, tracks failed
 * login attempts per IP using shared memory, and blocks or challenges
 * with reCAPTCHA after a configurable threshold is exceeded.
 */

#include <ls.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#define MNAME                       mod_wpprotect
#define ModuleNameStr               "mod_wpprotect"
#define VERSIONNUMBER               "1.0"
#define MODULE_VERSION_INFO         ModuleNameStr " " VERSIONNUMBER

/////////////////////////////////////////////////////////////////////////////
extern lsi_module_t MNAME;

/**
 * Shared memory IP tracking structures.
 * These reside in a mmap'd anonymous region shared across worker processes.
 */

#define WP_PROTECT_MAX_IPS          4096
#define WP_PROTECT_IP_LEN           46      /* Max IPv6 string length */
#define WP_PROTECT_COOKIE_NAME      "wp_captcha_verified"
#define WP_PROTECT_COOKIE_LEN       19

enum WpProtectAction
{
    WP_ACTION_DENY    = 0,
    WP_ACTION_CAPTCHA = 1
};

struct IpRecord
{
    char        ip[WP_PROTECT_IP_LEN];
    int         attempts;
    time_t      firstAttempt;
    time_t      lockoutUntil;
};

struct ShmData
{
    pthread_mutex_t     mutex;
    int                 count;
    IpRecord            records[WP_PROTECT_MAX_IPS];
};

static ShmData *s_pShmData = NULL;

/**
 * Per-context (vhost/context level) configuration.
 */
struct WpProtectConfig
{
    int         enabled;            /* wpProtect: 0=off, 1=on */
    int         maxRetry;           /* wpProtectMaxRetry: default 5 */
    int         lockoutSecs;        /* wpProtectLockout: default 300 */
    int         action;             /* wpProtectAction: 0=deny, 1=captcha */
    char        siteKey[256];       /* recaptchaSiteKey */
    char        secretKey[256];     /* recaptchaSecretKey */
};

static const WpProtectConfig s_defaultConfig = {
    0,      /* disabled */
    5,      /* maxRetry */
    300,    /* lockoutSecs */
    1,      /* action: captcha */
    "",     /* siteKey */
    ""      /* secretKey */
};


/*****************************************************************************
 * Configuration parameter parsing
 *****************************************************************************/

enum
{
    PARAM_WP_PROTECT = 0,
    PARAM_MAX_RETRY,
    PARAM_LOCKOUT,
    PARAM_ACTION,
    PARAM_SITE_KEY,
    PARAM_SECRET_KEY
};

static lsi_config_key_t paramArray[] =
{
    {"wpProtect",           0, 0},
    {"wpProtectMaxRetry",   1, 0},
    {"wpProtectLockout",    2, 0},
    {"wpProtectAction",     3, 0},
    {"recaptchaSiteKey",    4, 0},
    {"recaptchaSecretKey",  5, 0},
    {NULL, 0, 0}
};


static void *ParseConfig(module_param_info_t *param, int param_count,
                          void *_initial_config, int level, const char *name)
{
    WpProtectConfig *pInit = (WpProtectConfig *)_initial_config;
    WpProtectConfig *pConfig = new WpProtectConfig;
    if (!pConfig)
        return NULL;

    /* Inherit from parent level or use defaults */
    if (pInit)
        *pConfig = *pInit;
    else
        *pConfig = s_defaultConfig;

    if (!param || param_count == 0)
        return (void *)pConfig;

    for (int i = 0; i < param_count; ++i)
    {
        if (param[i].val_len == 0)
            continue;

        switch (param[i].key_index)
        {
        case PARAM_WP_PROTECT:
            pConfig->enabled = atoi(param[i].val);
            break;
        case PARAM_MAX_RETRY:
            pConfig->maxRetry = atoi(param[i].val);
            if (pConfig->maxRetry < 1)
                pConfig->maxRetry = 1;
            break;
        case PARAM_LOCKOUT:
            pConfig->lockoutSecs = atoi(param[i].val);
            if (pConfig->lockoutSecs < 1)
                pConfig->lockoutSecs = 1;
            break;
        case PARAM_ACTION:
            pConfig->action = atoi(param[i].val);
            if (pConfig->action != WP_ACTION_DENY
                && pConfig->action != WP_ACTION_CAPTCHA)
                pConfig->action = WP_ACTION_CAPTCHA;
            break;
        case PARAM_SITE_KEY:
        {
            int len = param[i].val_len;
            if (len >= (int)sizeof(pConfig->siteKey))
                len = (int)sizeof(pConfig->siteKey) - 1;
            memcpy(pConfig->siteKey, param[i].val, len);
            pConfig->siteKey[len] = '\0';
            break;
        }
        case PARAM_SECRET_KEY:
        {
            int len = param[i].val_len;
            if (len >= (int)sizeof(pConfig->secretKey))
                len = (int)sizeof(pConfig->secretKey) - 1;
            memcpy(pConfig->secretKey, param[i].val, len);
            pConfig->secretKey[len] = '\0';
            break;
        }
        default:
            break;
        }
    }

    g_api->log(NULL, LSI_LOG_INFO,
               "[%s] ParseConfig level=%d enabled=%d maxRetry=%d "
               "lockout=%d action=%d\n",
               ModuleNameStr, level, pConfig->enabled,
               pConfig->maxRetry, pConfig->lockoutSecs, pConfig->action);

    return (void *)pConfig;
}


static void FreeConfig(void *_config)
{
    delete (WpProtectConfig *)_config;
}


/*****************************************************************************
 * Shared memory management
 *****************************************************************************/

static int initShm()
{
    if (s_pShmData)
        return 0;

    s_pShmData = (ShmData *)mmap(NULL, sizeof(ShmData),
                                  PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (s_pShmData == MAP_FAILED)
    {
        s_pShmData = NULL;
        g_api->log(NULL, LSI_LOG_ERROR,
                   "[%s] Failed to allocate shared memory for IP tracking.\n",
                   ModuleNameStr);
        return -1;
    }

    memset(s_pShmData, 0, sizeof(ShmData));

    /* Initialize a process-shared mutex */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&s_pShmData->mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    g_api->log(NULL, LSI_LOG_INFO,
               "[%s] Shared memory initialized, capacity=%d IPs.\n",
               ModuleNameStr, WP_PROTECT_MAX_IPS);

    return 0;
}


/**
 * Find or create an IpRecord for the given IP.
 * Caller must hold s_pShmData->mutex.
 * Expired records are recycled. If the table is full, the oldest record
 * is evicted.
 */
static IpRecord *findOrCreateRecord(const char *ip, time_t now)
{
    IpRecord *pFree = NULL;
    IpRecord *pOldest = NULL;
    time_t    oldestTime = now + 1;

    for (int i = 0; i < s_pShmData->count; ++i)
    {
        IpRecord *r = &s_pShmData->records[i];

        /* Exact match */
        if (strcmp(r->ip, ip) == 0)
            return r;

        /* Track oldest for eviction */
        if (r->firstAttempt < oldestTime)
        {
            oldestTime = r->firstAttempt;
            pOldest = r;
        }

        /* Track expired slots for reuse */
        if (r->lockoutUntil > 0 && r->lockoutUntil <= now)
        {
            if (!pFree)
                pFree = r;
        }
        else if (r->ip[0] == '\0')
        {
            if (!pFree)
                pFree = r;
        }
    }

    /* Use a free/expired slot if available */
    if (!pFree)
    {
        if (s_pShmData->count < WP_PROTECT_MAX_IPS)
        {
            pFree = &s_pShmData->records[s_pShmData->count];
            s_pShmData->count++;
        }
        else
        {
            /* Evict oldest record */
            pFree = pOldest;
        }
    }

    /* Initialize the new record */
    if (pFree)
    {
        memset(pFree, 0, sizeof(IpRecord));
        int len = strlen(ip);
        if (len >= WP_PROTECT_IP_LEN)
            len = WP_PROTECT_IP_LEN - 1;
        memcpy(pFree->ip, ip, len);
        pFree->ip[len] = '\0';
        pFree->attempts = 0;
        pFree->firstAttempt = now;
        pFree->lockoutUntil = 0;
    }

    return pFree;
}


/*****************************************************************************
 * Helper: check for WordPress admin cookie (wordpress_logged_in_*)
 *****************************************************************************/

static int hasWpAdminCookie(lsi_session_t *session)
{
    int cookieLen = 0;
    const char *cookies = g_api->get_req_cookies(session, &cookieLen);
    if (!cookies || cookieLen == 0)
        return 0;

    /*
     * Look for "wordpress_logged_in_" anywhere in the cookie header.
     * This indicates the user has a valid WordPress login session and
     * should be whitelisted from brute force checks.
     */
    const char *needle = "wordpress_logged_in_";
    const int needleLen = 20;

    if (cookieLen < needleLen)
        return 0;
    for (int i = 0; i <= cookieLen - needleLen; ++i)
    {
        if (memcmp(cookies + i, needle, needleLen) == 0)
            return 1;
    }
    return 0;
}


/*****************************************************************************
 * Helper: check for reCAPTCHA verification cookie
 *****************************************************************************/

static int hasCaptchaVerifiedCookie(lsi_session_t *session)
{
    int cookieLen = 0;
    const char *cookies = g_api->get_req_cookies(session, &cookieLen);
    if (!cookies || cookieLen == 0)
        return 0;

    const char *needle = WP_PROTECT_COOKIE_NAME "=";
    const int needleLen = WP_PROTECT_COOKIE_LEN + 1;

    if (cookieLen < needleLen)
        return 0;
    for (int i = 0; i <= cookieLen - needleLen; ++i)
    {
        if (memcmp(cookies + i, needle, needleLen) == 0)
            return 1;
    }
    return 0;
}


/*****************************************************************************
 * Helper: check if URI is a WordPress login/xmlrpc endpoint
 *****************************************************************************/

static int isWpLoginUri(const char *uri, int uriLen)
{
    /* Check for /wp-login.php */
    const char *wpLogin = "/wp-login.php";
    const int wpLoginLen = 13;

    /* Check for /xmlrpc.php */
    const char *xmlrpc = "/xmlrpc.php";
    const int xmlrpcLen = 11;

    if (uriLen >= wpLoginLen)
    {
        /* Match /wp-login.php at end or followed by ? */
        for (int i = 0; i <= uriLen - wpLoginLen; ++i)
        {
            if (memcmp(uri + i, wpLogin, wpLoginLen) == 0)
            {
                int end = i + wpLoginLen;
                if (end == uriLen || uri[end] == '?')
                    return 1;
            }
        }
    }

    if (uriLen >= xmlrpcLen)
    {
        for (int i = 0; i <= uriLen - xmlrpcLen; ++i)
        {
            if (memcmp(uri + i, xmlrpc, xmlrpcLen) == 0)
            {
                int end = i + xmlrpcLen;
                if (end == uriLen || uri[end] == '?')
                    return 1;
            }
        }
    }

    return 0;
}


/*****************************************************************************
 * Build the 403 reCAPTCHA challenge HTML page
 *****************************************************************************/

static int sendCaptchaPage(lsi_session_t *session, const WpProtectConfig *pConfig)
{
    const char *htmlPart1 =
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "  <title>Security Check Required</title>\n"
        "  <script src=\"https://www.google.com/recaptcha/api.js\" async defer>"
        "</script>\n"
        "  <style>\n"
        "    body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI',\n"
        "           Roboto, sans-serif; background: #f1f1f1; display: flex;\n"
        "           justify-content: center; align-items: center;\n"
        "           min-height: 100vh; margin: 0; }\n"
        "    .container { background: #fff; padding: 40px; border-radius: 8px;\n"
        "                 box-shadow: 0 2px 10px rgba(0,0,0,0.1);\n"
        "                 text-align: center; max-width: 420px; }\n"
        "    h1 { color: #23282d; font-size: 22px; margin-bottom: 10px; }\n"
        "    p  { color: #666; font-size: 14px; line-height: 1.6; }\n"
        "    .g-recaptcha { display: inline-block; margin: 20px 0; }\n"
        "    button { background: #0073aa; color: #fff; border: none;\n"
        "             padding: 12px 32px; font-size: 15px; border-radius: 4px;\n"
        "             cursor: pointer; }\n"
        "    button:hover { background: #005a87; }\n"
        "    .notice { color: #dc3232; font-size: 13px; margin-top: 15px; }\n"
        "  </style>\n"
        "</head>\n"
        "<body>\n"
        "  <div class=\"container\">\n"
        "    <h1>Security Check</h1>\n"
        "    <p>Too many failed login attempts have been detected from your IP "
        "address. Please complete the challenge below to continue.</p>\n"
        "    <form method=\"POST\" action=\"\">\n"
        "      <div class=\"g-recaptcha\" data-sitekey=\"";

    const char *htmlPart2 =
        "\"></div>\n"
        "      <br>\n"
        "      <button type=\"submit\">Verify &amp; Continue</button>\n"
        "    </form>\n"
        "    <p class=\"notice\">This protection is provided by "
        "OpenLiteSpeed WP Protect.</p>\n"
        "  </div>\n"
        "  <script>\n"
        "    document.querySelector('form').addEventListener('submit', "
        "function(e) {\n"
        "      var resp = document.getElementById('g-recaptcha-response');\n"
        "      if (!resp || !resp.value) {\n"
        "        e.preventDefault();\n"
        "        alert('Please complete the reCAPTCHA challenge.');\n"
        "      } else {\n"
        "        /* Set verification cookie and allow form submission */\n"
        "        document.cookie = '" WP_PROTECT_COOKIE_NAME "=1; path=/; "
        "max-age=3600; SameSite=Strict';\n"
        "      }\n"
        "    });\n"
        "  </script>\n"
        "</body>\n"
        "</html>\n";

    g_api->set_status_code(session, 403);
    g_api->set_resp_header(session, LSI_RSPHDR_CONTENT_TYPE, NULL, 0,
                           "text/html", 9, LSI_HEADEROP_SET);

    g_api->append_resp_body(session, htmlPart1, strlen(htmlPart1));

    /* HTML-escape the siteKey to prevent XSS via config injection */
    const char *src = pConfig->siteKey;
    char escaped[1024];
    int eLen = 0;
    for (int j = 0; src[j] && eLen < (int)sizeof(escaped) - 8; ++j)
    {
        switch (src[j])
        {
        case '"':  memcpy(escaped + eLen, "&quot;", 6); eLen += 6; break;
        case '&':  memcpy(escaped + eLen, "&amp;",  5); eLen += 5; break;
        case '<':  memcpy(escaped + eLen, "&lt;",   4); eLen += 4; break;
        case '>':  memcpy(escaped + eLen, "&gt;",   4); eLen += 4; break;
        case '\'': memcpy(escaped + eLen, "&#39;",  5); eLen += 5; break;
        default:   escaped[eLen++] = src[j]; break;
        }
    }
    escaped[eLen] = '\0';
    g_api->append_resp_body(session, escaped, eLen);

    g_api->append_resp_body(session, htmlPart2, strlen(htmlPart2));
    g_api->end_resp(session);

    return LSI_OK;
}


/*****************************************************************************
 * Build a simple 403 Denied page (when action=deny)
 *****************************************************************************/

static int sendDenyPage(lsi_session_t *session)
{
    const char *html =
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <title>403 Forbidden</title>\n"
        "  <style>\n"
        "    body { font-family: sans-serif; background: #f1f1f1;\n"
        "           display: flex; justify-content: center;\n"
        "           align-items: center; min-height: 100vh; margin: 0; }\n"
        "    .container { background: #fff; padding: 40px;\n"
        "                 border-radius: 8px; text-align: center;\n"
        "                 box-shadow: 0 2px 10px rgba(0,0,0,0.1); }\n"
        "    h1 { color: #dc3232; }\n"
        "  </style>\n"
        "</head>\n"
        "<body>\n"
        "  <div class=\"container\">\n"
        "    <h1>403 Forbidden</h1>\n"
        "    <p>Your IP address has been temporarily blocked due to too many "
        "failed login attempts.</p>\n"
        "    <p>Please try again later.</p>\n"
        "  </div>\n"
        "</body>\n"
        "</html>\n";

    g_api->set_status_code(session, 403);
    g_api->set_resp_header(session, LSI_RSPHDR_CONTENT_TYPE, NULL, 0,
                           "text/html", 9, LSI_HEADEROP_SET);
    g_api->append_resp_body(session, html, strlen(html));
    g_api->end_resp(session);

    return LSI_OK;
}


/*****************************************************************************
 * Periodic cleanup timer: purge expired records from shared memory
 *****************************************************************************/

static void cleanupTimerCb(const void *pParam)
{
    if (!s_pShmData)
        return;

    time_t now = time(NULL);

    pthread_mutex_lock(&s_pShmData->mutex);

    for (int i = 0; i < s_pShmData->count; ++i)
    {
        IpRecord *r = &s_pShmData->records[i];
        if (r->ip[0] == '\0')
            continue;

        /* Remove records whose lockout has expired and have no recent
         * activity (last attempt was more than 2x the typical lockout ago) */
        if (r->lockoutUntil > 0 && r->lockoutUntil <= now)
        {
            memset(r, 0, sizeof(IpRecord));
        }
        else if (r->lockoutUntil == 0
                 && (now - r->firstAttempt) > 600)
        {
            /* Stale record with no lockout, older than 10 minutes */
            memset(r, 0, sizeof(IpRecord));
        }
    }

    pthread_mutex_unlock(&s_pShmData->mutex);
}


/*****************************************************************************
 * URI Map Hook: main entry point for brute force detection
 *
 * This runs at the URI_MAP stage, after the request header is received
 * and the URI has been mapped to a context. We check:
 *   1. Is the module enabled for this context?
 *   2. Is this a POST to /wp-login.php or /xmlrpc.php?
 *   3. Does the client have a valid WP admin cookie? (whitelist)
 *   4. Is the IP currently locked out?
 *   5. If locked out, serve deny or captcha page.
 *   6. Otherwise, record the attempt.
 *****************************************************************************/

static int onUriMap(lsi_param_t *rec)
{
    lsi_session_t *session = (lsi_session_t *)rec->session;

    /* Get module config for this context */
    WpProtectConfig *pConfig =
        (WpProtectConfig *)g_api->get_config(session, &MNAME);
    if (!pConfig || !pConfig->enabled)
        return LSI_OK;

    /* Only intercept POST requests */
    if (g_api->get_req_method(session) != LSI_METHOD_POST)
        return LSI_OK;

    /* Check if the URI matches a WordPress login endpoint */
    int uriLen = 0;
    const char *uri = g_api->get_req_uri(session, &uriLen);
    if (!uri || uriLen <= 0)
        return LSI_OK;

    if (!isWpLoginUri(uri, uriLen))
        return LSI_OK;

    /* Whitelist: allow requests with valid WordPress admin cookies */
    if (hasWpAdminCookie(session))
    {
        g_api->log(session, LSI_LOG_DEBUG,
                   "[%s] Whitelisted: wordpress_logged_in_ cookie present.\n",
                   ModuleNameStr);
        return LSI_OK;
    }

    /* Get client IP */
    char clientIp[WP_PROTECT_IP_LEN] = {0};
    g_api->get_req_var_by_id(session, LSI_VAR_REMOTE_ADDR,
                             clientIp, WP_PROTECT_IP_LEN);

    if (clientIp[0] == '\0')
        return LSI_OK;

    if (!s_pShmData)
        return LSI_OK;

    time_t now = time(NULL);
    int blocked = 0;

    pthread_mutex_lock(&s_pShmData->mutex);

    IpRecord *r = findOrCreateRecord(clientIp, now);
    if (r)
    {
        /* Check if currently locked out */
        if (r->lockoutUntil > 0 && r->lockoutUntil > now)
        {
            blocked = 1;
            g_api->log(session, LSI_LOG_INFO,
                       "[%s] BLOCKED: IP %s locked out until %ld "
                       "(attempts=%d, uri=%.*s)\n",
                       ModuleNameStr, clientIp,
                       (long)r->lockoutUntil, r->attempts,
                       uriLen, uri);
        }
        else
        {
            /* Reset if the lockout has expired */
            if (r->lockoutUntil > 0 && r->lockoutUntil <= now)
            {
                r->attempts = 0;
                r->lockoutUntil = 0;
                r->firstAttempt = now;
            }

            /* Reset the window if the first attempt is too old */
            if ((now - r->firstAttempt) > pConfig->lockoutSecs)
            {
                r->attempts = 0;
                r->firstAttempt = now;
            }

            r->attempts++;

            g_api->log(session, LSI_LOG_DEBUG,
                       "[%s] IP %s attempt %d/%d on %.*s\n",
                       ModuleNameStr, clientIp, r->attempts,
                       pConfig->maxRetry, uriLen, uri);

            if (r->attempts > pConfig->maxRetry)
            {
                r->lockoutUntil = now + pConfig->lockoutSecs;
                blocked = 1;

                g_api->log(session, LSI_LOG_WARN,
                           "[%s] LOCKOUT: IP %s exceeded %d attempts, "
                           "locked for %d seconds (uri=%.*s)\n",
                           ModuleNameStr, clientIp,
                           pConfig->maxRetry, pConfig->lockoutSecs,
                           uriLen, uri);
            }
        }
    }

    pthread_mutex_unlock(&s_pShmData->mutex);

    if (!blocked)
        return LSI_OK;

    /* IP is blocked -- check action */

    /* If captcha mode and the client has the verification cookie, allow */
    if (pConfig->action == WP_ACTION_CAPTCHA)
    {
        if (hasCaptchaVerifiedCookie(session))
        {
            g_api->log(session, LSI_LOG_INFO,
                       "[%s] IP %s has captcha verification cookie, allowing.\n",
                       ModuleNameStr, clientIp);

            /* Reset the attempt counter on successful captcha */
            pthread_mutex_lock(&s_pShmData->mutex);
            IpRecord *r2 = findOrCreateRecord(clientIp, now);
            if (r2)
            {
                r2->attempts = 0;
                r2->lockoutUntil = 0;
                r2->firstAttempt = now;
            }
            pthread_mutex_unlock(&s_pShmData->mutex);

            return LSI_OK;
        }
    }

    /* Register this module as the request handler to serve the block page */
    g_api->register_req_handler(session, &MNAME, 0);

    return LSI_OK;
}


/*****************************************************************************
 * Request handler: serves the block/captcha page
 *****************************************************************************/

static int beginProcess(const lsi_session_t *session)
{
    WpProtectConfig *pConfig =
        (WpProtectConfig *)g_api->get_config(session, &MNAME);

    if (pConfig && pConfig->action == WP_ACTION_CAPTCHA
        && pConfig->siteKey[0] != '\0')
    {
        sendCaptchaPage((lsi_session_t *)session, pConfig);
    }
    else
    {
        sendDenyPage((lsi_session_t *)session);
    }

    return LSI_OK;
}


static int onCleanUp(const lsi_session_t *session)
{
    return LSI_OK;
}


/*****************************************************************************
 * Response header hook: track failed logins (HTTP 401/403 responses)
 *
 * This monitors responses to WordPress login URLs. If the backend returns
 * a non-redirect response (indicating failed login), the attempt count is
 * already tracked in the URI_MAP hook. The URI_MAP approach is simpler and
 * counts every POST as a potential brute force attempt. A successful login
 * will result in a redirect (302) and set the wordpress_logged_in cookie,
 * at which point the user is whitelisted on subsequent requests.
 *****************************************************************************/


/*****************************************************************************
 * Module hooks and initialization
 *****************************************************************************/

static lsi_serverhook_t serverHooks[] =
{
    { LSI_HKPT_URI_MAP, onUriMap, LSI_HOOK_EARLY, LSI_FLAG_ENABLED },
    LSI_HOOK_END
};


static lsi_reqhdlr_t reqHandler = {
    beginProcess,
    NULL,           /* on_read_req_body */
    NULL,           /* on_write_resp */
    onCleanUp,
    NULL,           /* ts_hdlr_ctx */
    NULL,           /* ts_enqueue_req */
    NULL            /* ts_cancel_req */
};


static int init(lsi_module_t *pModule)
{
    /* Initialize shared memory for cross-process IP tracking */
    if (initShm() != 0)
    {
        g_api->log(NULL, LSI_LOG_ERROR,
                   "[%s] Module initialization failed: "
                   "could not create shared memory.\n",
                   ModuleNameStr);
        return -1;
    }

    /* Set up periodic cleanup timer: every 60 seconds */
    g_api->set_timer(60000, 1, cleanupTimerCb, NULL);

    g_api->log(NULL, LSI_LOG_INFO,
               "[%s] Module v%s initialized successfully.\n",
               ModuleNameStr, VERSIONNUMBER);

    return 0;
}


static lsi_confparser_t configParser = { ParseConfig, FreeConfig, paramArray };

LSMODULE_EXPORT lsi_module_t MNAME =
{
    LSI_MODULE_SIGNATURE,
    init,
    &reqHandler,
    &configParser,
    MODULE_VERSION_INFO,
    serverHooks,
    {0}
};
