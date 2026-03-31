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
 * mod_esi -- Edge Side Includes module for OpenLiteSpeed.
 *
 * This module intercepts response bodies that contain ESI markup and
 * processes them inline. It hooks into the RECV_RESP_BODY and
 * SEND_RESP_BODY filter chains.
 *
 * Activation: the module is activated when the response contains the
 * header "X-LiteSpeed-Cache-Control: esi=on".
 *
 * Processing flow:
 *   1. On RCVD_RESP_HEADER, check for the ESI activation header.
 *      If present, enable the RECV_RESP_BODY and SEND_RESP_BODY hooks
 *      for this session.
 *   2. In RECV_RESP_BODY, buffer the entire response body and parse
 *      it for ESI tags using EsiParser.
 *   3. In RCVD_RESP_BODY, if ESI content was found, perform include
 *      fetching and assemble the final response.
 *   4. Write the assembled response through the filter chain.
 *
 * ESI include fragments are fetched via internal HTTP subrequests
 * using the exec_ext_cmd facility, or by inlining the fetch through
 * a simple synchronous local socket connection to the server itself.
 * For production deployments, includes are limited to same-origin
 * local URIs.
 */

#include "esiparser.h"

#include <ls.h>
#include <lsr/ls_xpool.h>
#include <lsr/ls_loopbuf.h>

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <new>


#define MNAME                       mod_esi
#define MODULE_NAME_STR             "ESI"
#define MODULE_VERSION_INFO         "1.0"

/**
 * Maximum size of a full response body that we will buffer for ESI
 * processing. Responses larger than this bypass ESI entirely.
 */
#define ESI_MAX_BODY_SIZE           (8 * 1024 * 1024)

/**
 * Maximum size of an ESI include fragment response.
 */
#define ESI_MAX_FRAGMENT_SIZE       (2 * 1024 * 1024)

/**
 * Maximum recursion depth for nested ESI includes.
 */
#define ESI_MAX_DEPTH               5

/**
 * Timeout in seconds for ESI fragment fetch.
 */
#define ESI_FETCH_TIMEOUT_SEC       5

/**
 * Response header that activates ESI processing.
 */
static const char s_esiHeader[]     = "X-LiteSpeed-Cache-Control";
static const int  s_esiHeaderLen    = 25;
static const char s_esiValue[]      = "esi=on";
static const int  s_esiValueLen     = 6;

/**
 * Surrogate-Control header (W3C ESI spec).
 */
static const char s_surrogateCtl[]     = "Surrogate-Control";
static const int  s_surrogateCtlLen    = 17;
static const char s_surrogateEsi[]     = "ESI/1.0";
static const int  s_surrogateEsiLen    = 7;


extern lsi_module_t MNAME;


/**
 * Per-session module data.
 */
struct EsiModData
{
    EsiParser       parser;

    /**
     * Accumulated response body chunks. Using a simple linked list
     * of buffers for streaming accumulation.
     */
    char           *bodyBuf;
    int             bodyLen;
    int             bodyCap;

    /**
     * Assembled output after ESI processing.
     */
    char           *outputBuf;
    int             outputLen;
    int             outputCap;

    /**
     * Flag: has ESI processing been activated for this session?
     */
    int             esiActive;

    /**
     * Flag: has the body been fully received?
     */
    int             bodyComplete;

    /**
     * Flag: has ESI assembly been done?
     */
    int             assembled;

    /**
     * Depth counter for recursive ESI processing.
     */
    int             depth;
};


/**
 * Ensure the body buffer has room for at least 'needed' more bytes.
 */
static int ensureBodyCap(EsiModData *pData, int needed)
{
    if (needed <= 0)
        return 0;
    if (needed > ESI_MAX_BODY_SIZE - pData->bodyLen)
        return -1;
    int required = pData->bodyLen + needed;
    if (required <= pData->bodyCap)
        return 0;

    int newCap = pData->bodyCap;
    if (newCap <= 0)
        newCap = 4096;
    while (newCap < required && newCap <= ESI_MAX_BODY_SIZE / 2)
        newCap *= 2;
    if (newCap < required)
        newCap = required;
    if (newCap > ESI_MAX_BODY_SIZE)
        newCap = ESI_MAX_BODY_SIZE;

    char *newBuf = (char *)realloc(pData->bodyBuf, newCap);
    if (!newBuf)
        return -1;
    pData->bodyBuf = newBuf;
    pData->bodyCap = newCap;
    return 0;
}


