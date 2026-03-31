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

#include "esiparser.h"
#include <string.h>
#include <ctype.h>


/**
 * ESI tag prefixes and closing patterns used during scanning.
 */
static const char s_esiPrefix[]       = "<esi:";
static const int  s_esiPrefixLen      = 5;

static const char s_esiInclude[]      = "include";
static const int  s_esiIncludeLen     = 7;

static const char s_esiRemove[]       = "remove";
static const int  s_esiRemoveLen      = 6;

static const char s_esiRemoveClose[]  = "</esi:remove>";
static const int  s_esiRemoveCloseLen = 13;

static const char s_esiComment[]      = "comment";
static const int  s_esiCommentLen     = 7;

static const char s_esiTry[]          = "try";
static const int  s_esiTryLen         = 3;

static const char s_esiTryClose[]     = "</esi:try>";
static const int  s_esiTryCloseLen    = 10;

static const char s_esiAttempt[]      = "attempt";
static const int  s_esiAttemptLen     = 7;

static const char s_esiAttemptClose[] = "</esi:attempt>";
static const int  s_esiAttemptCloseLen = 14;

static const char s_esiExcept[]       = "except";
static const int  s_esiExceptLen      = 6;

static const char s_esiExceptClose[]  = "</esi:except>";
static const int  s_esiExceptCloseLen = 13;


/**
 * Case-insensitive comparison of n bytes.
 */
static int strncasecmpLocal(const char *a, const char *b, int n)
{
    for (int i = 0; i < n; ++i)
    {
        int ca = tolower((unsigned char)a[i]);
        int cb = tolower((unsigned char)b[i]);
        if (ca != cb)
            return ca - cb;
    }
    return 0;
}


/**
 * Find a case-insensitive substring needle in haystack of given lengths.
 * Returns pointer to the match or NULL.
 */
static const char *memmemCI(const char *haystack, int haystackLen,
                            const char *needle, int needleLen)
{
    if (needleLen > haystackLen)
        return NULL;
    int limit = haystackLen - needleLen;
    for (int i = 0; i <= limit; ++i)
    {
        if (strncasecmpLocal(haystack + i, needle, needleLen) == 0)
            return haystack + i;
    }
    return NULL;
}


EsiParser::EsiParser()
{
    reset();
}


EsiParser::~EsiParser()
{
}


void EsiParser::reset()
{
    m_nodeCount = 0;
    m_tagBufLen = 0;
    m_inTag     = 0;
    m_tryIndex  = -1;
    m_hasEsi    = 0;
    m_inRemove  = 0;
    m_poolUsed  = 0;
}


char *EsiParser::poolDup(const char *src, int len)
{
    if (m_poolUsed + len + 1 > (int)sizeof(m_pool))
        return NULL;
    char *dst = m_pool + m_poolUsed;
    memcpy(dst, src, len);
    dst[len] = '\0';
    m_poolUsed += len + 1;
    return dst;
}


int EsiParser::addNode(EsiNodeType type)
{
    if (m_nodeCount >= ESI_MAX_NODES)
        return -1;
    int idx = m_nodeCount++;
    EsiNode *node       = &m_nodes[idx];
    node->type           = type;
    node->data           = NULL;
    node->dataLen        = 0;
    node->altUrl         = NULL;
    node->altUrlLen      = 0;
    node->onerrorContinue = 0;
    node->parentIndex    = -1;
    return idx;
}


int EsiParser::addTextNode(const char *data, int len)
{
    if (len <= 0)
        return 0;
    // If we are inside an esi:remove block, suppress text
    if (m_inRemove)
        return 0;

    int idx = addNode(ESI_NODE_TEXT);
    if (idx < 0)
        return -1;
    char *copy = poolDup(data, len);
    if (!copy)
        return -1;
    m_nodes[idx].data    = copy;
    m_nodes[idx].dataLen = len;
    return 0;
}


