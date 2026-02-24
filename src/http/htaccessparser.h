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
#ifndef HTACCESSPARSER_H
#define HTACCESSPARSER_H

#include <util/autostr.h>

class HttpContext;
class RewriteMapList;

#define HTA_ALLOW_AUTH          (1<<0)
#define HTA_ALLOW_ACCESS        (1<<1)
#define HTA_ALLOW_ERRORDOC      (1<<2)
#define HTA_ALLOW_DIRINDEX      (1<<3)
#define HTA_ALLOW_OPTIONS       (1<<4)
#define HTA_ALLOW_MIME          (1<<5)
#define HTA_ALLOW_PHP           (1<<6)
#define HTA_ALLOW_HEADERS       (1<<7)
#define HTA_ALLOW_EXPIRES       (1<<8)
#define HTA_ALLOW_REWRITE       (1<<9)
#define HTA_ALLOW_REDIRECT      (1<<10)
#define HTA_ALLOW_FILESMATCH    (1<<11)
#define HTA_ALLOW_SETENVIF      (1<<12)
#define HTA_ALLOW_SSL           (1<<13)
#define HTA_ALLOW_ALL           0xFFFFFFFF

struct HtParseState
{
    AutoStr2    rewriteBuf;
    AutoStr2    authType;
    AutoStr2    authName;
    AutoStr2    authUserFile;
    AutoStr2    authGroupFile;
    AutoStr2    requireLine;
    int         ifModuleSkip;
    int         ifModuleNesting;
    HttpContext *pFilesCtx;
    uint32_t    allowOverride;
    const char *pLogId;

    HtParseState()
        : ifModuleSkip(0)
        , ifModuleNesting(0)
        , pFilesCtx(NULL)
        , allowOverride(HTA_ALLOW_ALL)
        , pLogId(NULL)
    {}
};


class HtAccessParser
{
    HtAccessParser();
    ~HtAccessParser();
    HtAccessParser(const HtAccessParser &rhs);
    void operator=(const HtAccessParser &rhs);

    static int isKnownModule(const char *pModule, int len);

    // Directive handlers
    static int handleAuthType(HtParseState &state, const char *pArgs);
    static int handleAuthName(HtParseState &state, const char *pArgs);
    static int handleAuthUserFile(HtParseState &state, const char *pArgs,
                                  HttpContext *pContext);
    static int handleAuthGroupFile(HtParseState &state, const char *pArgs);
    static int handleRequire(HtParseState &state, const char *pArgs);

    static int handleOrder(HtParseState &state, HttpContext *pContext,
                           const char *pArgs);
    static int handleAllow(HttpContext *pContext, const char *pArgs);
    static int handleDeny(HttpContext *pContext, const char *pArgs);
    static int handleSatisfy(HttpContext *pContext, const char *pArgs);

    static int handleErrorDocument(HttpContext *pContext, const char *pArgs);
    static int handleDirectoryIndex(HttpContext *pContext, const char *pArgs);
    static int handleOptions(HttpContext *pContext, const char *pArgs);

    static int handleAddType(HttpContext *pContext, const char *pArgs);
    static int handleForceType(HttpContext *pContext, const char *pArgs,
                               const char *pLogId);
    static int handleDefaultType(HttpContext *pContext, const char *pArgs);
    static int handleAddHandler(HttpContext *pContext, const char *pArgs);

    static int handlePhpValue(HttpContext *pContext, int id,
                              const char *pArgs);

    static int handleHeader(HttpContext *pContext, const char *pLine,
                            int lineLen, const char *pLogId);
    static int handleExpiresActive(HttpContext *pContext, const char *pArgs);
    static int handleExpiresDefault(HttpContext *pContext, const char *pArgs);
    static int handleExpiresByType(HttpContext *pContext, const char *pArgs);
    static int handleSetOutputFilter(HttpContext *pContext, const char *pArgs);
    static int handleAddOutputFilterByType(HttpContext *pContext,
                                           const char *pArgs);

    static int handleFilesBlock(HtParseState &state, HttpContext *pContext,
                                const char *pLine, int isMatch);
    static int handleEndFilesBlock(HtParseState &state, HttpContext *pContext);

    static int handleIfModule(HtParseState &state, const char *pArgs);
    static int handleEndIfModule(HtParseState &state);

    static int handleRedirect(HtParseState &state, const char *pArgs,
                              int status);
    static int handleRedirectMatch(HtParseState &state, const char *pArgs);

    static int handleSSLRequireSSL(HttpContext *pContext);

    static int handleSetEnv(HttpContext *pContext, const char *pArgs);
    static int handleUnsetEnv(HttpContext *pContext, const char *pArgs);
    static int handleSetEnvIf(HttpContext *pContext, const char *pArgs,
                              int noCase);
    static int handleSetHandler(HttpContext *pContext, const char *pArgs,
                                const char *pLogId);
    static int handleFallbackResource(HtParseState &state, const char *pArgs);
    static int handleFileETag(HttpContext *pContext, const char *pArgs);

    static int finalizeAuth(HtParseState &state, HttpContext *pContext);
    static int finalizeRewrite(HtParseState &state, HttpContext *pContext,
                               const RewriteMapList *pMapList);

public:
    static int parse(const char *pBuf, int len, HttpContext *pContext,
                     const RewriteMapList *pMapList);
};


#endif