/**
 * Ensure the output buffer has room for at least 'needed' more bytes.
 */
static int ensureOutputCap(EsiModData *pData, int needed)
{
    if (needed <= 0)
        return 0;
    if (needed > ESI_MAX_BODY_SIZE - pData->outputLen)
        return -1;
    int required = pData->outputLen + needed;
    if (required <= pData->outputCap)
        return 0;

    int newCap = pData->outputCap;
    if (newCap <= 0)
        newCap = 4096;
    while (newCap < required && newCap <= ESI_MAX_BODY_SIZE / 2)
        newCap *= 2;
    if (newCap < required)
        newCap = required;

    char *newBuf = (char *)realloc(pData->outputBuf, newCap);
    if (!newBuf)
        return -1;
    pData->outputBuf = newBuf;
    pData->outputCap = newCap;
    return 0;
}


/**
 * Append data to the output buffer.
 */
static int appendOutput(EsiModData *pData, const char *buf, int len)
{
    if (len <= 0)
        return 0;
    if (ensureOutputCap(pData, len) < 0)
        return -1;
    memcpy(pData->outputBuf + pData->outputLen, buf, len);
    pData->outputLen += len;
    return 0;
}


/**
 * Fetch an ESI include fragment via a local loopback HTTP request.
 *
 * This connects to the server's own listener on 127.0.0.1 to fetch
 * the fragment. The session's Host header is forwarded so the
 * virtual host routing works correctly.
 *
 * @param session   Current LSI session (for reading Host header, server port).
 * @param uri       The URI to fetch (e.g., "/header.html").
 * @param uriLen    Length of URI.
 * @param outBuf    [out] Allocated buffer with response body (caller must free).
 * @param outLen    [out] Length of response body.
 * @return 0 on success, -1 on failure.
 */