int EsiParser::extractAttr(const char *attrs, int attrsLen,
                           const char *name, int nameLen,
                           const char **valOut, int *valLenOut)
{
    const char *p   = attrs;
    const char *end = attrs + attrsLen;

    while (p < end)
    {
        // Skip whitespace
        while (p < end && isspace((unsigned char)*p))
            ++p;
        if (p >= end)
            break;

        // Read attribute name
        const char *attrName = p;
        while (p < end && *p != '=' && !isspace((unsigned char)*p))
            ++p;
        int attrNameLen = (int)(p - attrName);

        // Skip whitespace around '='
        while (p < end && isspace((unsigned char)*p))
            ++p;
        if (p >= end || *p != '=')
            continue;
        ++p; // skip '='
        while (p < end && isspace((unsigned char)*p))
            ++p;
        if (p >= end)
            break;

        // Read quoted value
        char quote = *p;
        if (quote != '"' && quote != '\'')
        {
            // Unquoted attribute value -- skip to next whitespace
            const char *valStart = p;
            while (p < end && !isspace((unsigned char)*p))
                ++p;
            if (attrNameLen == nameLen
                && strncasecmpLocal(attrName, name, nameLen) == 0)
            {
                *valOut    = valStart;
                *valLenOut = (int)(p - valStart);
                return 0;
            }
            continue;
        }
        ++p; // skip opening quote
        const char *valStart = p;
        while (p < end && *p != quote)
            ++p;
        int valLen = (int)(p - valStart);
        if (p < end)
            ++p; // skip closing quote

        if (attrNameLen == nameLen
            && strncasecmpLocal(attrName, name, nameLen) == 0)
        {
            *valOut    = valStart;
            *valLenOut = valLen;
            return 0;
        }
    }
    return -1;
}


int EsiParser::parseIncludeTag(const char *attrs, int attrsLen)
{
    const char *srcVal  = NULL;
    int         srcLen  = 0;

    if (extractAttr(attrs, attrsLen, "src", 3, &srcVal, &srcLen) != 0
        || srcLen <= 0 || srcLen > ESI_MAX_URL_LEN)
        return -1;

    int idx = addNode(ESI_NODE_INCLUDE);
    if (idx < 0)
        return -1;

    char *srcCopy = poolDup(srcVal, srcLen);
    if (!srcCopy)
        return -1;
    m_nodes[idx].data    = srcCopy;
    m_nodes[idx].dataLen = srcLen;

    // Optional alt attribute
    const char *altVal = NULL;
    int         altLen = 0;
    if (extractAttr(attrs, attrsLen, "alt", 3, &altVal, &altLen) == 0
        && altLen > 0 && altLen <= ESI_MAX_URL_LEN)
    {
        char *altCopy = poolDup(altVal, altLen);
        if (altCopy)
        {
            m_nodes[idx].altUrl    = altCopy;
            m_nodes[idx].altUrlLen = altLen;
        }
    }

    // Optional onerror attribute
    const char *errVal = NULL;
    int         errLen = 0;
    if (extractAttr(attrs, attrsLen, "onerror", 7, &errVal, &errLen) == 0
        && errLen == 8
        && strncasecmpLocal(errVal, "continue", 8) == 0)
    {
        m_nodes[idx].onerrorContinue = 1;
    }

    // If inside a try/attempt, set parent
    if (m_tryIndex >= 0)
        m_nodes[idx].parentIndex = m_tryIndex;

    return 0;
}


int EsiParser::parseCommentTag(const char *attrs, int attrsLen)
{
    int idx = addNode(ESI_NODE_COMMENT);
    if (idx < 0)
        return -1;

    const char *textVal = NULL;
    int         textLen = 0;
    if (extractAttr(attrs, attrsLen, "text", 4, &textVal, &textLen) == 0
        && textLen > 0)
    {
        char *copy = poolDup(textVal, textLen);
        if (copy)
        {
            m_nodes[idx].data    = copy;
            m_nodes[idx].dataLen = textLen;
        }
    }
    return 0;
}


