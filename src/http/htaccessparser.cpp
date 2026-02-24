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
#include "htaccessparser.h"

#include <http/httpcontext.h>
#include <http/htauth.h>
#include <http/httpmime.h>
#include <http/phpconfig.h>
#include <http/rewriteengine.h>
#include <http/rewriterule.h>
#include <http/rewriterulelist.h>
#include <http/userdir.h>
#include <log4cxx/logger.h>
#include <log4cxx/tmplogid.h>
#include <util/accesscontrol.h>
#include <util/autostr.h>
#include <util/pcregex.h>
#include <util/stringtool.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


static const char *skipWhitespace(const char *p)
{
    while (*p && isspace(*p))
        ++p;
    return p;
}

static int trimmedLen(const char *p, int len)
{
    while (len > 0 && isspace(p[len - 1]))
        --len;
    return len;
}

// Escape regex metacharacters in a literal URL path for use in RewriteRule.
// Returns length written (not including null terminator).
static int escapeRegex(const char *pSrc, int srcLen, char *pDst, int dstSize)
{
    const char *pEnd = pSrc + srcLen;
    char *pOut = pDst;
    char *pOutEnd = pDst + dstSize - 1;
    while (pSrc < pEnd && pOut < pOutEnd)
    {
        switch (*pSrc)
        {
        case '.': case '+': case '*': case '?': case '(': case ')':
        case '[': case ']': case '{': case '}': case '\\': case '^':
        case '$': case '|':
            if (pOut + 1 < pOutEnd)
            {
                *pOut++ = '\\';
                *pOut++ = *pSrc;
            }
            break;
        default:
            *pOut++ = *pSrc;
            break;
        }
        ++pSrc;
    }
    *pOut = 0;
    return pOut - pDst;
}

// Extract a possibly-quoted argument. Returns pointer past the argument.
static const char *extractArg(const char *p, const char **pStart, int *pLen)
{
    p = skipWhitespace(p);
    if (*p == '"')
    {
        ++p;
        *pStart = p;
        const char *end = strchr(p, '"');
        if (end)
        {
            *pLen = end - p;
            return end + 1;
        }
        *pLen = strlen(p);
        return p + *pLen;
    }
    *pStart = p;
    const char *start = p;
    while (*p && !isspace(*p))
        ++p;
    *pLen = p - start;
    return p;
}


int HtAccessParser::isKnownModule(const char *pModule, int len)
{
    static const char *s_knownModules[] = {
        "mod_rewrite.c", "mod_headers.c", "mod_expires.c",
        "mod_deflate.c", "mod_ssl.c", "mod_auth_basic.c",
        "mod_auth_digest.c", "mod_authz_core.c", "mod_setenvif.c",
        "mod_mime.c", "mod_dir.c", "mod_autoindex.c",
        "mod_alias.c", "mod_filter.c", "mod_env.c",
        NULL
    };
    for (int i = 0; s_knownModules[i]; ++i)
    {
        if ((int)strlen(s_knownModules[i]) == len
            && strncasecmp(pModule, s_knownModules[i], len) == 0)
            return 1;
    }
    return 0;
}


int HtAccessParser::handleAuthType(HtParseState &state, const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    if (strncasecmp(pArgs, "basic", 5) == 0)
        state.authType.setStr("basic", 5);
    else if (strncasecmp(pArgs, "digest", 6) == 0)
        state.authType.setStr("digest", 6);
    else
    {
        LS_INFO("[.htaccess] Unsupported AuthType: %s", pArgs);
        return -1;
    }
    return 0;
}


int HtAccessParser::handleAuthName(HtParseState &state, const char *pArgs)
{
    const char *pStart;
    int argLen;
    extractArg(pArgs, &pStart, &argLen);
    if (argLen > 0)
        state.authName.setStr(pStart, argLen);
    return 0;
}


int HtAccessParser::handleAuthUserFile(HtParseState &state, const char *pArgs,
                                       HttpContext *pContext)
{
    pArgs = skipWhitespace(pArgs);
    int len = strlen(pArgs);
    len = trimmedLen(pArgs, len);

    // Security: reject path traversal
    if (strstr(pArgs, "..") != NULL)
    {
        LS_WARN("[.htaccess] AuthUserFile path traversal rejected: %s", pArgs);
        return -1;
    }
    state.authUserFile.setStr(pArgs, len);
    return 0;
}


int HtAccessParser::handleAuthGroupFile(HtParseState &state, const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    int len = strlen(pArgs);
    len = trimmedLen(pArgs, len);

    if (strstr(pArgs, "..") != NULL)
    {
        LS_WARN("[.htaccess] AuthGroupFile path traversal rejected: %s", pArgs);
        return -1;
    }
    state.authGroupFile.setStr(pArgs, len);
    return 0;
}


int HtAccessParser::handleRequire(HtParseState &state, const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    int len = strlen(pArgs);
    len = trimmedLen(pArgs, len);
    if (len > 0)
        state.requireLine.setStr(pArgs, len);
    return 0;
}


int HtAccessParser::finalizeAuth(HtParseState &state, HttpContext *pContext)
{
    if (state.authUserFile.len() == 0)
        return 0;

    PlainFileUserDir *pUserDir = new PlainFileUserDir();
    if (!pUserDir)
        return -1;

    const char *pGroupFile = NULL;
    if (state.authGroupFile.len() > 0)
        pGroupFile = state.authGroupFile.c_str();

    if (pUserDir->setDataStore(state.authUserFile.c_str(), pGroupFile) != 0)
    {
        LS_WARN("[.htaccess] Failed to open AuthUserFile: %s",
                state.authUserFile.c_str());
        delete pUserDir;
        return -1;
    }

    const char *pRealm = "htaccess";
    if (state.authName.len() > 0)
        pRealm = state.authName.c_str();
    pUserDir->setName(pRealm);

    HTAuth *pAuth = new HTAuth();
    if (!pAuth)
    {
        delete pUserDir;
        return -1;
    }
    pAuth->setName(pRealm);
    pAuth->setUserDir(pUserDir);

    if (state.authType.len() > 0
        && strncasecmp(state.authType.c_str(), "digest", 6) == 0)
        pAuth->setAuthType(HTAuth::AUTH_DIGEST);
    else
        pAuth->setAuthType(HTAuth::AUTH_BASIC);

    pContext->setHTAuth(pAuth);
    pContext->setConfigBit(BIT_AUTH, 1);
    // Mark that we own the UserDir (for cleanup in releaseHTAConf)
    pContext->setConfigBit(BIT_HTA_OWNS_UDIR, 1);

    const char *pReq = "valid-user";
    if (state.requireLine.len() > 0)
        pReq = state.requireLine.c_str();
    pContext->setAuthRequired(pReq);

    return 0;
}