static int fetchFragment(const lsi_session_t *session,
                         const char *uri, int uriLen,
                         char **outBuf, int *outLen)
{
    *outBuf = NULL;
    *outLen = 0;

    // Get server port
    char portBuf[16];
    int portLen = 0;
    portLen = g_api->get_req_var_by_id(session, LSI_VAR_SERVER_PORT,
                                       portBuf, sizeof(portBuf) - 1);
    if (portLen <= 0)
        return -1;
    portBuf[portLen] = '\0';
    int port = atoi(portBuf);
    if (port <= 0 || port > 65535)
        port = 80;

    // Get Host header
    int hostLen = 0;
    const char *hostVal = g_api->get_req_header_by_id(session, LSI_HDR_HOST,
                                                       &hostLen);
    char hostBuf[512];
    if (hostVal && hostLen > 0 && hostLen < (int)sizeof(hostBuf) - 1)
    {
        memcpy(hostBuf, hostVal, hostLen);
        hostBuf[hostLen] = '\0';
    }
    else
    {
        snprintf(hostBuf, sizeof(hostBuf), "localhost:%d", port);
        hostLen = (int)strlen(hostBuf);
    }

    // Build HTTP/1.1 request
    char reqBuf[ESI_MAX_URL_LEN + 1024];
    int reqLen = snprintf(reqBuf, sizeof(reqBuf),
                          "GET %.*s HTTP/1.1\r\n"
                          "Host: %s\r\n"
                          "Connection: close\r\n"
                          "X-ESI-Fragment: 1\r\n"
                          "\r\n",
                          uriLen, uri, hostBuf);
    if (reqLen <= 0 || reqLen >= (int)sizeof(reqBuf))
        return -1;

    // Create socket and connect to 127.0.0.1:port
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    // Set socket timeout
    struct timeval tv;
    tv.tv_sec  = ESI_FETCH_TIMEOUT_SEC;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }

    // Send request
    int totalSent = 0;
    while (totalSent < reqLen)
    {
        int n = send(fd, reqBuf + totalSent, reqLen - totalSent, 0);
        if (n <= 0)
        {
            close(fd);
            return -1;
        }
        totalSent += n;
    }

    // Read response
    int respCap  = 16384;
    char *respBuf = (char *)malloc(respCap);
    if (!respBuf)
    {
        close(fd);
        return -1;
    }
    int respLen = 0;

    for (;;)
    {
        if (respLen >= ESI_MAX_FRAGMENT_SIZE)
            break;
        if (respLen + 4096 > respCap)
        {
            int newCap = respCap * 2;
            if (newCap > ESI_MAX_FRAGMENT_SIZE + 4096)
                newCap = ESI_MAX_FRAGMENT_SIZE + 4096;
            char *tmp = (char *)realloc(respBuf, newCap);
            if (!tmp)
                break;
            respBuf = tmp;
            respCap = newCap;
        }
        int n = recv(fd, respBuf + respLen, respCap - respLen, 0);
        if (n <= 0)
            break;
        respLen += n;
    }
    close(fd);

    if (respLen <= 0)
    {
        free(respBuf);
        return -1;
    }

    // Parse HTTP response: find end of headers (double CRLF)
    const char *headerEnd = NULL;
    for (int i = 0; i < respLen - 3; ++i)
    {
        if (respBuf[i] == '\r' && respBuf[i + 1] == '\n'
            && respBuf[i + 2] == '\r' && respBuf[i + 3] == '\n')
        {
            headerEnd = respBuf + i + 4;
            break;
        }
    }

    if (!headerEnd)
    {
        free(respBuf);
        return -1;
    }

    // Check for 2xx status code
    // Format: "HTTP/1.1 200 OK\r\n..."
    if (respLen < 12 || (respBuf[9] != '2'))
    {
        free(respBuf);
        return -1;
    }

    int bodyOffset = (int)(headerEnd - respBuf);
    int bodySize   = respLen - bodyOffset;

    if (bodySize <= 0)
    {
        // Empty body is valid (200 with no content)
        *outBuf = NULL;
        *outLen = 0;
        free(respBuf);
        return 0;
    }

    // Move body to start of buffer (or allocate new)
    char *body = (char *)malloc(bodySize);
    if (!body)
    {
        free(respBuf);
        return -1;
    }
    memcpy(body, headerEnd, bodySize);
    free(respBuf);

    *outBuf = body;
    *outLen = bodySize;
    return 0;
}


/**
 * Assemble the final output by walking the parsed ESI nodes.
 *
 * TEXT nodes are copied directly. INCLUDE nodes trigger fragment
 * fetches. REMOVE and COMMENT nodes are suppressed.
 * TRY/ATTEMPT/EXCEPT provides error handling for includes.
 */
