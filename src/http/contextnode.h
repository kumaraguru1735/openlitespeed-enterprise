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
#ifndef CONTEXTNODE_H
#define CONTEXTNODE_H

#include <util/hashstringmap.h>
#include <lsr/ls_atomic.h>

#define HTA_UNKNOWN         0
#define HTA_NONE            1
#define HTA_EXIST           2

class HttpContext;
class ContextList;
class AutoStr2;
class StringList;
class HttpHandler;
class ContextNode;

class ChildNodeList : public HashStringMap< ContextNode * >
{
public:
    ChildNodeList()
        : HashStringMap< ContextNode * >(7)
    {}
    ~ChildNodeList()
    {   release_objects();  }

};


class ContextNode
{
    ChildNodeList      *m_pChildren;
    ContextNode        *m_pParentNode;
    HttpContext        *m_pContext;
    char               *m_pLabel;
    long                m_lHTALastCheck;
    short               m_iHTAState;
    char                m_isDir;
    char                m_iRelease;

    ContextNode(const ContextNode &rhs);
    void operator=(const ContextNode &rhs);

public:

    ContextNode(const char *pchLabel, ContextNode *pParentNode);

    const HttpContext *getContext() const
    {
        HttpContext *p;
        ls_atomic_load(p, const_cast<HttpContext **>(&m_pContext));
        return p;
    }
    HttpContext *getContext()
    {
        HttpContext *p;
        ls_atomic_load(p, &m_pContext);
        return p;
    }
    ContextNode *getParentNode() const      {   return m_pParentNode ;  }
    ChildNodeList *getChildren() const      {   return m_pChildren;     }
    HttpContext *getParentContext();
    void setChildrenParentContext(const HttpContext *pOldParent,
                                  const HttpContext *pNewParent, int noUpdateRedirect);
    const char *getLabel() const            {   return m_pLabel ;       }
    void setLabel(const char *l);

    void contextInherit(const HttpContext *pRootContext);

    void setRelease(char r)                 {   m_iRelease = r;         }
    char getRelease() const                 {   return m_iRelease;      }

    void setIsDir(char dir)                 {   m_isDir = dir;          }
    char isDir() const                      {   return m_isDir;         }

    void setHTAState(char state)            {   m_iHTAState = state;    }
    short getHTAState()  const              {   return m_iHTAState;     }

    long getLastCheck() const               {   return m_lHTALastCheck; }
    void setLastCheck(long check)           {   m_lHTALastCheck = check;}

    void setContext(HttpContext *pContext)
    {   ls_atomic_store(&m_pContext, pContext);  }

    // Atomically swap context pointer. Returns true if swap succeeded
    // (this thread won the race). On success, caller must recycleContext(pOld).
    // On failure, another thread already swapped; caller should delete pNew.
    bool casSwapContext(HttpContext *pOld, HttpContext *pNew)
    {   return ls_atomic_casptr(&m_pContext, pOld, pNew);   }

    // Atomic CAS on m_lHTALastCheck for per-node throttling.
    // Returns true if this thread claimed the check window.
    bool casLastCheck(long expected, long desired)
    {   return ls_atomic_caslong(&m_lHTALastCheck, expected, desired);  }
    void setContextUpdateParent(HttpContext *pContext, int noUpdateRedirect);
    ContextNode *insertChild(const char *pchLabel);
    ContextNode *match(const char *pStart);
    ContextNode *find(const char *pStart);

    ContextNode()
        : m_pLabel(NULL)
        , m_lHTALastCheck(0)
        , m_iHTAState(HTA_UNKNOWN)
        , m_isDir(0)
        , m_iRelease(0)
    {}

    ~ContextNode();

    ContextNode *findChild(const char *pStart)
    {
        ChildNodeList::iterator iter ;
        if (m_pChildren)
        {
            iter = m_pChildren->find(pStart);
            if (iter != m_pChildren->end())
                return iter.second();
        }
        return NULL;
    }
    void removeChild(ContextNode *pChild);

    ContextNode *getChildNode(const char *pStart, long lastCheck = 0)
    {

        ContextNode *pNode = findChild(pStart);
        if (pNode)
            return pNode;
        pNode = insertChild(pStart);
        if (pNode && lastCheck > 0)
            pNode->setLastCheck(lastCheck);
        return pNode;
    }
    //void getAllContexts( ContextList &list );
    const HttpContext *matchIndexes(
        const StringList *pIndexList, AutoStr2 *&pIdx);
};

#endif