int HtAccessParser::handleOrder(HtParseState &state, HttpContext *pContext,
                                const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    // "allow,deny" means default deny — "deny,allow" means default allow
    if (strncasecmp(pArgs, "allow", 5) == 0)
    {
        // allow,deny => default deny: add deny from all first
        pContext->addAccessRule("ALL", 0);
    }
    else if (strncasecmp(pArgs, "deny", 4) == 0)
    {
        // deny,allow => default allow: add allow from all first
        pContext->addAccessRule("ALL", 1);
    }
    return 0;
}


int HtAccessParser::handleAllow(HttpContext *pContext, const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    // "Allow from <list>"
    if (strncasecmp(pArgs, "from", 4) == 0)
    {
        pArgs += 4;
        pArgs = skipWhitespace(pArgs);
        // Check for env= syntax: "Allow from env=VARNAME"
        if (strncasecmp(pArgs, "env=", 4) == 0)
        {
            const char *pEnvName = pArgs + 4;
            int envLen = strlen(pEnvName);
            envLen = trimmedLen(pEnvName, envLen);
            if (envLen > 0)
            {
                char envBuf[1024];
                if (envLen >= (int)sizeof(envBuf))
                    envLen = sizeof(envBuf) - 1;
                memcpy(envBuf, pEnvName, envLen);
                envBuf[envLen] = 0;
                pContext->addEnvAccessRule(envBuf, 1);
            }
            return 0;
        }
        int len = strlen(pArgs);
        len = trimmedLen(pArgs, len);
        if (len > 0)
        {
            char buf[4096];
            if (len >= (int)sizeof(buf))
                len = sizeof(buf) - 1;
            memcpy(buf, pArgs, len);
            buf[len] = 0;
            // Replace "all" with "*"
            if (strncasecmp(buf, "all", 3) == 0)
                pContext->addAccessRule("ALL", 1);
            else
                pContext->addAccessRule(buf, 1);
        }
    }
    return 0;
}


int HtAccessParser::handleDeny(HttpContext *pContext, const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    if (strncasecmp(pArgs, "from", 4) == 0)
    {
        pArgs += 4;
        pArgs = skipWhitespace(pArgs);
        // Check for env= syntax: "Deny from env=VARNAME"
        if (strncasecmp(pArgs, "env=", 4) == 0)
        {
            const char *pEnvName = pArgs + 4;
            int envLen = strlen(pEnvName);
            envLen = trimmedLen(pEnvName, envLen);
            if (envLen > 0)
            {
                char envBuf[1024];
                if (envLen >= (int)sizeof(envBuf))
                    envLen = sizeof(envBuf) - 1;
                memcpy(envBuf, pEnvName, envLen);
                envBuf[envLen] = 0;
                pContext->addEnvAccessRule(envBuf, 0);
            }
            return 0;
        }
        int len = strlen(pArgs);
        len = trimmedLen(pArgs, len);
        if (len > 0)
        {
            char buf[4096];
            if (len >= (int)sizeof(buf))
                len = sizeof(buf) - 1;
            memcpy(buf, pArgs, len);
            buf[len] = 0;
            if (strncasecmp(buf, "all", 3) == 0)
                pContext->addAccessRule("ALL", 0);
            else
                pContext->addAccessRule(buf, 0);
        }
    }
    return 0;
}


int HtAccessParser::handleSatisfy(HttpContext *pContext, const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    if (strncasecmp(pArgs, "any", 3) == 0)
        pContext->setSatisfyAny(1);
    else if (strncasecmp(pArgs, "all", 3) == 0)
        pContext->setSatisfyAny(0);
    return 0;
}


int HtAccessParser::handleErrorDocument(HttpContext *pContext,
                                        const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    // ErrorDocument <code> <url>
    char codeStr[8];
    int i = 0;
    while (*pArgs && !isspace(*pArgs) && i < 7)
        codeStr[i++] = *pArgs++;
    codeStr[i] = 0;

    pArgs = skipWhitespace(pArgs);

    const char *pUrl;
    int urlLen;
    extractArg(pArgs, &pUrl, &urlLen);
    if (urlLen <= 0)
        return -1;

    char urlBuf[4096];
    if (urlLen >= (int)sizeof(urlBuf))
        urlLen = sizeof(urlBuf) - 1;
    memcpy(urlBuf, pUrl, urlLen);
    urlBuf[urlLen] = 0;

    return pContext->setCustomErrUrls(codeStr, urlBuf);
}


int HtAccessParser::handleDirectoryIndex(HttpContext *pContext,
                                         const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    int len = strlen(pArgs);
    len = trimmedLen(pArgs, len);
    if (len <= 0)
        return 0;
    char buf[4096];
    if (len >= (int)sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, pArgs, len);
    buf[len] = 0;
    pContext->clearDirIndexes();
    pContext->addDirIndexes(buf);
    return 0;
}


int HtAccessParser::handleOptions(HttpContext *pContext, const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    while (*pArgs)
    {
        int enable = 1;
        if (*pArgs == '+')
        {
            enable = 1;
            ++pArgs;
        }
        else if (*pArgs == '-')
        {
            enable = 0;
            ++pArgs;
        }

        if (strncasecmp(pArgs, "Indexes", 7) == 0)
        {
            if (enable)
                pContext->setAutoIndex(1);
            else
                pContext->setAutoIndexOff(1);
            pArgs += 7;
        }
        else if (strncasecmp(pArgs, "Includes", 8) == 0)
        {
            if (strncasecmp(pArgs + 8, "NOEXEC", 6) == 0)
            {
                pContext->setIncludesNoExec(enable);
                pArgs += 14;
            }
            else
            {
                pContext->setFeaturesBit(BIT_F_INCLUDES, enable);
                pArgs += 8;
            }
        }
        else if (strncasecmp(pArgs, "FollowSymLinks", 14) == 0)
        {
            LS_DBG("[.htaccess] Options FollowSymLinks is a vhost-level "
                   "setting in OLS, ignored");
            pArgs += 14;
        }
        else if (strncasecmp(pArgs, "SymLinksIfOwnerMatch", 20) == 0)
        {
            LS_DBG("[.htaccess] Options SymLinksIfOwnerMatch ignored");
            pArgs += 20;
        }
        else if (strncasecmp(pArgs, "MultiViews", 10) == 0)
        {
            LS_INFO("[.htaccess] Options MultiViews not supported, ignored");
            pArgs += 10;
        }
        else if (strncasecmp(pArgs, "ExecCGI", 7) == 0)
        {
            pContext->enableScript(enable);
            pArgs += 7;
        }
        else if (strncasecmp(pArgs, "All", 3) == 0)
        {
            if (enable)
            {
                pContext->setAutoIndex(1);
                pContext->setFeaturesBit(BIT_F_INCLUDES, 1);
                pContext->enableScript(1);
            }
            else
            {
                pContext->setAutoIndexOff(1);
                pContext->setFeaturesBit(BIT_F_INCLUDES, 0);
                pContext->enableScript(0);
            }
            pArgs += 3;
        }
        else if (strncasecmp(pArgs, "None", 4) == 0)
        {
            pContext->setAutoIndexOff(1);
            pContext->setFeaturesBit(BIT_F_INCLUDES, 0);
            pContext->enableScript(0);
            pArgs += 4;
        }
        else
        {
            // Skip unknown option word
            while (*pArgs && !isspace(*pArgs))
                ++pArgs;
        }

        while (*pArgs && (isspace(*pArgs) || *pArgs == ','))
            ++pArgs;
    }
    return 0;
}