static int assembleOutput(const lsi_session_t *session, EsiModData *pData)
{
    const EsiNode *nodes = pData->parser.getNodes();
    int count            = pData->parser.getNodeCount();

    if (count <= 0)
        return 0;

    // Enforce recursion depth to prevent infinite self-includes
    if (pData->depth >= ESI_MAX_DEPTH)
    {
        g_api->log(session, LSI_LOG_WARN,
                   "[%s] ESI max recursion depth (%d) exceeded, aborting.\n",
                   MODULE_NAME_STR, ESI_MAX_DEPTH);
        return -1;
    }
    pData->depth++;

    // Stack-based try/attempt/except nesting support (max 8 levels deep)
    #define ESI_TRY_STACK_MAX   8
    struct TryState {
        int attemptFailed;
        int inAttempt;
        int inExcept;
        int skipAttempt;
        int skipExcept;
    };
    TryState tryStack[ESI_TRY_STACK_MAX];
    int tryDepth = 0;

    // Current active state (top of stack or default)
    int attemptFailed = 0;
    int inAttempt     = 0;
    int inExcept      = 0;
    int skipAttempt   = 0;
    int skipExcept    = 0;

    for (int i = 0; i < count; ++i)
    {
        const EsiNode *node = &nodes[i];

        // Check if current node should be skipped
        int skip = (inExcept && skipExcept) || (inAttempt && skipAttempt);

        switch (node->type)
        {
        case ESI_NODE_TEXT:
            if (!skip)
                appendOutput(pData, node->data, node->dataLen);
            break;

        case ESI_NODE_INCLUDE:
        {
            if (skip)
                break;

            char *fragBuf  = NULL;
            int   fragLen  = 0;
            int   success  = 0;

            // Try primary src
            if (node->data && node->dataLen > 0)
            {
                if (fetchFragment(session, node->data, node->dataLen,
                                  &fragBuf, &fragLen) == 0)
                    success = 1;
            }

            // If primary failed and alt is available, try alt
            if (!success && node->altUrl && node->altUrlLen > 0)
            {
                if (fetchFragment(session, node->altUrl, node->altUrlLen,
                                  &fragBuf, &fragLen) == 0)
                    success = 1;
            }

            if (success && fragBuf && fragLen > 0)
            {
                appendOutput(pData, fragBuf, fragLen);
                free(fragBuf);
            }
            else
            {
                if (fragBuf)
                    free(fragBuf);

                if (inAttempt)
                {
                    attemptFailed = 1;
                    skipAttempt   = 1;
                }
                else if (!node->onerrorContinue)
                {
                    g_api->log(session, LSI_LOG_WARN,
                               "[%s] ESI include failed for '%.*s'\n",
                               MODULE_NAME_STR,
                               node->dataLen, node->data ? node->data : "");
                }
            }
            break;
        }

        case ESI_NODE_REMOVE:
        case ESI_NODE_COMMENT:
            break;

        case ESI_NODE_TRY:
            // Push current state onto stack
            if (tryDepth < ESI_TRY_STACK_MAX)
            {
                tryStack[tryDepth].attemptFailed = attemptFailed;
                tryStack[tryDepth].inAttempt     = inAttempt;
                tryStack[tryDepth].inExcept      = inExcept;
                tryStack[tryDepth].skipAttempt   = skipAttempt;
                tryStack[tryDepth].skipExcept    = skipExcept;
                tryDepth++;
            }
            attemptFailed = 0;
            skipAttempt   = 0;
            skipExcept    = 0;
            inAttempt     = 0;
            inExcept      = 0;
            break;

        case ESI_NODE_ATTEMPT:
            inAttempt   = 1;
            inExcept    = 0;
            skipAttempt = 0;
            break;

        case ESI_NODE_EXCEPT:
            inAttempt = 0;
            inExcept  = 1;
            skipExcept = !attemptFailed;
            break;
        }

        // Detect end of TRY block: if this is the last node of an EXCEPT
        // and the next node is not ATTEMPT or EXCEPT at the same level,
        // pop the try stack. We detect TRY end by checking if the next
        // node is a new TRY or we leave the current try scope.
        if (node->type == ESI_NODE_EXCEPT && i + 1 < count)
        {
            const EsiNode *next = &nodes[i + 1];
            if (next->type != ESI_NODE_TEXT
                || (next->type == ESI_NODE_TRY
                    || next->type == ESI_NODE_INCLUDE
                    || next->type == ESI_NODE_REMOVE))
            {
                // Check if next node's parent differs (end of TRY scope)
                if (next->parentIndex != node->parentIndex && tryDepth > 0)
                {
                    tryDepth--;
                    attemptFailed = tryStack[tryDepth].attemptFailed;
                    inAttempt     = tryStack[tryDepth].inAttempt;
                    inExcept      = tryStack[tryDepth].inExcept;
                    skipAttempt   = tryStack[tryDepth].skipAttempt;
                    skipExcept    = tryStack[tryDepth].skipExcept;
                }
            }
        }
    }

    pData->assembled = 1;
    return 0;
}


/**
 * Release module data callback.
 */
static int esiDataRelease(void *data)
{
    EsiModData *pData = (EsiModData *)data;
    if (pData)
    {
        if (pData->bodyBuf)
        {
            free(pData->bodyBuf);
            pData->bodyBuf = NULL;
        }
        if (pData->outputBuf)
        {
            free(pData->outputBuf);
            pData->outputBuf = NULL;
        }
        // pData itself is allocated from session pool, no need to free
    }
    return 0;
}


/**
 * Allocate and initialize per-session module data.
 */
