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
#ifndef ESI_PARSER_H
#define ESI_PARSER_H

#include <stddef.h>
#include <stdint.h>


/**
 * ESI tag types recognized by the parser.
 */
enum EsiNodeType
{
    ESI_NODE_TEXT = 0,       // Plain text (non-ESI content)
    ESI_NODE_INCLUDE,        // <esi:include src="..." />
    ESI_NODE_REMOVE,         // <esi:remove>...</esi:remove>
    ESI_NODE_COMMENT,        // <esi:comment text="..." />
    ESI_NODE_TRY,            // <esi:try> container
    ESI_NODE_ATTEMPT,        // <esi:attempt> block
    ESI_NODE_EXCEPT,         // <esi:except> block
};


/**
 * Represents a single parsed ESI node.
 * Nodes form a flat list; try/attempt/except nesting is indicated
 * by parent_index references.
 */
struct EsiNode
{
    EsiNodeType     type;

    /**
     * For TEXT nodes: pointer into the original buffer for the literal text.
     * For INCLUDE nodes: the src URL extracted from the tag.
     * For COMMENT nodes: the comment text attribute.
     * For REMOVE/TRY/ATTEMPT/EXCEPT: NULL (content is suppressed or
     * represented by child nodes).
     */
    const char     *data;
    int             dataLen;

    /**
     * For INCLUDE: alt URL (fallback), or NULL if not specified.
     */
    const char     *altUrl;
    int             altUrlLen;

    /**
     * For INCLUDE: onerror="continue" flag.
     */
    int             onerrorContinue;

    /**
     * Index of the parent TRY node for ATTEMPT/EXCEPT nodes, -1 otherwise.
     */
    int             parentIndex;
};


/**
 * Maximum number of ESI nodes that can be parsed from a single response.
 * This prevents unbounded memory growth from malicious input.
 */
#define ESI_MAX_NODES       1024

/**
 * Maximum length of a single ESI tag (including attributes).
 */
#define ESI_MAX_TAG_LEN     8192

/**
 * Maximum URL length for esi:include src/alt attributes.
 */
#define ESI_MAX_URL_LEN     4096


/**
 * Parser state for streaming ESI tag detection.
 *
 * The parser is designed to be fed data incrementally. It scans for
 * the "<esi:" prefix and then parses the complete tag. When a partial
 * tag straddles a buffer boundary, the parser buffers the partial tag
 * internally until the next data chunk completes it.
 */
class EsiParser
{
public:
    EsiParser();
    ~EsiParser();

    /**
     * Reset the parser to initial state, clearing all nodes.
     */
    void reset();

    /**
     * Feed a chunk of data to the parser.
     *
     * @param data    Pointer to the input data.
     * @param len     Length of the input data.
     * @return 0 on success, -1 on error (e.g., too many nodes).
     *
     * After calling feed(), the parsed nodes are available via
     * getNodes() / getNodeCount().
     */
    int feed(const char *data, int len);

    /**
     * Signal end of input. Flushes any remaining buffered text
     * as a TEXT node.
     *
     * @return 0 on success, -1 on error.
     */
    int finish();

    /**
     * @return pointer to the array of parsed nodes.
     */
    const EsiNode *getNodes() const     { return m_nodes; }

    /**
     * @return number of parsed nodes.
     */
    int getNodeCount() const            { return m_nodeCount; }

    /**
     * @return non-zero if any ESI tags were found during parsing.
     */
    int hasEsiContent() const           { return m_hasEsi; }

private:
    /**
     * Internal: attempt to parse a complete ESI tag from m_tagBuf.
     * @return 0 on success, -1 on error.
     */
    int parseTag();

    /**
     * Internal: parse an <esi:include .../> tag.
     */
    int parseIncludeTag(const char *attrs, int attrsLen);

    /**
     * Internal: parse an <esi:comment .../> tag.
     */
    int parseCommentTag(const char *attrs, int attrsLen);

    /**
     * Internal: add a text node covering [start, start+len) of data
     * previously seen.
     */
    int addTextNode(const char *data, int len);

    /**
     * Internal: add a node of given type.
     * @return index of the new node, or -1 on overflow.
     */
    int addNode(EsiNodeType type);

    /**
     * Internal: extract the value of a named attribute from an
     * attribute string.
     *
     * @param attrs     The attribute portion of the tag.
     * @param attrsLen  Length of attrs.
     * @param name      Attribute name to search for.
     * @param nameLen   Length of name.
     * @param valOut    [out] Pointer set to the attribute value start.
     * @param valLenOut [out] Length of the attribute value.
     * @return 0 if found, -1 if not found.
     */
    static int extractAttr(const char *attrs, int attrsLen,
                           const char *name, int nameLen,
                           const char **valOut, int *valLenOut);


    EsiNode         m_nodes[ESI_MAX_NODES];
    int             m_nodeCount;

    /**
     * Buffer for accumulating a partial ESI tag that spans
     * chunk boundaries.
     */
    char            m_tagBuf[ESI_MAX_TAG_LEN];
    int             m_tagBufLen;

    /**
     * State machine: are we inside a potential "<esi:" prefix?
     */
    int             m_inTag;

    /**
     * Track nesting for <esi:try> blocks.
     * Index of the current open TRY node, or -1.
     */
    int             m_tryIndex;

    /**
     * Flag: true once we have seen at least one ESI tag.
     */
    int             m_hasEsi;

    /**
     * Track whether we are inside an <esi:remove> block so we
     * can suppress the enclosed content.
     */
    int             m_inRemove;

    /**
     * Storage pool for attribute values (src URLs, etc.) that
     * need to outlive the original input buffers.
     * Simple bump allocator.
     */
    char            m_pool[ESI_MAX_NODES * 256];
    int             m_poolUsed;

    /**
     * Allocate len bytes from the internal pool, copy src into it.
     * @return pointer to the copy, or NULL on overflow.
     */
    char *poolDup(const char *src, int len);
};


#endif // ESI_PARSER_H