int HtAccessParser::handleAddType(HttpContext *pContext, const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    // AddType mime/type .ext1 .ext2 ...
    const char *pMime;
    int mimeLen;
    pArgs = extractArg(pArgs, &pMime, &mimeLen);
    if (mimeLen <= 0)
        return -1;

    char mimeBuf[256];
    if (mimeLen >= (int)sizeof(mimeBuf))
        mimeLen = sizeof(mimeBuf) - 1;
    memcpy(mimeBuf, pMime, mimeLen);
    mimeBuf[mimeLen] = 0;

    // Process each extension
    while (*pArgs)
    {
        const char *pExt;
        int extLen;
        pArgs = extractArg(pArgs, &pExt, &extLen);
        if (extLen <= 0)
            break;
        char extBuf[64];
        if (extLen >= (int)sizeof(extBuf))
            extLen = sizeof(extBuf) - 1;
        // Strip leading dot
        if (*pExt == '.')
        {
            ++pExt;
            --extLen;
        }
        memcpy(extBuf, pExt, extLen);
        extBuf[extLen] = 0;
        pContext->addMIME(mimeBuf, extBuf);
    }
    return 0;
}


int HtAccessParser::handleForceType(HttpContext *pContext, const char *pArgs,
                                    const char *pLogId)
{
    pArgs = skipWhitespace(pArgs);
    int len = strlen(pArgs);
    len = trimmedLen(pArgs, len);
    if (len <= 0)
        return 0;
    char buf[256];
    if (len >= (int)sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, pArgs, len);
    buf[len] = 0;

    if (strncasecmp(buf, "none", 4) == 0)
        return 0;
    return pContext->setForceType(buf, pLogId);
}


int HtAccessParser::handleDefaultType(HttpContext *pContext, const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    int len = strlen(pArgs);
    len = trimmedLen(pArgs, len);
    if (len <= 0 || strncasecmp(pArgs, "none", 4) == 0)
        return 0;

    if (pContext->initMIME() == 0 && pContext->getMIME())
    {
        char buf[256];
        if (len >= (int)sizeof(buf))
            len = sizeof(buf) - 1;
        memcpy(buf, pArgs, len);
        buf[len] = 0;
        pContext->getMIME()->initDefault(buf);
    }
    return 0;
}


int HtAccessParser::handleAddHandler(HttpContext *pContext, const char *pArgs)
{
    // AddHandler handler-name .ext1 .ext2
    // Map to addMIME using the handler-name as a hint
    pArgs = skipWhitespace(pArgs);
    const char *pHandler;
    int handlerLen;
    pArgs = extractArg(pArgs, &pHandler, &handlerLen);
    if (handlerLen <= 0)
        return -1;

    // For common handlers, map to application/x-httpd-* MIME type
    char mimeBuf[256];
    if (strncasecmp(pHandler, "cgi-script", 10) == 0)
        lstrncpy(mimeBuf, "application/x-httpd-cgi", sizeof(mimeBuf));
    else if (strncasecmp(pHandler, "php", 3) == 0
             || memmem(pHandler, handlerLen, "php", 3) != NULL)
        lstrncpy(mimeBuf, "application/x-httpd-php", sizeof(mimeBuf));
    else if (strncasecmp(pHandler, "server-parsed", 13) == 0
             || strncasecmp(pHandler, "server-info", 11) == 0)
    {
        LS_INFO("[.htaccess] AddHandler '%.*s' not supported, ignored",
                handlerLen, pHandler);
        return 0;
    }
    else
    {
        // Generic: application/x-httpd-<handler>
        snprintf(mimeBuf, sizeof(mimeBuf), "application/x-httpd-%.*s",
                 handlerLen, pHandler);
    }

    while (*pArgs)
    {
        const char *pExt;
        int extLen;
        pArgs = extractArg(pArgs, &pExt, &extLen);
        if (extLen <= 0)
            break;
        char extBuf[64];
        if (*pExt == '.')
        {
            ++pExt;
            --extLen;
        }
        if (extLen >= (int)sizeof(extBuf))
            extLen = sizeof(extBuf) - 1;
        memcpy(extBuf, pExt, extLen);
        extBuf[extLen] = 0;
        pContext->addMIME(mimeBuf, extBuf);
    }
    return 0;
}


int HtAccessParser::handlePhpValue(HttpContext *pContext, int id,
                                   const char *pArgs)
{
    PHPConfig *pConfig = pContext->getPHPConfig();
    if (!pConfig)
    {
        pConfig = new PHPConfig();
        if (!pConfig)
            return -1;
        pContext->setPHPConfig(pConfig);
    }
    char errBuf[1024] = {0};
    pConfig->parse(id, pArgs, errBuf, sizeof(errBuf));
    if (errBuf[0])
        LS_WARN("[.htaccess] PHP config error: %s", errBuf);
    if (pConfig->size() > 0)
        pConfig->buildLsapiEnv();
    return 0;
}


int HtAccessParser::handleHeader(HttpContext *pContext, const char *pLine,
                                 int lineLen, const char *pLogId)
{
    return pContext->setHeaderOps(pLogId, pLine, lineLen);
}


int HtAccessParser::handleExpiresActive(HttpContext *pContext,
                                        const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    if (strncasecmp(pArgs, "on", 2) == 0)
    {
        pContext->getExpires().enable(1);
        pContext->getExpires().setBit(CONFIG_EXPIRES);
        pContext->setConfigBit(BIT_ENABLE_EXPIRES, 1);
    }
    else if (strncasecmp(pArgs, "off", 3) == 0)
    {
        pContext->getExpires().enable(0);
    }
    return 0;
}