int EsiParser::parseTag()
{
    // m_tagBuf contains the full tag from '<' to '>' inclusive.
    // Null-terminate for safety.
    if (m_tagBufLen <= 0)
        return -1;

    const char *tag    = m_tagBuf;
    int         tagLen = m_tagBufLen;

    // Determine if this is a closing tag: </esi:...>
    int isClose = 0;
    const char *afterPrefix = NULL;
    int afterPrefixLen = 0;

    if (tagLen > 6 && tag[0] == '<' && tag[1] == '/')
    {
        // Closing tag: </esi:xxx>
        if (strncasecmpLocal(tag + 2, "esi:", 4) != 0)
            return -1;
        isClose = 1;
        afterPrefix    = tag + 6; // after "</esi:"
        afterPrefixLen = tagLen - 6;
        // Strip trailing '>'
        if (afterPrefixLen > 0 && afterPrefix[afterPrefixLen - 1] == '>')
            --afterPrefixLen;
    }
    else if (tagLen > s_esiPrefixLen && tag[0] == '<')
    {
        // Opening tag: <esi:xxx ...> or <esi:xxx .../>
        if (strncasecmpLocal(tag + 1, "esi:", 4) != 0)
            return -1;
        afterPrefix    = tag + s_esiPrefixLen;
        afterPrefixLen = tagLen - s_esiPrefixLen;
        // Strip trailing '>' or '/>'
        if (afterPrefixLen > 0 && afterPrefix[afterPrefixLen - 1] == '>')
        {
            --afterPrefixLen;
            if (afterPrefixLen > 0 && afterPrefix[afterPrefixLen - 1] == '/')
                --afterPrefixLen;
        }
    }
    else
        return -1;

    if (afterPrefixLen <= 0)
        return -1;

    m_hasEsi = 1;

    // Identify the tag name (first word in afterPrefix)
    const char *tagName = afterPrefix;
    int tagNameLen = 0;
    while (tagNameLen < afterPrefixLen
           && !isspace((unsigned char)afterPrefix[tagNameLen]))
        ++tagNameLen;

    // Attributes start after the tag name
    const char *attrs    = afterPrefix + tagNameLen;
    int         attrsLen = afterPrefixLen - tagNameLen;
    while (attrsLen > 0 && isspace((unsigned char)*attrs))
    {
        ++attrs;
        --attrsLen;
    }

    // --- esi:include ---
    if (tagNameLen == s_esiIncludeLen
        && strncasecmpLocal(tagName, s_esiInclude, s_esiIncludeLen) == 0
        && !isClose)
    {
        return parseIncludeTag(attrs, attrsLen);
    }

    // --- esi:remove ---
    if (tagNameLen == s_esiRemoveLen
        && strncasecmpLocal(tagName, s_esiRemove, s_esiRemoveLen) == 0)
    {
        if (isClose)
        {
            m_inRemove = 0;
            // Add a REMOVE node to mark end
            addNode(ESI_NODE_REMOVE);
            return 0;
        }
        else
        {
            m_inRemove = 1;
            addNode(ESI_NODE_REMOVE);
            return 0;
        }
    }

    // --- esi:comment ---
    if (tagNameLen == s_esiCommentLen
        && strncasecmpLocal(tagName, s_esiComment, s_esiCommentLen) == 0
        && !isClose)
    {
        return parseCommentTag(attrs, attrsLen);
    }

    // --- esi:try ---
    if (tagNameLen == s_esiTryLen
        && strncasecmpLocal(tagName, s_esiTry, s_esiTryLen) == 0)
    {
        if (isClose)
        {
            m_tryIndex = -1;
            return 0;
        }
        else
        {
            int idx = addNode(ESI_NODE_TRY);
            if (idx < 0)
                return -1;
            m_tryIndex = idx;
            return 0;
        }
    }

    // --- esi:attempt ---
    if (tagNameLen == s_esiAttemptLen
        && strncasecmpLocal(tagName, s_esiAttempt, s_esiAttemptLen) == 0)
    {
        if (isClose)
            return 0; // nothing to do
        int idx = addNode(ESI_NODE_ATTEMPT);
        if (idx < 0)
            return -1;
        m_nodes[idx].parentIndex = m_tryIndex;
        return 0;
    }

    // --- esi:except ---
    if (tagNameLen == s_esiExceptLen
        && strncasecmpLocal(tagName, s_esiExcept, s_esiExceptLen) == 0)
    {
        if (isClose)
            return 0;
        int idx = addNode(ESI_NODE_EXCEPT);
        if (idx < 0)
            return -1;
        m_nodes[idx].parentIndex = m_tryIndex;
        return 0;
    }

    // Unknown ESI tag -- ignore gracefully
    return 0;
}