static EsiModData *getModData(const lsi_session_t *session)
{
    EsiModData *pData = (EsiModData *)g_api->get_module_data(
                             session, &MNAME, LSI_DATA_HTTP);
    if (pData)
        return pData;

    ls_xpool_t *pool = g_api->get_session_pool(session);
    pData = (EsiModData *)ls_xpool_alloc(pool, sizeof(EsiModData));
    if (!pData)
        return NULL;

    // Zero-initialize POD fields, then placement-new the parser
    pData->bodyBuf      = NULL;
    pData->bodyLen      = 0;
    pData->bodyCap      = 0;
    pData->outputBuf    = NULL;
    pData->outputLen    = 0;
    pData->outputCap    = 0;
    pData->esiActive    = 0;
    pData->bodyComplete = 0;
    pData->assembled    = 0;
    pData->depth        = 0;
    new (&pData->parser) EsiParser();

    g_api->set_module_data(session, &MNAME, LSI_DATA_HTTP, pData);
    return pData;
}


/**
 * Check if the response contains the ESI activation header.
 *
 * We check for:
 *   X-LiteSpeed-Cache-Control: esi=on
 * or:
 *   Surrogate-Control: content="ESI/1.0"
 */
static int checkEsiHeader(const lsi_session_t *session)
{
    struct iovec iov[4];
    int count;

    // Check X-LiteSpeed-Cache-Control header
    count = g_api->get_resp_header(session,
                                   LSI_RSPHDR_LITESPEED_CACHE_CONTROL,
                                   NULL, 0, iov, 4);
    for (int i = 0; i < count; ++i)
    {
        const char *val = (const char *)iov[i].iov_base;
        int         len = (int)iov[i].iov_len;
        // Look for "esi=on" substring (case-insensitive)
        if (len >= s_esiValueLen)
        {
            for (int j = 0; j <= len - s_esiValueLen; ++j)
            {
                if (strncasecmp(val + j, s_esiValue, s_esiValueLen) == 0)
                    return 1;
            }
        }
    }

    // Check Surrogate-Control header
    count = g_api->get_resp_header(session, LSI_RSPHDR_UNKNOWN,
                                   s_surrogateCtl, s_surrogateCtlLen,
                                   iov, 4);
    for (int i = 0; i < count; ++i)
    {
        const char *val = (const char *)iov[i].iov_base;
        int         len = (int)iov[i].iov_len;
        // Look for "ESI/1.0" substring
        if (len >= s_surrogateEsiLen)
        {
            for (int j = 0; j <= len - s_surrogateEsiLen; ++j)
            {
                if (strncasecmp(val + j, s_surrogateEsi,
                                s_surrogateEsiLen) == 0)
                    return 1;
            }
        }
    }

    return 0;
}


/**
 * Check if this request is itself an ESI fragment fetch (prevent recursion).
 */
static int isFragmentRequest(const lsi_session_t *session)
{
    int valLen = 0;
    const char *val = g_api->get_req_header(session, "X-ESI-Fragment", 14,
                                            &valLen);
    return (val && valLen > 0);
}


/**
 * Hook: RCVD_RESP_HEADER
 *
 * Called when response headers have been received from the backend.
 * We check for the ESI activation header and enable body processing
 * hooks if ESI is active.
 */
static int onRcvdRespHeader(lsi_param_t *rec)
{
    const lsi_session_t *session = rec->session;

    // Do not process ESI on fragment subrequests to avoid infinite recursion
    if (isFragmentRequest(session))
        return LSI_OK;

    if (!checkEsiHeader(session))
        return LSI_OK;

    g_api->log(session, LSI_LOG_DEBUG,
               "[%s] ESI activation header found, enabling ESI processing.\n",
               MODULE_NAME_STR);

    EsiModData *pData = getModData(session);
    if (!pData)
        return LSI_OK;

    pData->esiActive = 1;

    // Enable the RECV_RESP_BODY and SEND_RESP_BODY hooks for this session
    int aHkpts[3];
    aHkpts[0] = LSI_HKPT_RECV_RESP_BODY;
    aHkpts[1] = LSI_HKPT_SEND_RESP_BODY;
    aHkpts[2] = LSI_HKPT_RCVD_RESP_BODY;
    g_api->enable_hook(session, &MNAME,
                       LSI_FLAG_TRANSFORM | LSI_FLAG_DECOMPRESS_REQUIRED,
                       aHkpts, 3);

    // Remove Content-Length since ESI processing changes body size.
    // The response will use chunked encoding.
    g_api->remove_resp_header(session, LSI_RSPHDR_CONTENT_LENGTH, NULL, 0);
    g_api->set_resp_content_length(session, -1);

    // Remove the ESI activation headers from the response
    // (they are internal directives, not meant for the client)
    g_api->remove_resp_header(session, LSI_RSPHDR_LITESPEED_CACHE_CONTROL,
                              NULL, 0);
    g_api->remove_resp_header(session, LSI_RSPHDR_UNKNOWN,
                              s_surrogateCtl, s_surrogateCtlLen);

    // Add an indicator header for debugging
    g_api->set_resp_header(session, LSI_RSPHDR_UNKNOWN,
                           "X-ESI-Processed", 15,
                           "true", 4, LSI_HEADEROP_SET);

    return LSI_OK;
}