int HtAccessParser::handleExpiresDefault(HttpContext *pContext,
                                         const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    const char *pStart;
    int argLen;
    extractArg(pArgs, &pStart, &argLen);
    if (argLen <= 0)
        return -1;

    char buf[256];
    // May be quoted
    if (*pArgs == '"')
    {
        int len = strlen(pArgs);
        len = trimmedLen(pArgs, len);
        if (len >= (int)sizeof(buf))
            len = sizeof(buf) - 1;
        memcpy(buf, pArgs, len);
        buf[len] = 0;
        // Strip quotes
        char *p = buf;
        if (*p == '"') ++p;
        int blen = strlen(p);
        if (blen > 0 && p[blen - 1] == '"')
            p[blen - 1] = 0;
        pContext->getExpires().parse(p);
    }
    else
    {
        if (argLen >= (int)sizeof(buf))
            argLen = sizeof(buf) - 1;
        memcpy(buf, pStart, argLen);
        buf[argLen] = 0;
        pContext->getExpires().parse(buf);
    }
    pContext->setConfigBit(BIT_EXPIRES_DEFAULT, 1);
    return 0;
}


int HtAccessParser::handleExpiresByType(HttpContext *pContext,
                                        const char *pArgs)
{
    // ExpiresByType mime/type "access plus ..."
    pArgs = skipWhitespace(pArgs);
    int len = strlen(pArgs);
    len = trimmedLen(pArgs, len);
    if (len <= 0)
        return -1;
    char buf[512];
    if (len >= (int)sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, pArgs, len);
    buf[len] = 0;
    return pContext->setExpiresByType(buf);
}


int HtAccessParser::handleSetOutputFilter(HttpContext *pContext,
                                          const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    if (strncasecmp(pArgs, "DEFLATE", 7) == 0)
    {
        pContext->getExpires().setCompressible(1);
        pContext->getExpires().setBit(CONFIG_COMPRESS);
    }
    return 0;
}


int HtAccessParser::handleAddOutputFilterByType(HttpContext *pContext,
                                                const char *pArgs)
{
    // AddOutputFilterByType DEFLATE type1 type2 ...
    pArgs = skipWhitespace(pArgs);
    if (strncasecmp(pArgs, "DEFLATE", 7) != 0)
        return 0;
    pArgs += 7;
    pArgs = skipWhitespace(pArgs);
    int len = strlen(pArgs);
    len = trimmedLen(pArgs, len);
    if (len <= 0)
        return 0;
    char buf[2048];
    if (len >= (int)sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, pArgs, len);
    buf[len] = 0;
    return pContext->setCompressByType(buf);
}


int HtAccessParser::handleFilesBlock(HtParseState &state,
                                     HttpContext *pContext,
                                     const char *pLine, int isMatch)
{
    if (state.pFilesCtx)
    {
        LS_WARN("[.htaccess] Nested <Files> blocks not supported, ignoring");
        return -1;
    }

    // Parse pattern from <Files "pattern"> or <FilesMatch "pattern">
    const char *p = pLine;
    // Skip to first space/quote after directive name
    while (*p && *p != ' ' && *p != '\t' && *p != '"')
        ++p;
    p = skipWhitespace(p);

    // Extract pattern
    const char *pPattern;
    int patLen;
    extractArg(p, &pPattern, &patLen);
    if (patLen <= 0)
        return -1;

    // Strip trailing >
    while (patLen > 0 && (pPattern[patLen - 1] == '>' || isspace(pPattern[patLen - 1])))
        --patLen;
    if (patLen <= 0)
        return -1;

    char patBuf[1024];
    if (patLen >= (int)sizeof(patBuf))
        patLen = sizeof(patBuf) - 1;
    memcpy(patBuf, pPattern, patLen);
    patBuf[patLen] = 0;

    HttpContext *pFilesCtx = new HttpContext();
    if (!pFilesCtx)
        return -1;
    if (pFilesCtx->setFilesMatch(patBuf, isMatch) != 0)
    {
        delete pFilesCtx;
        return -1;
    }
    pFilesCtx->allocateInternal();
    pFilesCtx->inherit(pContext);
    state.pFilesCtx = pFilesCtx;
    return 0;
}


int HtAccessParser::handleEndFilesBlock(HtParseState &state,
                                        HttpContext *pContext)
{
    if (!state.pFilesCtx)
        return -1;
    pContext->addFilesMatchContext(state.pFilesCtx);
    state.pFilesCtx = NULL;
    return 0;
}


int HtAccessParser::handleIfModule(HtParseState &state, const char *pArgs)
{
    if (state.ifModuleSkip > 0)
    {
        // Already skipping, just track nesting
        ++state.ifModuleSkip;
        return 0;
    }

    pArgs = skipWhitespace(pArgs);
    int negated = 0;
    if (*pArgs == '!')
    {
        negated = 1;
        ++pArgs;
    }
    // Strip leading whitespace again
    pArgs = skipWhitespace(pArgs);

    // Get module name - strip trailing >
    int len = strlen(pArgs);
    len = trimmedLen(pArgs, len);
    while (len > 0 && (pArgs[len - 1] == '>' || isspace(pArgs[len - 1])))
        --len;

    int known = isKnownModule(pArgs, len);
    if ((!negated && !known) || (negated && known))
    {
        // Skip this block
        state.ifModuleSkip = 1;
        LS_DBG("[.htaccess] Skipping <IfModule> block for: %.*s", len, pArgs);
    }
    else
    {
        ++state.ifModuleNesting;
    }
    return 0;
}


int HtAccessParser::handleEndIfModule(HtParseState &state)
{
    if (state.ifModuleSkip > 0)
    {
        --state.ifModuleSkip;
        return 0;
    }
    if (state.ifModuleNesting > 0)
        --state.ifModuleNesting;
    return 0;
}