int EsiParser::feed(const char *data, int len)
{
    if (!data || len <= 0)
        return 0;

    const char *p           = data;
    const char *end         = data + len;
    const char *textStart   = data;  // Start of current non-tag text region

    while (p < end)
    {
        if (m_inTag)
        {
            // We are accumulating bytes of an ESI tag.
            // Look for the closing '>'.
            while (p < end)
            {
                if (m_tagBufLen >= ESI_MAX_TAG_LEN - 1)
                {
                    // Tag too long -- treat the buffered content as text
                    if (addTextNode(m_tagBuf, m_tagBufLen) < 0)
                        return -1;
                    m_tagBufLen = 0;
                    m_inTag     = 0;
                    textStart   = p;
                    break;
                }
                m_tagBuf[m_tagBufLen++] = *p;
                if (*p == '>')
                {
                    m_tagBuf[m_tagBufLen] = '\0';
                    // Parse the complete tag
                    parseTag();
                    m_tagBufLen = 0;
                    m_inTag     = 0;
                    ++p;
                    textStart = p;
                    break;
                }
                ++p;
            }
            continue;
        }

        // Not inside a tag -- scan for '<esi:' or '</esi:'
        const char *found = NULL;
        const char *scan  = p;
        while (scan < end)
        {
            if (*scan == '<')
            {
                // Check if this could be an ESI tag opening
                int remaining = (int)(end - scan);

                // Check for </esi: (closing tags)
                if (remaining >= 6
                    && scan[1] == '/'
                    && strncasecmpLocal(scan + 2, "esi:", 4) == 0)
                {
                    found = scan;
                    break;
                }

                // Check for <esi: (opening tags)
                if (remaining >= s_esiPrefixLen
                    && strncasecmpLocal(scan + 1, "esi:", 4) == 0)
                {
                    found = scan;
                    break;
                }

                // Partial match at the end of buffer: could be <, <e, <es,
                // <esi, <esi: at the boundary. Check if this is a partial
                // prefix at the very end.
                if (remaining < s_esiPrefixLen && remaining < 7)
                {
                    // Could be a partial "<esi:" or "</esi:" -- check
                    const char *prefixes[] = { "<esi:", "</esi:" };
                    int prefLens[]         = { 5, 6 };
                    int isPartial = 0;
                    for (int pi = 0; pi < 2; ++pi)
                    {
                        if (remaining <= prefLens[pi]
                            && strncasecmpLocal(scan, prefixes[pi],
                                                remaining) == 0)
                        {
                            isPartial = 1;
                            break;
                        }
                    }
                    if (isPartial)
                    {
                        found = scan;
                        break;
                    }
                }
            }
            ++scan;
        }

        if (found)
        {
            // Emit text before the tag
            if (found > textStart)
            {
                if (addTextNode(textStart, (int)(found - textStart)) < 0)
                    return -1;
            }

            // Check if the full prefix is available
            int remaining = (int)(end - found);
            int prefixNeeded = (remaining >= 2 && found[1] == '/') ? 6 : 5;

            if (remaining < prefixNeeded)
            {
                // Partial prefix at end of buffer -- buffer it
                memcpy(m_tagBuf, found, remaining);
                m_tagBufLen = remaining;
                m_inTag     = 1;
                p = end;
                continue;
            }

            // Full prefix present -- start accumulating the tag
            m_tagBufLen = 0;
            m_inTag     = 1;
            p           = found;
            textStart   = found;
            continue;
        }
        else
        {
            // No ESI tag found in the remainder -- everything is text
            // but only emit up to where we've scanned
            p = end;
        }
    }

    // Any remaining text that hasn't been emitted
    if (!m_inTag && textStart < end)
    {
        if (addTextNode(textStart, (int)(end - textStart)) < 0)
            return -1;
    }

    return 0;
}


int EsiParser::finish()
{
    // If we were in the middle of accumulating a tag, emit it as text
    if (m_inTag && m_tagBufLen > 0)
    {
        if (addTextNode(m_tagBuf, m_tagBufLen) < 0)
            return -1;
        m_tagBufLen = 0;
        m_inTag     = 0;
    }
    return 0;
}