/**
 * Hook: RECV_RESP_BODY (filter)
 *
 * Called as response body data arrives from the backend.
 * We buffer all body data for ESI parsing. The data is consumed
 * (not passed to the next filter) so we can reassemble it after
 * ESI processing.
 */
static int onRecvRespBody(lsi_param_t *rec)
{
    const lsi_session_t *session = rec->session;
    EsiModData *pData = (EsiModData *)g_api->get_module_data(
                             session, &MNAME, LSI_DATA_HTTP);

    if (!pData || !pData->esiActive)
        return g_api->stream_write_next(rec, (const char *)rec->ptr1,
                                        rec->len1);

    const char *buf = (const char *)rec->ptr1;
    int         len = rec->len1;

    if (len > 0)
    {
        if (ensureBodyCap(pData, len) < 0)
        {
            g_api->log(session, LSI_LOG_ERROR,
                       "[%s] Body buffer overflow, disabling ESI.\n",
                       MODULE_NAME_STR);
            pData->esiActive = 0;
            return g_api->stream_write_next(rec, buf, len);
        }
        memcpy(pData->bodyBuf + pData->bodyLen, buf, len);
        pData->bodyLen += len;
    }

    if (rec->flag_in & LSI_CBFI_EOF)
    {
        pData->bodyComplete = 1;

        // Parse the complete body for ESI tags
        pData->parser.feed(pData->bodyBuf, pData->bodyLen);
        pData->parser.finish();

        if (!pData->parser.hasEsiContent())
        {
            // No ESI tags found -- pass through original body
            g_api->log(session, LSI_LOG_DEBUG,
                       "[%s] No ESI tags found in response body (%d bytes).\n",
                       MODULE_NAME_STR, pData->bodyLen);

            // Write original body to next filter with EOF
            int written = g_api->stream_write_next(rec, pData->bodyBuf,
                                                   pData->bodyLen);
            // Now send EOF
            rec->flag_in = LSI_CBFI_EOF;
            g_api->stream_write_next(rec, NULL, 0);
            return written;
        }

        // ESI content found -- assemble the output
        g_api->log(session, LSI_LOG_DEBUG,
                   "[%s] ESI tags found (%d nodes), assembling output.\n",
                   MODULE_NAME_STR, pData->parser.getNodeCount());

        assembleOutput(session, pData);

        // Write assembled output
        if (pData->outputLen > 0)
        {
            g_api->stream_write_next(rec, pData->outputBuf,
                                     pData->outputLen);
        }

        // Send EOF
        rec->flag_in = LSI_CBFI_EOF;
        g_api->stream_write_next(rec, NULL, 0);
        return pData->outputLen;
    }

    // Not EOF yet -- buffer the data, signal that we consumed it
    if (rec->flag_out)
        *rec->flag_out |= LSI_CBFO_BUFFERED;
    return len;
}


/**
 * Hook: SEND_RESP_BODY (filter)
 *
 * In most cases the assembled output has already been written in
 * onRecvRespBody at EOF. This hook is a passthrough for any data
 * that flows through the send chain.
 */
static int onSendRespBody(lsi_param_t *rec)
{
    // Pass data through unchanged to the next filter in the chain.
    return g_api->stream_write_next(rec, (const char *)rec->ptr1,
                                    rec->len1);
}