int HtAccessParser::handleRedirect(HtParseState &state, const char *pArgs,
                                   int status)
{
    pArgs = skipWhitespace(pArgs);

    // Redirect [status] url-path url
    // If status is 0, parse it from args
    if (status == 0)
    {
        // Check for status code or keyword
        if (isdigit(*pArgs))
        {
            status = atoi(pArgs);
            while (*pArgs && !isspace(*pArgs))
                ++pArgs;
            pArgs = skipWhitespace(pArgs);
        }
        else if (strncasecmp(pArgs, "permanent", 9) == 0)
        {
            status = 301;
            pArgs += 9;
            pArgs = skipWhitespace(pArgs);
        }
        else if (strncasecmp(pArgs, "temp", 4) == 0)
        {
            status = 302;
            pArgs += 4;
            pArgs = skipWhitespace(pArgs);
        }
        else if (strncasecmp(pArgs, "seeother", 8) == 0)
        {
            status = 303;
            pArgs += 8;
            pArgs = skipWhitespace(pArgs);
        }
        else if (strncasecmp(pArgs, "gone", 4) == 0)
        {
            status = 410;
            pArgs += 4;
            pArgs = skipWhitespace(pArgs);
        }
        else
            status = 302;
    }

    // url-path
    const char *pPath;
    int pathLen;
    pArgs = extractArg(pArgs, &pPath, &pathLen);
    if (pathLen <= 0)
        return -1;

    // target url (not needed for 410 Gone)
    const char *pTarget = "";
    int targetLen = 0;
    if (status != 410)
    {
        pArgs = extractArg(pArgs, &pTarget, &targetLen);
        if (targetLen <= 0)
            return -1;
    }

    // Convert to RewriteRule: ^escaped-path$ target [R=status,L]
    char escapedPath[4096];
    escapeRegex(pPath, pathLen, escapedPath, sizeof(escapedPath));

    char rewriteBuf[8192];
    if (status == 410)
    {
        snprintf(rewriteBuf, sizeof(rewriteBuf),
                 "RewriteRule ^%s$ - [G,L]\n", escapedPath);
    }
    else
    {
        snprintf(rewriteBuf, sizeof(rewriteBuf),
                 "RewriteRule ^%s$ %.*s [R=%d,L]\n",
                 escapedPath, targetLen, pTarget, status);
    }
    state.rewriteBuf.append(rewriteBuf, strlen(rewriteBuf));
    return 0;
}


int HtAccessParser::handleRedirectMatch(HtParseState &state,
                                        const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);

    int status = 302;
    // Check for status code
    if (isdigit(*pArgs))
    {
        status = atoi(pArgs);
        while (*pArgs && !isspace(*pArgs))
            ++pArgs;
        pArgs = skipWhitespace(pArgs);
    }

    // regex
    const char *pRegex;
    int regexLen;
    pArgs = extractArg(pArgs, &pRegex, &regexLen);
    if (regexLen <= 0)
        return -1;

    // target url
    const char *pTarget;
    int targetLen;
    extractArg(pArgs, &pTarget, &targetLen);
    if (targetLen <= 0)
        return -1;

    char rewriteBuf[8192];
    snprintf(rewriteBuf, sizeof(rewriteBuf),
             "RewriteRule %.*s %.*s [R=%d,L]\n",
             regexLen, pRegex, targetLen, pTarget, status);
    state.rewriteBuf.append(rewriteBuf, strlen(rewriteBuf));
    return 0;
}


int HtAccessParser::handleSSLRequireSSL(HttpContext *pContext)
{
    // Require SSL — use RewriteRule to redirect if not HTTPS
    // This is handled at auth check phase — for now, set a config bit
    // We'll emit a rewrite rule: RewriteCond %{HTTPS} !on → RewriteRule ^ - [F]
    // Actually, better to use the access control mechanism
    LS_DBG("[.htaccess] SSLRequireSSL set");
    // TODO: Ideally set a flag on context that's checked during auth.
    // For now, no-op logged. The proper implementation requires a new
    // config bit for SSL requirement.
    return 0;
}


int HtAccessParser::handleSetEnv(HttpContext *pContext, const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    // Set KV
    const char *pKey = pArgs;
    while (*pArgs && !isspace(*pArgs))
        ++pArgs;
    int keyLen = pArgs - pKey;
    if (keyLen <= 0)
        return -1;

    pArgs = skipWhitespace(pArgs);
    int valLen = strlen(pArgs);
    valLen = trimmedLen(pArgs, valLen);

    // Store as "KEY=VALUE"
    char buf[4096];
    if (valLen > 0)
        snprintf(buf, sizeof(buf), "%.*s=%.*s", keyLen, pKey, valLen, pArgs);
    else
        snprintf(buf, sizeof(buf), "%.*s=", keyLen, pKey);

    return pContext->addCtxEnv(buf);
}


int HtAccessParser::handleUnsetEnv(HttpContext *pContext, const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    int len = strlen(pArgs);
    len = trimmedLen(pArgs, len);
    if (len <= 0)
        return -1;

    // Store as "!KEY"
    char buf[4096];
    snprintf(buf, sizeof(buf), "!%.*s", len, pArgs);
    return pContext->addCtxEnv(buf);
}


int HtAccessParser::handleSetEnvIf(HttpContext *pContext, const char *pArgs,
                                    int noCase)
{
    pArgs = skipWhitespace(pArgs);

    // SetEnvIf attribute regex [!]env-variable[=value] ...
    const char *pAttr;
    int attrLen;
    pArgs = extractArg(pArgs, &pAttr, &attrLen);
    if (attrLen <= 0)
        return -1;

    // Parse regex
    const char *pPattern;
    int patLen;
    pArgs = extractArg(pArgs, &pPattern, &patLen);
    if (patLen <= 0)
        return -1;

    char patBuf[4096];
    if (patLen >= (int)sizeof(patBuf))
        patLen = sizeof(patBuf) - 1;
    memcpy(patBuf, pPattern, patLen);
    patBuf[patLen] = 0;

    // Parse env var assignments (there can be multiple)
    while (*pArgs)
    {
        const char *pEnv;
        int envLen;
        pArgs = extractArg(pArgs, &pEnv, &envLen);
        if (envLen <= 0)
            break;

        char envBuf[1024];
        if (envLen >= (int)sizeof(envBuf))
            envLen = sizeof(envBuf) - 1;
        memcpy(envBuf, pEnv, envLen);
        envBuf[envLen] = 0;

        char attrBuf[256];
        if (attrLen >= (int)sizeof(attrBuf))
            attrLen = sizeof(attrBuf) - 1;
        memcpy(attrBuf, pAttr, attrLen);
        attrBuf[attrLen] = 0;

        pContext->addEnvIfRule(attrBuf, attrLen, patBuf, noCase, envBuf);
    }
    return 0;
}