/**
 * Hook: RCVD_RESP_BODY
 *
 * Called when the complete response body has been received.
 * At this point the RECV_RESP_BODY filter has already processed
 * everything. This hook is primarily for cleanup logging.
 */
static int onRcvdRespBody(lsi_param_t *rec)
{
    const lsi_session_t *session = rec->session;
    EsiModData *pData = (EsiModData *)g_api->get_module_data(
                             session, &MNAME, LSI_DATA_HTTP);

    if (pData && pData->esiActive)
    {
        g_api->log(session, LSI_LOG_DEBUG,
                   "[%s] ESI processing complete. Input: %d bytes, "
                   "Output: %d bytes, Nodes: %d\n",
                   MODULE_NAME_STR,
                   pData->bodyLen,
                   pData->outputLen,
                   pData->parser.getNodeCount());
    }

    return LSI_OK;
}


/**
 * Hook: HTTP_END
 *
 * Session cleanup -- free allocated buffers.
 */
static int onHttpEnd(lsi_param_t *rec)
{
    g_api->free_module_data(rec->session, &MNAME, LSI_DATA_HTTP,
                            esiDataRelease);
    return LSI_OK;
}


/**
 * Hook: HANDLER_RESTART
 *
 * If the handler is restarted (e.g., internal redirect or error),
 * clean up ESI state.
 */
static int onHandlerRestart(lsi_param_t *rec)
{
    g_api->free_module_data(rec->session, &MNAME, LSI_DATA_HTTP,
                            esiDataRelease);
    return LSI_OK;
}


/**
 * Module initialization function.
 */
static int esiModuleInit(lsi_module_t *pModule)
{
    g_api->init_module_data(pModule, esiDataRelease, LSI_DATA_HTTP);

    g_api->log(NULL, LSI_LOG_NOTICE,
               "[%s] ESI module v%s initialized.\n",
               MODULE_NAME_STR, MODULE_VERSION_INFO);
    return LSI_OK;
}


/**
 * Server hook definitions.
 *
 * We register globally but with hooks initially disabled (flag=0).
 * The RCVD_RESP_HEADER hook is always enabled to check for the ESI
 * activation header. The body processing hooks are enabled per-session
 * only when ESI is detected.
 */
static lsi_serverhook_t esiHooks[] =
{
    // Always enabled: check response headers for ESI activation
    {
        LSI_HKPT_RCVD_RESP_HEADER,
        onRcvdRespHeader,
        LSI_HOOK_NORMAL,
        LSI_FLAG_ENABLED
    },

    // Enabled per-session: buffer and parse response body
    {
        LSI_HKPT_RECV_RESP_BODY,
        onRecvRespBody,
        LSI_HOOK_EARLY,
        LSI_FLAG_TRANSFORM | LSI_FLAG_DECOMPRESS_REQUIRED
    },

    // Enabled per-session: send assembled body
    {
        LSI_HKPT_SEND_RESP_BODY,
        onSendRespBody,
        LSI_HOOK_LATE,
        LSI_FLAG_TRANSFORM | LSI_FLAG_DECOMPRESS_REQUIRED
    },

    // Enabled per-session: post-processing after full body received
    {
        LSI_HKPT_RCVD_RESP_BODY,
        onRcvdRespBody,
        LSI_HOOK_NORMAL,
        0
    },

    // Always enabled: cleanup
    {
        LSI_HKPT_HTTP_END,
        onHttpEnd,
        LSI_HOOK_NORMAL,
        LSI_FLAG_ENABLED
    },

    // Always enabled: cleanup on handler restart
    {
        LSI_HKPT_HANDLER_RESTART,
        onHandlerRestart,
        LSI_HOOK_NORMAL,
        LSI_FLAG_ENABLED
    },

    LSI_HOOK_END
};


/**
 * The LSIAPI module definition.
 *
 * The module name 'mod_esi' must match the MNAME define.
 */
LSMODULE_EXPORT lsi_module_t MNAME =
{
    LSI_MODULE_SIGNATURE,       // signature
    esiModuleInit,              // init_pf
    NULL,                       // reqhandler
    NULL,                       // config_parser
    MODULE_VERSION_INFO,        // about
    esiHooks,                   // serverhook
    { 0 }                       // reserved
};