int HtAccessParser::handleSetHandler(HttpContext *pContext, const char *pArgs,
                                      const char *pLogId)
{
    pArgs = skipWhitespace(pArgs);

    // Strip quotes
    const char *pStart;
    int argLen;
    extractArg(pArgs, &pStart, &argLen);
    if (argLen <= 0)
        return -1;

    char buf[256];
    if (argLen >= (int)sizeof(buf))
        argLen = sizeof(buf) - 1;
    memcpy(buf, pStart, argLen);
    buf[argLen] = 0;

    // SetHandler None to clear handler
    if (strncasecmp(buf, "none", 4) == 0)
        return 0;

    // proxy:fcgi:// log warning, needs vhost-level config
    if (strncasecmp(buf, "proxy:", 6) == 0)
    {
        LS_WARN("[.htaccess] SetHandler '%s' — proxy:fcgi handlers should be "
                "configured at the vhost level in OLS, not in .htaccess", buf);
        return 0;
    }

    // server-status, server-info not applicable
    if (strncasecmp(buf, "server-", 7) == 0)
        return 0;

    // Map common handler names to MIME types
    char mimeBuf[256];
    if (strncasecmp(buf, "application/", 12) == 0)
    {
        // Already a MIME type
        lstrncpy(mimeBuf, buf, sizeof(mimeBuf));
    }
    else if (strncasecmp(buf, "cgi-script", 10) == 0)
        lstrncpy(mimeBuf, "application/x-httpd-cgi", sizeof(mimeBuf));
    else if (strncasecmp(buf, "php", 3) == 0
             || strcasestr(buf, "php") != NULL)
        lstrncpy(mimeBuf, "application/x-httpd-php", sizeof(mimeBuf));
    else if (strncasecmp(buf, "default-handler", 15) == 0)
        return 0;  // No-op: serve as static file (default)
    else
    {
        // Generic: try as application/x-httpd-<handler>
        snprintf(mimeBuf, sizeof(mimeBuf), "application/x-httpd-%s", buf);
    }

    return pContext->setForceType(mimeBuf, pLogId);
}


int HtAccessParser::handleFallbackResource(HtParseState &state,
                                            const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);
    int len = strlen(pArgs);
    len = trimmedLen(pArgs, len);
    if (len <= 0)
        return 0;

    // FallbackResource disabled = no-op
    if (strncasecmp(pArgs, "disabled", 8) == 0)
        return 0;

    // Convert to rewrite rules:
    //   RewriteEngine On
    //   RewriteCond %{REQUEST_FILENAME} !-f
    //   RewriteCond %{REQUEST_FILENAME} !-d
    //   RewriteRule . <fallback-uri> [L]
    state.rewriteBuf.append("RewriteEngine On\n", 18);
    state.rewriteBuf.append("RewriteCond %{REQUEST_FILENAME} !-f\n", 35);
    state.rewriteBuf.append("RewriteCond %{REQUEST_FILENAME} !-d\n", 35);

    char rule[1024];
    int ruleLen = snprintf(rule, sizeof(rule), "RewriteRule . %.*s [L]\n",
                           len, pArgs);
    state.rewriteBuf.append(rule, ruleLen);
    return 0;
}


int HtAccessParser::handleFileETag(HttpContext *pContext, const char *pArgs)
{
    pArgs = skipWhitespace(pArgs);

    if (strncasecmp(pArgs, "None", 4) == 0)
    {
        pContext->setFileEtag(ETAG_NONE);
        return 0;
    }
    if (strncasecmp(pArgs, "All", 3) == 0)
    {
        pContext->setFileEtag(ETAG_ALL);
        return 0;
    }

    int etag = pContext->getFileEtag();
    int modified = 0;
    while (*pArgs)
    {
        int enable = 1;
        if (*pArgs == '+')
        {
            enable = 1;
            ++pArgs;
        }
        else if (*pArgs == '-')
        {
            enable = 0;
            ++pArgs;
        }

        if (strncasecmp(pArgs, "INode", 5) == 0)
        {
            if (enable)
                etag |= ETAG_INODE;
            else
                etag &= ~ETAG_INODE;
            pArgs += 5;
            modified = 1;
        }
        else if (strncasecmp(pArgs, "MTime", 5) == 0)
        {
            if (enable)
                etag |= ETAG_MTIME;
            else
                etag &= ~ETAG_MTIME;
            pArgs += 5;
            modified = 1;
        }
        else if (strncasecmp(pArgs, "Size", 4) == 0)
        {
            if (enable)
                etag |= ETAG_SIZE;
            else
                etag &= ~ETAG_SIZE;
            pArgs += 4;
            modified = 1;
        }
        else
        {
            // Skip unknown word
            while (*pArgs && !isspace(*pArgs))
                ++pArgs;
        }

        while (*pArgs && isspace(*pArgs))
            ++pArgs;
    }

    if (modified)
        pContext->setFileEtag(etag);
    return 0;
}


int HtAccessParser::finalizeRewrite(HtParseState &state,
                                    HttpContext *pContext,
                                    const RewriteMapList *pMapList)
{
    if (state.rewriteBuf.len() == 0)
        return 0;

    char *pRules = state.rewriteBuf.buf();
    RewriteRuleList *pRuleList = new RewriteRuleList();
    if (!pRuleList)
        return -1;

    RewriteRule::setLogger(NULL, TmpLogId::getLogId());
    if (RewriteEngine::parseRules(pRules, pRuleList, pMapList, pContext) == 0
        && pRuleList->begin())
    {
        pContext->setRewriteRules(pRuleList);
    }
    else
        delete pRuleList;

    return 0;
}


// Main parse entry point
int HtAccessParser::parse(const char *pBuf, int len,
                          HttpContext *pContext,
                          const RewriteMapList *pMapList)
{
    if (!pBuf || len <= 0 || !pContext)
        return -1;

    HtParseState state;
    state.pLogId = TmpLogId::getLogId();

    // Make a mutable copy for line processing
    char *buf = new char[len + 1];
    if (!buf)
        return -1;
    memcpy(buf, pBuf, len);
    buf[len] = 0;

    char *pLine = buf;
    char *pEnd = buf + len;

    while (pLine < pEnd)
    {
        // Skip leading whitespace
        while (pLine < pEnd && isspace(*pLine))
            ++pLine;
        if (pLine >= pEnd)
            break;

        // Find end of line
        char *pLineEnd = pLine;
        while (pLineEnd < pEnd && *pLineEnd != '\n' && *pLineEnd != '\r')
            ++pLineEnd;

        // Handle line continuation (backslash before newline)
        while (pLineEnd > pLine && *(pLineEnd - 1) == '\\' && pLineEnd < pEnd)
        {
            *(pLineEnd - 1) = ' ';
            if (*pLineEnd == '\r')
                *pLineEnd++ = ' ';
            if (pLineEnd < pEnd && *pLineEnd == '\n')
                *pLineEnd++ = ' ';
            while (pLineEnd < pEnd && *pLineEnd != '\n' && *pLineEnd != '\r')
                ++pLineEnd;
        }

        // Null-terminate line
        char saved = *pLineEnd;
        *pLineEnd = 0;

        int lineLen = pLineEnd - pLine;
        int trimLen = trimmedLen(pLine, lineLen);

        // Skip empty lines and comments
        if (trimLen == 0 || *pLine == '#')
        {
            *pLineEnd = saved;
            pLine = pLineEnd + 1;
            continue;
        }

        // Handle </IfModule>
        if (strncasecmp(pLine, "</IfModule", 10) == 0)
        {
            handleEndIfModule(state);
            *pLineEnd = saved;
            pLine = pLineEnd + 1;
            continue;
        }

        // If skipping due to IfModule, just track nesting
        if (state.ifModuleSkip > 0)
        {
            if (strncasecmp(pLine, "<IfModule", 9) == 0)
                ++state.ifModuleSkip;
            *pLineEnd = saved;
            pLine = pLineEnd + 1;
            continue;
        }

        // Determine target context (parent or FilesMatch child)
        HttpContext *pTargetCtx = state.pFilesCtx ? state.pFilesCtx : pContext;

        // Dispatch directives
        if (strncasecmp(pLine, "<IfModule", 9) == 0)
        {
            handleIfModule(state, pLine + 9);
        }
        else if (strncasecmp(pLine, "<Files ", 7) == 0
                 || strncasecmp(pLine, "<Files\t", 7) == 0)
        {
            handleFilesBlock(state, pContext, pLine + 7, 0);
        }
        else if (strncasecmp(pLine, "<FilesMatch ", 12) == 0
                 || strncasecmp(pLine, "<FilesMatch\t", 12) == 0)
        {
            handleFilesBlock(state, pContext, pLine + 12, 1);
        }
        else if (strncasecmp(pLine, "</Files", 7) == 0
                 || strncasecmp(pLine, "</FilesMatch", 12) == 0)
        {
            handleEndFilesBlock(state, pContext);
        }
        // Rewrite directives — collect into buffer for RewriteEngine::parseRules
        else if ((strncasecmp(pLine, "RewriteEngine", 13) == 0
                  && (lineLen == 13 || isspace(pLine[13])))
                 || (strncasecmp(pLine, "RewriteBase", 11) == 0
                     && (lineLen == 11 || isspace(pLine[11])))
                 || (strncasecmp(pLine, "RewriteCond", 11) == 0
                     && isspace(pLine[11]))
                 || (strncasecmp(pLine, "RewriteRule", 11) == 0
                     && isspace(pLine[11]))
                 || (strncasecmp(pLine, "RewriteFile", 11) == 0
                     && isspace(pLine[11]))
                 || (strncasecmp(pLine, "CacheKeyModify", 14) == 0
                     && isspace(pLine[14])))
        {
            state.rewriteBuf.append(pLine, lineLen);
            state.rewriteBuf.append("\n", 1);
        }
        // Authentication directives
        else if (strncasecmp(pLine, "AuthType", 8) == 0 && isspace(pLine[8]))
        {
            handleAuthType(state, pLine + 8);
        }
        else if (strncasecmp(pLine, "AuthName", 8) == 0 && isspace(pLine[8]))
        {
            handleAuthName(state, pLine + 8);
        }
        else if (strncasecmp(pLine, "AuthUserFile", 12) == 0
                 && isspace(pLine[12]))
        {
            handleAuthUserFile(state, pLine + 12, pTargetCtx);
        }
        else if (strncasecmp(pLine, "AuthGroupFile", 13) == 0
                 && isspace(pLine[13]))
        {
            handleAuthGroupFile(state, pLine + 13);
        }
        else if (strncasecmp(pLine, "Require", 7) == 0 && isspace(pLine[7]))
        {
            handleRequire(state, pLine + 7);
        }
        // Access control
        else if (strncasecmp(pLine, "Order", 5) == 0 && isspace(pLine[5]))
        {
            handleOrder(state, pTargetCtx, pLine + 5);
        }
        else if (strncasecmp(pLine, "Allow", 5) == 0 && isspace(pLine[5]))
        {
            handleAllow(pTargetCtx, pLine + 5);
        }
        else if (strncasecmp(pLine, "Deny", 4) == 0 && isspace(pLine[4]))
        {
            handleDeny(pTargetCtx, pLine + 4);
        }
        else if (strncasecmp(pLine, "Satisfy", 7) == 0 && isspace(pLine[7]))
        {
            handleSatisfy(pTargetCtx, pLine + 7);
        }
        // Error documents, directory index, options
        else if (strncasecmp(pLine, "ErrorDocument", 13) == 0
                 && isspace(pLine[13]))
        {
            handleErrorDocument(pTargetCtx, pLine + 13);
        }
        else if (strncasecmp(pLine, "DirectoryIndex", 14) == 0
                 && isspace(pLine[14]))
        {
            handleDirectoryIndex(pTargetCtx, pLine + 14);
        }
        else if (strncasecmp(pLine, "Options", 7) == 0
                 && (isspace(pLine[7]) || lineLen == 7))
        {
            handleOptions(pTargetCtx, pLine + 7);
        }
        // MIME types
        else if (strncasecmp(pLine, "AddType", 7) == 0 && isspace(pLine[7]))
        {
            handleAddType(pTargetCtx, pLine + 7);
        }
        else if (strncasecmp(pLine, "ForceType", 9) == 0
                 && isspace(pLine[9]))
        {
            handleForceType(pTargetCtx, pLine + 9, state.pLogId);
        }
        else if (strncasecmp(pLine, "DefaultType", 11) == 0
                 && isspace(pLine[11]))
        {
            handleDefaultType(pTargetCtx, pLine + 11);
        }
        else if (strncasecmp(pLine, "AddHandler", 10) == 0
                 && isspace(pLine[10]))
        {
            handleAddHandler(pTargetCtx, pLine + 10);
        }
        // PHP config
        else if (strncasecmp(pLine, "php_value", 9) == 0
                 && isspace(pLine[9]))
        {
            handlePhpValue(pTargetCtx, PHP_VALUE, pLine + 9);
        }
        else if (strncasecmp(pLine, "php_flag", 8) == 0
                 && isspace(pLine[8]))
        {
            handlePhpValue(pTargetCtx, PHP_FLAG, pLine + 8);
        }
        else if (strncasecmp(pLine, "php_admin_value", 15) == 0
                 || strncasecmp(pLine, "php_admin_flag", 14) == 0)
        {
            LS_WARN("[.htaccess] %.*s rejected — admin PHP directives not "
                    "allowed in .htaccess", 15, pLine);
        }
        // Header operations
        else if (strncasecmp(pLine, "Header ", 7) == 0)
        {
            handleHeader(pTargetCtx, pLine, lineLen, state.pLogId);
        }
        else if (strncasecmp(pLine, "RequestHeader ", 14) == 0)
        {
            handleHeader(pTargetCtx, pLine, lineLen, state.pLogId);
        }
        // Expires
        else if (strncasecmp(pLine, "ExpiresActive", 13) == 0
                 && isspace(pLine[13]))
        {
            handleExpiresActive(pTargetCtx, pLine + 13);
        }
        else if (strncasecmp(pLine, "ExpiresDefault", 14) == 0
                 && isspace(pLine[14]))
        {
            handleExpiresDefault(pTargetCtx, pLine + 14);
        }
        else if (strncasecmp(pLine, "ExpiresByType", 13) == 0
                 && isspace(pLine[13]))
        {
            handleExpiresByType(pTargetCtx, pLine + 13);
        }
        // Compression
        else if (strncasecmp(pLine, "SetOutputFilter", 15) == 0
                 && isspace(pLine[15]))
        {
            handleSetOutputFilter(pTargetCtx, pLine + 15);
        }
        else if (strncasecmp(pLine, "AddOutputFilterByType", 21) == 0
                 && isspace(pLine[21]))
        {
            handleAddOutputFilterByType(pTargetCtx, pLine + 21);
        }
        // Redirect directives
        else if (strncasecmp(pLine, "RedirectMatch", 13) == 0
                 && isspace(pLine[13]))
        {
            handleRedirectMatch(state, pLine + 13);
        }
        else if (strncasecmp(pLine, "RedirectPermanent", 17) == 0
                 && isspace(pLine[17]))
        {
            handleRedirect(state, pLine + 17, 301);
        }
        else if (strncasecmp(pLine, "RedirectTemp", 12) == 0
                 && isspace(pLine[12]))
        {
            handleRedirect(state, pLine + 12, 302);
        }
        else if (strncasecmp(pLine, "Redirect", 8) == 0
                 && isspace(pLine[8]))
        {
            handleRedirect(state, pLine + 8, 0);
        }
        // SetEnvIfNoCase
        else if (strncasecmp(pLine, "SetEnvIfNoCase", 14) == 0
                 && isspace(pLine[14]))
        {
            handleSetEnvIf(pTargetCtx, pLine + 14, 1);
        }
        // SetEnvIf
        else if (strncasecmp(pLine, "SetEnvIf", 8) == 0
                 && isspace(pLine[8]))
        {
            handleSetEnvIf(pTargetCtx, pLine + 8, 0);
        }
        // SetHandler
        else if (strncasecmp(pLine, "SetHandler", 10) == 0
                 && isspace(pLine[10]))
        {
            handleSetHandler(pTargetCtx, pLine + 10, state.pLogId);
        }
        // SetEnv
        else if (strncasecmp(pLine, "SetEnv", 6) == 0
                 && isspace(pLine[6]))
        {
            handleSetEnv(pTargetCtx, pLine + 6);
        }
        // UnsetEnv
        else if (strncasecmp(pLine, "UnsetEnv", 8) == 0
                 && isspace(pLine[8]))
        {
            handleUnsetEnv(pTargetCtx, pLine + 8);
        }
        // FallbackResource
        else if (strncasecmp(pLine, "FallbackResource", 16) == 0
                 && isspace(pLine[16]))
        {
            handleFallbackResource(state, pLine + 16);
        }
        // FileETag
        else if (strncasecmp(pLine, "FileETag", 8) == 0
                 && isspace(pLine[8]))
        {
            handleFileETag(pTargetCtx, pLine + 8);
        }
        // SSLRequireSSL
        else if (strncasecmp(pLine, "SSLRequireSSL", 13) == 0)
        {
            handleSSLRequireSSL(pTargetCtx);
        }
        // AddDefaultCharset
        else if (strncasecmp(pLine, "AddDefaultCharset", 17) == 0
                 && isspace(pLine[17]))
        {
            const char *pArgs = skipWhitespace(pLine + 17);
            if (strncasecmp(pArgs, "off", 3) == 0)
                ; // No-op: default is off
            else if (strncasecmp(pArgs, "on", 2) == 0)
                pTargetCtx->setDefaultCharsetOn();
            else
                pTargetCtx->setDefaultCharset(pArgs);
        }
        // ServerSignature — ignore
        else if (strncasecmp(pLine, "ServerSignature", 15) == 0)
        {
            // Ignore server-level directive
        }
        // <If>, <ElseIf>, <Else> — not supported
        else if (strncasecmp(pLine, "<If ", 4) == 0
                 || strncasecmp(pLine, "<ElseIf ", 8) == 0
                 || strncasecmp(pLine, "<Else>", 6) == 0
                 || strncasecmp(pLine, "</If>", 5) == 0
                 || strncasecmp(pLine, "</ElseIf>", 9) == 0
                 || strncasecmp(pLine, "</Else>", 7) == 0)
        {
            LS_INFO("[.htaccess] Conditional blocks (<If>/<ElseIf>/<Else>) "
                    "not supported, ignored: %s", pLine);
        }
        // <Directory> — not valid in .htaccess, ignore
        else if (strncasecmp(pLine, "<Directory", 10) == 0
                 || strncasecmp(pLine, "</Directory>", 12) == 0)
        {
            LS_INFO("[.htaccess] <Directory> not valid in .htaccess, ignored");
        }
        // <Location> — not valid in .htaccess, ignore
        else if (strncasecmp(pLine, "<Location", 9) == 0
                 || strncasecmp(pLine, "</Location>", 11) == 0)
        {
            LS_INFO("[.htaccess] <Location> not valid in .htaccess, ignored");
        }
        else
        {
            LS_DBG("[.htaccess] Unrecognized directive: %s", pLine);
        }

        *pLineEnd = saved;
        pLine = pLineEnd;
        if (pLine < pEnd)
            ++pLine;
    }

    // Finalize: apply collected auth settings
    finalizeAuth(state, pContext);

    // Finalize: process collected rewrite rules
    finalizeRewrite(state, pContext, pMapList);

    // Clean up orphaned FilesMatch context
    if (state.pFilesCtx)
    {
        LS_WARN("[.htaccess] Unclosed <Files>/<FilesMatch> block");
        delete state.pFilesCtx;
        state.pFilesCtx = NULL;
    }

    delete[] buf;
    return 0;
}
